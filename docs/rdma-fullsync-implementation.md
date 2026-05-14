# kvstore RDMA 全量复制实现详解

> 本文档从代码层面逐层剖析 kvstore 中 RDMA 全量复制（fullsync）的完整实现，覆盖建链、握手、数据传输、异常处理全流程。

---

## 目录

1. [整体架构概览](#1-整体架构概览)
2. [核心数据结构](#2-核心数据结构)
3. [RDMA 建链流程（slave 侧）](#3-rdma-建链流程slave-侧)
4. [RDMA 接受连接流程（master 侧）](#4-rdma-接受连接流程master-侧)
5. [全量数据传输流程](#5-全量数据传输流程)
6. [Slave 侧接收与解析](#6-slave-侧接收与解析)
7. [CQ 轮询与并发控制](#7-cq-轮询与并发控制)
8. [异常处理与回退机制](#8-异常处理与回退机制)
9. [混合传输模式（RDMA + eBPF）](#9-混合传输模式rdma--ebpf)
10. [关键代码位置索引](#10-关键代码位置索引)

---

## 1. 整体架构概览

### 1.1 通道架构

RDMA 全量同步使用**独立的 RDMA 连接**进行数据传输，与 TCP 控制链路分离：

```
Master                                    Slave
┌──────────────────────┐                 ┌──────────────────────┐
│  TCP 端口 (5000)     │◄────TCP───────►│  TCP 连接             │
│  (控制命令、实时复制) │   REPLSYNC     │  (接收复制命令)       │
│                      │   REPLACK      │                      │
├──────────────────────┤                 ├──────────────────────┤
│  RDMA 端口 (5001)    │◄──RDMA RC QP──►│  RDMA 连接            │
│  (全量数据传输)       │   FULLRESYNC   │  (接收快照数据)       │
│                      │   快照数据      │                      │
└──────────────────────┘                 └──────────────────────┘
```

**关键点**：
- RDMA 默认使用 `TCP端口 + 1` 作为 RDMA 监听端口
- RDMA 连接仅用于 fullsync（全量快照传输），不承载 REPLSYNC/REPLACK 等控制命令
- 实时增量命令走 TCP（或 eBPF over TCP）通道

### 1.2 传输分层

```
┌─────────────────────────────────────────────┐
│  repl_fullsync_send() / repl_realtime_send()│  ← 发送上下文选择
├─────────────────────────────────────────────┤
│  repl_transport_ops_t (策略模式)            │
│  ├── tcp_ops  → send()/recv()               │
│  ├── rdma_ops → repl_rdma_try_send()        │
│  └── ebpf_ops → eBPF over TCP               │
├─────────────────────────────────────────────┤
│  repl_rdma_ctx_t (全局单例)                  │  ← RDMA 资源管理
│  ├── rdma_cm_id / ibv_pd / ibv_cq           │
│  ├── send_buf + send_mr                     │
│  └── recv_slots[] (环形队列)                 │
└─────────────────────────────────────────────┘
```

---

## 2. 核心数据结构

所有 RDMA 相关代码位于 `src/replication/kvs_repl.c`，由 `#if KVS_ENABLE_RDMA` 条件编译控制。

### 2.1 RDMA 上下文 `repl_rdma_ctx_t`

```c
// 文件: src/replication/kvs_repl.c, ~行70-120

typedef struct repl_rdma_ctx_s {
    // ---- RDMA CM (连接管理) ----
    struct rdma_event_channel *ec;    // 事件通道（异步事件通知）
    struct rdma_cm_id *id;            // 当前活跃连接 ID
    struct rdma_cm_id *listen_id;     // master 监听 ID
    struct rdma_cm_id *accepted_id;   // master accept 的 ID

    // ---- Verbs 资源 ----
    struct ibv_pd *pd;                // 保护域
    struct ibv_cq *cq;                // 完成队列（发送+接收共用）
    struct ibv_comp_channel *comp_chan; // 完成通道

    // ---- 发送缓冲区 ----
    struct ibv_mr *send_mr;           // 发送内存注册
    unsigned char *send_buf;          // 发送缓冲区
    size_t send_buf_cap;              // 发送缓冲区容量

    // ---- 接收缓冲区（多槽位环形队列）----
    repl_rdma_recv_slot_t recv_slots[KVS_RDMA_RECV_SLOTS_MAX]; // 最多64
    size_t recv_buf_cap;

    // ---- Pending Recv 环形队列 ----
    // 解决 send CQ 等待期间多个 recv completion 被覆盖的问题
    int pending_recv_slots[KVS_RDMA_RECV_SLOTS_MAX];
    size_t pending_recv_lens[KVS_RDMA_RECV_SLOTS_MAX];
    int pending_recv_head;
    int pending_recv_tail;
    int pending_recv_count;

    // ---- 运行时配置（可在运行时调整）----
    int active_recv_slots;            // 当前生效的 recv slot 数
    int active_qp_wr_depth;           // 当前生效的 QP WR 深度
    size_t active_chunk_size;         // 当前生效的分块大小

    // ---- 状态标记 ----
    int addr_resolved;                // 地址解析完成
    int route_resolved;               // 路由解析完成
    int qp_ready;                     // QP 创建完成
    int connected;                    // 连接已建立
    repl_rdma_state_t state;          // 状态机状态
} repl_rdma_ctx_t;

static repl_rdma_ctx_t g_repl_rdma_ctx = {0};  // 全局单例
```

### 2.2 接收槽位 `repl_rdma_recv_slot_t`

```c
typedef struct repl_rdma_recv_slot_s {
    struct ibv_mr *mr;      // 内存注册
    unsigned char *buf;     // 接收缓冲区
    size_t cap;             // 缓冲区容量
    int posted;             // 是否已 post 到 QP
} repl_rdma_recv_slot_t;
```

### 2.3 RDMA 状态机

```c
typedef enum repl_rdma_state_e {
    REPL_RDMA_STATE_INIT = 0,        // 初始态
    REPL_RDMA_STATE_CONNECTING,      // 正在连接
    REPL_RDMA_STATE_ESTABLISHED,     // 连接已建立
    REPL_RDMA_STATE_SYNCING,         // 正在同步
    REPL_RDMA_STATE_STEADY,          // 稳态运行
    REPL_RDMA_STATE_BACKOFF,         // 退避重试
    REPL_RDMA_STATE_FAILED,          // 失败
    REPL_RDMA_STATE_FALLBACK_TCP,    // 已回退到 TCP
} repl_rdma_state_t;
```

### 2.4 传输操作抽象 `repl_transport_ops_t`

```c
// 策略模式：统一 TCP / RDMA / eBPF 三种传输的接口
typedef struct repl_transport_ops_s {
    const char *name;
    int supported;
    int (*send)(conn_t *c, const unsigned char *buf, size_t len);
    int (*connect_slave)(const char *host, int port);
    void (*disconnect_slave)(int fd);
} repl_transport_ops_t;

// 三个具体实现
static const repl_transport_ops_t g_repl_transport_tcp_ops  = { .name = "tcp", ... };
static const repl_transport_ops_t g_repl_transport_rdma_ops = { .name = "rdma", .supported = KVS_ENABLE_RDMA, ... };
static const repl_transport_ops_t g_repl_transport_ebpf_ops = { .name = "ebpf", ... };
```

---

## 3. RDMA 建链流程（slave 侧）

### 3.1 入口：`repl_transport_rdma_connect_slave()`

```c
// 文件: src/replication/kvs_repl.c, ~行750
static int repl_transport_rdma_connect_slave(const char *host, int port) {
    struct sockaddr_in dst;
    int rdma_port = g_cfg.rdma_port > 0 ? g_cfg.rdma_port : port + 1;

    // Step 1: 准备目标地址（处理 loopback 映射）
    repl_rdma_prepare_addr(host, rdma_port, &dst);

    // Step 2: 重置上下文
    repl_rdma_reset_ctx();

    // Step 3: 创建事件通道（带重试，最多3次）
    for (int ec_retry = 0; ec_retry < 3; ec_retry++) {
        g_repl_rdma_ctx.ec = rdma_create_event_channel();
        if (g_repl_rdma_ctx.ec) break;
        if (ec_retry < 2) usleep(200000);
    }

    // Step 4: 创建 RDMA CM ID
    rdma_create_id(g_repl_rdma_ctx.ec, &g_repl_rdma_ctx.id, NULL, RDMA_PS_TCP);

    // Step 5: 地址解析
    rdma_resolve_addr(g_repl_rdma_ctx.id, NULL, (struct sockaddr *)&dst, 1000);
    repl_rdma_wait_event(RDMA_CM_EVENT_ADDR_RESOLVED, 1500);
    g_repl_rdma_ctx.addr_resolved = 1;

    // Step 6: 路由解析
    rdma_resolve_route(g_repl_rdma_ctx.id, 1000);
    repl_rdma_wait_event(RDMA_CM_EVENT_ROUTE_RESOLVED, 1500);
    g_repl_rdma_ctx.route_resolved = 1;

    // Step 7: 创建 Verbs 资源
    g_repl_rdma_ctx.comp_chan = ibv_create_comp_channel(g_repl_rdma_ctx.id->verbs);
    g_repl_rdma_ctx.pd = ibv_alloc_pd(g_repl_rdma_ctx.id->verbs);
    g_repl_rdma_ctx.cq = ibv_create_cq(g_repl_rdma_ctx.id->verbs, ...);
    repl_rdma_create_qp();       // 创建 QP（Reliable Connection）
    repl_rdma_prepare_buffers(); // 分配 + 注册 send_buf 和 recv_slots
    repl_rdma_post_initial_recv(); // 预 post 所有 recv WR

    // Step 8: 发起连接
    repl_rdma_connect_handshake(); // rdma_connect + wait ESTABLISHED
    repl_transport_mark_active("rdma");
    return 1;  // 返回 1 表示 RDMA 连接成功
}
```

### 3.2 QP 创建：`repl_rdma_create_qp()`

```c
static int repl_rdma_create_qp(void) {
    struct ibv_qp_init_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.send_cq = g_repl_rdma_ctx.cq;    // 发送和接收共用同一个 CQ
    attr.recv_cq = g_repl_rdma_ctx.cq;
    attr.qp_type = IBV_QPT_RC;            // Reliable Connection
    attr.cap.max_send_wr = active_qp_wr_depth;  // 默认 64
    attr.cap.max_recv_wr = active_qp_wr_depth;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;
    rdma_create_qp(g_repl_rdma_ctx.id, g_repl_rdma_ctx.pd, &attr);
}
```

### 3.3 缓冲区准备：`repl_rdma_prepare_buffers()`

```c
static int repl_rdma_prepare_buffers(void) {
    const size_t cap = BUFFER_CAP;  // 65536

    // 发送缓冲区：1 个大 buffer，注册 MR
    g_repl_rdma_ctx.send_buf = kvs_malloc(cap);
    g_repl_rdma_ctx.send_mr = ibv_reg_mr(pd, send_buf, cap, IBV_ACCESS_LOCAL_WRITE);

    // 接收缓冲区：active_recv_slots 个 buffer（默认 32），每个 64KB
    for (i = 0; i < active_recv_slots; ++i) {
        g_repl_rdma_ctx.recv_slots[i].buf = kvs_malloc(cap);
        g_repl_rdma_ctx.recv_slots[i].mr = ibv_reg_mr(pd, recv_slots[i].buf, cap,
                                                        IBV_ACCESS_LOCAL_WRITE);
    }
    return 0;
}
```

### 3.4 预 Post Recv：`repl_rdma_post_initial_recv()`

```c
static int repl_rdma_post_initial_recv(void) {
    // 为所有 recv_slot 预 post recv WR
    // 这样对端发送数据时，HCA 可以直接写入这些预注册的缓冲区
    for (slot = 0; slot < active_recv_slots; ++slot) {
        repl_rdma_post_recv_slot(slot);  // ibv_post_recv()
    }
}
```

---

## 4. RDMA 接受连接流程（master 侧）

### 4.1 入口：`rdma_master_listener_thread()`

```c
// 文件: src/replication/kvs_repl.c, ~行1940
static void *rdma_master_listener_thread(void *arg) {
    for (;;) {
        // 检查角色和配置：非 master 或未配置 RDMA 则睡眠
        if (g_cfg.role != ROLE_MASTER || !repl_should_use_rdma_now()) {
            sleep(1); continue;
        }

        // Step 1: 创建事件通道（带重试，最多5次）
        g_repl_rdma_ctx.ec = rdma_create_event_channel();

        // Step 2: 创建 listen_id，bind + listen
        rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP);
        addr.sin_port = htons(g_cfg.rdma_port > 0 ? g_cfg.rdma_port : g_cfg.port + 1);
        rdma_bind_addr(listen_id, &addr);
        rdma_listen(listen_id, 4);  // backlog = 4

        // Step 3: 等待 CONNECT_REQUEST 事件
        rdma_get_cm_event(ec, &event);  // 阻塞等待
        // 期望 RDMA_CM_EVENT_CONNECT_REQUEST

        // Step 4: 在 accept 的 ID 上创建资源
        g_repl_rdma_ctx.accepted_id = event->id;
        g_repl_rdma_ctx.id = accepted_id;
        ibv_create_comp_channel(id->verbs);
        ibv_alloc_pd(id->verbs);
        ibv_create_cq(id->verbs, ...);
        repl_rdma_create_qp();
        repl_rdma_prepare_buffers();
        repl_rdma_post_initial_recv();

        // Step 5: 接受连接
        rdma_accept(id, &param);
        repl_rdma_wait_event(RDMA_CM_EVENT_ESTABLISHED, 5000);
        g_repl_rdma_ctx.connected = 1;

        // Step 6: 进入接收循环（见第6节）
        for (;;) {
            // 接收 REPLSYNC / REPLACK 等控制命令
            // ...
        }

        // Step 7: 连接断开后重置（保留 listener 资源）
        repl_rdma_reset_conn_ctx(1);  // preserve_listener=1
    }
}
```

### 4.2 失败回退策略

```c
#define LISTENER_FAIL(step_name) do { \
    consecutive_failures++; \
    if (consecutive_failures >= 10) { \
        /* 累计失败 10 次，回退到 TCP 10 秒 */ \
        repl_transport_trigger_fallback("rdma_listener_fail", 10000); \
        repl_rdma_reset_ctx(); \
        consecutive_failures = 0; \
        sleep(3); \
    } else { \
        repl_rdma_reset_conn_ctx(1); \
        usleep(500000); /* 500ms 后重试 */ \
    } \
    continue; \
} while(0)
```

---

## 5. 全量数据传输流程

### 5.1 触发点：master 收到 `REPLSYNC` 命令

```c
// 文件: src/main/kvstore.c, ~行957
if (!strcmp(cmd, "REPLSYNC")) {
    const char *req_replid = argv[1];
    unsigned long long req_offset = strtoull(argv[2]);
    unsigned long long req_durable = strtoull(argv[3]);

    // 检查是否可以 partial resync（走 backlog）
    int can_continue = repl_backlog_can_continue(req_replid, req_offset);

    // 注册 slave 到 replica 链表
    repl_add_slave(c);

    if (can_continue) {
        // 走 partial resync：发送 CONTINUE + backlog 数据
        repl_backlog_send_continue(c, req_offset);
    } else {
        // 走全量同步：生成快照并发送
        c->repl_fullsync_pending = 1;
        queue_snapshot(c);  // ← 核心函数
    }
}
```

### 5.2 快照生成与发送：`queue_snapshot()`

```c
// 文件: src/main/kvstore.c, ~行477
static int queue_snapshot(conn_t *c) {
    // Step 1: 生成全量快照到临时文件
    snprintf(tmp_path, sizeof(tmp_path), "%s.fullsync.tmp.%ld",
             g_cfg.dump_path, (long)getpid());
    fp = fopen(tmp_path, "wb");
    kvs_snapshot_to_fp(fp);  // 遍历所有引擎，序列化到文件
    fclose(fp);

    // Step 2: 计算快照总大小
    fp = fopen(tmp_path, "rb");
    while ((r = fread(buf, 1, sizeof(buf), fp)) > 0) total_bytes += r;
    fclose(fp);

    // Step 3: 发送 FULLRESYNC 头
    // 格式: +FULLRESYNC <replid> <offset> <total_bytes>\r\n
    snprintf(hdr, sizeof(hdr), "+FULLRESYNC %s %llu %zu\r\n",
             repl_master_id(), repl_master_offset(), total_bytes);
    repl_send_chunked(c, hdr, n);  // ← 分块发送，走 RDMA 或 TCP

    // Step 4: 分块发送快照数据
    fp = fopen(tmp_path, "rb");
    while ((r = fread(buf, 1, sizeof(buf), fp)) > 0) {
        // repl_send_chunked 内部根据 fullsync_transport 选择 RDMA 或 TCP
        repl_send_chunked(c, buf, r);
    }
    fclose(fp);
    unlink(tmp_path);  // 清理临时文件

    // Step 5: 发送 REPLDONE 标记
    size_t done = resp_build_cmd1(buf, sizeof(buf), "REPLDONE");
    repl_send_chunked(c, buf, done);

    c->repl_fullsync_pending = 0;
}
```

### 5.3 分块发送：`repl_send_chunked_ctx()`

```c
// 文件: src/main/kvstore.c, ~行456
int repl_send_chunked_ctx(conn_t *c, const unsigned char *buf,
                          size_t len, int send_ctx) {
    size_t off = 0;
    // 关键：RDMA 模式下使用配置的 chunk_size（默认 16KB），TCP 则一次性发送
    size_t chunk_cap = !strcasecmp(repl_fullsync_transport_name(), "rdma")
        ? g_cfg.rdma_chunk_size   // RDMA: 分块，默认 BUFFER_CAP/4 = 16KB
        : len;                     // TCP:  整块发送

    while (off < len) {
        size_t chunk = len - off;
        if (chunk > chunk_cap) chunk = chunk_cap;

        if (send_ctx == KVS_REPL_SEND_FULLSYNC) {
            repl_fullsync_send(c, buf + off, chunk);  // → 选 RDMA 或 TCP
        } else {
            repl_realtime_send(c, buf + off, chunk);  // → 选 eBPF 或 TCP
        }
        off += chunk;
    }
}
```

### 5.4 传输选择：`repl_fullsync_send()`

```c
// 文件: src/replication/kvs_repl.c, ~行1060
int repl_fullsync_send(conn_t *c, const unsigned char *buf, size_t len) {
    // Step 1: 根据 fullsync_transport 配置选择传输
    const repl_transport_ops_t *ops = repl_transport_ops_for_context(KVS_REPL_SEND_FULLSYNC);
    // 如果配置了 rdma → 返回 &g_repl_transport_rdma_ops
    // 否则 → 返回 &g_repl_transport_tcp_ops

    int rc = ops->send(c, buf, len);
    if (rc == 0) return 0;

    // Step 2: 失败回退 → 尝试 TCP
    rc = repl_transport_tcp_send(c, buf, len);
    return rc;
}
```

### 5.5 RDMA 发送：`repl_rdma_try_send()`

```c
// 文件: src/replication/kvs_repl.c, ~行640
static int repl_rdma_try_send(const unsigned char *buf, size_t len) {
    struct ibv_sge sge;
    struct ibv_send_wr wr;
    struct ibv_send_wr *bad_wr = NULL;

    pthread_mutex_lock(&g_repl_rdma_send_lock);  // 发送锁

    // 检查连接状态，排空异步 CM 事件
    if (repl_rdma_drain_cm_events_nonblock() != 0) { /* 断开 */ }
    if (!g_repl_rdma_ctx.connected) { /* 断开 */ }

    // 拷贝数据到预注册的 send_buf
    memcpy(g_repl_rdma_ctx.send_buf, buf, len);

    // 构建 SGE（Scatter-Gather Element）
    sge.addr = (uintptr_t)g_repl_rdma_ctx.send_buf;
    sge.length = (uint32_t)len;
    sge.lkey = g_repl_rdma_ctx.send_mr->lkey;

    // 构建 Send WR（Work Request）
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;           // Send 操作（非 RDMA Write）
    wr.send_flags = IBV_SEND_SIGNALED; // 要求 CQ 通知

    // Post Send 到 QP
    ibv_post_send(g_repl_rdma_ctx.id->qp, &wr, &bad_wr);

    // 同步等待 Send Completion（超时 1 秒）
    rc = repl_rdma_wait_cq_send_completion(1000);

    pthread_mutex_unlock(&g_repl_rdma_send_lock);
    return rc;
}
```

**设计要点**：
- 使用 `IBV_WR_SEND`（双边 send/recv 语义），而非 RDMA Write（单边）
- 同步等待每次 Send 的 CQ completion 后才返回
- 有全局 `g_repl_rdma_send_lock` 互斥锁，确保发送串行化

---

## 6. Slave 侧接收与解析

### 6.1 混合模式下的接收循环

```c
// 文件: src/replication/kvs_repl.c, ~行1660
// 在 slave_thread() 的混合模式（RDMA fullsync + eBPF realtime）分支中：

g_slave_fd = tcp_fd;  // TCP 是主链路
unsigned char buf[BUFFER_CAP];
size_t blen = 0;

for (;;) {
    // --- 处理 TCP 通道（FULLRESYNC 头 + 实时命令）---
    ssize_t r = recv(tcp_fd, buf + blen, sizeof(buf) - blen, 0);
    if (r > 0) { blen += r; had_new_data = 1; }

    // --- 处理 RDMA 通道（快照数据）---
    #if KVS_ENABLE_RDMA
    if (g_repl_rdma_ctx.connected) {
        int recv_slot = -1;
        size_t rdma_blen = 0;
        // 非阻塞轮询 RDMA recv completion（超时 100ms）
        if (repl_rdma_wait_cq_recv_completion(100, &recv_slot, &rdma_blen) == 0) {
            // 拷贝 payload → 追加到统一缓冲区
            payload = repl_rdma_dup_recv_payload(recv_slot, rdma_blen);
            repl_rdma_repost_recv(recv_slot);  // 立即 repost recv
            memcpy(buf + blen, payload, rdma_blen);
            blen += rdma_blen;
        }
    }
    #endif

    // --- 统一解析 ---
    if (had_new_data && blen > 0) {
        parse_resp_stream(NULL, buf, &blen, 1);  // from_replication=1
    }
}
```

### 6.2 FULLRESYNC 解析：`parse_resp_stream()`

```c
// 文件: src/main/kvstore.c, ~行1608
int parse_resp_stream(conn_t *c, unsigned char *buf, size_t *len,
                      int from_replication) {
    // ...遍历缓冲区...

    // 解析 Simple String（+ 开头）
    if (buf[pos] == '+') {
        // 解析行内容
        char *argv[8];
        int argc = split_inline_argv(line, argv, 8);

        if (argc >= 3 && !strcmp(argv[0], "FULLRESYNC")) {
            // 格式: +FULLRESYNC <replid> <offset> <total_bytes>
            unsigned long long fullsync_target = strtoull(argv[3]);

            // 设置 slave 同步状态：标记正在加载 fullsync
            repl_slave_set_sync_state(
                argv[1],                    // replid
                strtoull(argv[2]),          // offset
                strtoull(argv[2]),          // durable_offset
                1,                          // fullsync_loading = 1
                fullsync_target             // 目标字节数
            );
        }

        if (argc >= 3 && !strcmp(argv[0], "CONTINUE")) {
            // 部分同步路径
            repl_slave_set_sync_state(argv[1], start_offset, durable_start, 0, 0);
        }
    }

    // ...解析 RESP 数组（* 开头）...
    // 每条 RESP 命令通过 handle_parsed_command 处理
    // 包含 REPLDONE 命令
}
```

### 6.3 快照数据加载追踪

```c
// 文件: src/replication/kvs_repl.c, ~行1350
void repl_slave_note_applied(size_t rawlen) {
    if (!g_slave_loading_fullsync) {
        // 正常复制：累加 applied offset
        g_slave_repl_applied_offset += rawlen;
    } else {
        // 正在加载 fullsync：累加已加载字节数
        g_slave_fullsync_loaded_bytes += rawlen;

        // 当加载量达到目标值时，自动完成 fullsync
        if (g_slave_fullsync_target_bytes > 0
            && g_slave_fullsync_loaded_bytes >= g_slave_fullsync_target_bytes) {
            repl_slave_finish_fullsync();  // 清除 loading 标记，发送 ACK
        }
    }
}
```

### 6.4 REPLDONE 处理

```c
// 文件: src/main/kvstore.c, ~行1008
if (!strcmp(cmd, "REPLDONE")) {
    repl_slave_finish_fullsync();  // 显式完成 fullsync
}
```

---

## 7. CQ 轮询与并发控制

### 7.1 CQ 锁

由于发送线程（broadcast）和接收线程（listener）可能并发轮询同一个 CQ，引入了互斥锁：

```c
static pthread_mutex_t g_repl_rdma_cq_lock = PTHREAD_MUTEX_INITIALIZER;
```

所有 `ibv_poll_cq()` 调用都在持锁状态下进行。

### 7.2 Send CQ 等待：`repl_rdma_wait_cq_send_completion()`

```c
static int repl_rdma_wait_cq_send_completion(int timeout_ms) {
    for (;;) {
        pthread_mutex_lock(&g_repl_rdma_cq_lock);
        n = ibv_poll_cq(cq, 1, &wc);

        if (n > 0) {
            if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_SEND) {
                // 目标：等待 send completion
                pthread_mutex_unlock(&g_repl_rdma_cq_lock);
                return 0;
            }
            if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_RECV) {
                // 非目标：收到 recv completion → 存入 pending_recv 队列
                int slot = wc.wr_id - 1;
                g_repl_rdma_ctx.recv_slots[slot].posted = 0;
                repl_rdma_pending_recv_push(slot, wc.byte_len);
                pthread_mutex_unlock(&g_repl_rdma_cq_lock);
                continue;  // 继续轮询
            }
            // 错误状态
            g_repl_rdma_ctx.connected = 0;
            return -1;
        }

        pthread_mutex_unlock(&g_repl_rdma_cq_lock);
        // 检查 CM 事件，检查超时
        if (kvs_now_ms() >= deadline) return -1;
        usleep(1000);  // 1ms 间隔
    }
}
```

**关键设计**：在等待 send completion 的过程中，如果先收到 recv completion，不会丢弃，而是存入 `pending_recv` 环形队列，供后续 `repl_rdma_wait_cq_recv_completion()` 消费。这解决了**单槽覆盖**问题。

### 7.3 Recv CQ 等待：`repl_rdma_wait_cq_recv_completion()`

```c
static int repl_rdma_wait_cq_recv_completion(int timeout_ms,
                                              int *slot_out, size_t *recv_len) {
    // Step 1: 优先从 pending_recv 队列取（由 send_cq 等待时预存）
    pthread_mutex_lock(&g_repl_rdma_cq_lock);
    if (repl_rdma_pending_recv_pop(slot_out, recv_len) == 0) {
        pthread_mutex_unlock(&g_repl_rdma_cq_lock);
        return 0;  // 命中缓存
    }
    pthread_mutex_unlock(&g_repl_rdma_cq_lock);

    // Step 2: 轮询 CQ
    for (;;) {
        pthread_mutex_lock(&g_repl_rdma_cq_lock);
        n = ibv_poll_cq(cq, 1, &wc);
        if (n > 0 && wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_RECV) {
            *slot_out = wc.wr_id - 1;
            *recv_len = wc.byte_len;
            pthread_mutex_unlock(&g_repl_rdma_cq_lock);
            return 0;
        }
        // send completion 被忽略（继续轮询）
        pthread_mutex_unlock(&g_repl_rdma_cq_lock);
        // 检查超时...
        usleep(1000);
    }
}
```

### 7.4 Pending Recv 环形队列

```c
// 容量等于 recv_slots 最大数（64）
static int repl_rdma_pending_recv_push(int slot, size_t len) {
    g_repl_rdma_ctx.pending_recv_slots[tail] = slot;
    g_repl_rdma_ctx.pending_recv_lens[tail] = len;
    tail = (tail + 1) % KVS_RDMA_RECV_SLOTS_MAX;
    g_repl_rdma_ctx.pending_recv_count++;
}

static int repl_rdma_pending_recv_pop(int *slot_out, size_t *len_out) {
    int slot = g_repl_rdma_ctx.pending_recv_slots[head];
    size_t len = g_repl_rdma_ctx.pending_recv_lens[head];
    head = (head + 1) % KVS_RDMA_RECV_SLOTS_MAX;
    g_repl_rdma_ctx.pending_recv_count--;
    // ...
}
```

---

## 8. 异常处理与回退机制

### 8.1 异步 CM 事件处理：`repl_rdma_drain_cm_events_nonblock()`

```c
static int repl_rdma_drain_cm_events_nonblock(void) {
    // 非阻塞轮询 CM 事件通道
    while (poll(&pfd, 1, 0) > 0) {  // timeout=0 非阻塞
        rdma_get_cm_event(ec, &event);

        switch (event->event) {
            case RDMA_CM_EVENT_DISCONNECTED:
            case RDMA_CM_EVENT_REJECTED:
            case RDMA_CM_EVENT_ADDR_ERROR:
            case RDMA_CM_EVENT_ROUTE_ERROR:
            case RDMA_CM_EVENT_CONNECT_ERROR:
            case RDMA_CM_EVENT_UNREACHABLE:
            case RDMA_CM_EVENT_DEVICE_REMOVAL:
            case RDMA_CM_EVENT_TIMEWAIT_EXIT:
                g_repl_rdma_ctx.connected = 0;
                repl_rdma_drop_master_replica_from_list("cm_event_async_disconnect");
                repl_rdma_set_state(REPL_RDMA_STATE_FAILED, ...);
                break;
        }
        rdma_ack_cm_event(event);
    }
}
```

### 8.2 发送失败清理：`repl_handle_replica_send_failure()`

```c
int repl_handle_replica_send_failure(conn_t *c, conn_t **linkp) {
    if (c == &g_rdma_master_replica_conn) {
        // 从全局 replica 链表移除
        repl_remove_slave(c);
        repl_rdma_drop_master_replica_shallow(c);
        // 重置 RDMA 上下文（保留 listener）
        repl_rdma_reset_conn_ctx(1);
        return 1;
    }
    return 0;
}
```

### 8.3 回退到 TCP

```c
static void repl_transport_trigger_fallback(const char *reason, int cooldown_ms) {
    g_repl_transport_fallback_count++;
    snprintf(g_repl_transport_fallback_reason, ..., "%s", reason);
    g_repl_transport_fallback_until_ms = kvs_now_ms() + cooldown_ms;
    repl_transport_mark_active("tcp");
    repl_rdma_set_state(REPL_RDMA_STATE_FALLBACK_TCP, reason);
}
```

回退触发条件：
- RDMA 地址解析失败
- RDMA 路由解析失败
- RDMA 连接握手失败
- RDMA 发送失败
- Master listener 连续失败 10 次

### 8.4 连接断开判定

区分"观察到 disconnect"和"disconnect 影响正确性"：

```c
// 在工具脚本 run_repl_rdma_stress.py 中
rdma_master_async_disconnect_seen      // 是否观察到 disconnect 事件
rdma_master_async_disconnect_impactful // 是否影响了数据正确性
rdma_slave_async_disconnect_seen
rdma_slave_async_disconnect_impactful
```

---

## 9. 混合传输模式（RDMA + eBPF）

### 9.1 配置方式

```conf
# kvstore.conf
repl_fullsync_transport=rdma    # 全量同步走 RDMA
repl_realtime_transport=ebpf    # 实时同步走 eBPF sockmap
```

或者命令行：

```bash
./kvstore --repl-fullsync-transport rdma --repl-realtime-transport ebpf
```

### 9.2 混合模式的启动时序

```
slave 启动
  │
  ├─ 1. 建立 TCP 连接（作为主链路）
  │
  ├─ 2. 后台线程启动 RDMA 连接
  │     └─ repl_rdma_bg_connect_thread()
  │
  ├─ 3. 主线程循环等待 RDMA 就绪 或 超时 5 秒
  │     while (!rdma_connected && elapsed < 5000ms) {
  │         usleep(50000);  // 50ms 间隔
  │     }
  │
  ├─ 4. 发送 REPLSYNC（通过 TCP）
  │     master 收到后触发 queue_snapshot()
  │
  ├─ 5. Master 通过 RDMA 发送 FULLRESYNC 头 + 快照数据
  │     同时 TCP 通道也可能收到 FULLRESYNC 头
  │
  └─ 6. Slave 从 TCP 和 RDMA 两个通道同时读取，统一解析
```

### 9.3 为什么 TCP 先收到 FULLRESYNC 头

```c
// 混合模式下，TCP 优先处理，确保 FULLRESYNC 头先被解析
ssize_t r = recv(tcp_fd, buf + blen, sizeof(buf) - blen, 0);  // TCP
// 然后才检查 RDMA
if (g_repl_rdma_ctx.connected) {
    repl_rdma_wait_cq_recv_completion(100, ...);  // RDMA
}
// 然后统一 parse_resp_stream
```

这样确保了 slave 在收到 RDMA 快照数据块之前，已经通过 TCP 解析了 `FULLRESYNC` 头（包含 target_bytes），从而知道何时快照传输完成。

---

## 10. 关键代码位置索引

| 功能 | 文件 | 行号（约） |
|------|------|-----------|
| RDMA 上下文定义 `repl_rdma_ctx_t` | `src/replication/kvs_repl.c` | 70-120 |
| 全局状态变量 | `src/replication/kvs_repl.c` | 20-60 |
| RDMA 配置读取（运行时） | `src/replication/kvs_repl.c` | 165-185 |
| Pending Recv 环形队列 | `src/replication/kvs_repl.c` | 190-220 |
| RDMA 上下文重置 | `src/replication/kvs_repl.c` | 220-310 |
| CM 事件等待 | `src/replication/kvs_repl.c` | 315-345 |
| CM 事件异步排空 | `src/replication/kvs_repl.c` | 347-395 |
| QP 创建 | `src/replication/kvs_repl.c` | 440-460 |
| 连接握手 | `src/replication/kvs_repl.c` | 462-485 |
| 缓冲区准备 | `src/replication/kvs_repl.c` | 487-530 |
| Post Recv | `src/replication/kvs_repl.c` | 532-565 |
| CQ Send Completion 等待 | `src/replication/kvs_repl.c` | 575-615 |
| CQ Recv Completion 等待 | `src/replication/kvs_repl.c` | 617-650 |
| RDMA Send | `src/replication/kvs_repl.c` | 652-695 |
| Slave RDMA 连接 | `src/replication/kvs_repl.c` | 750-860 |
| Slave 线程主循环 | `src/replication/kvs_repl.c` | 1570-1900 |
| Master Listener 线程 | `src/replication/kvs_repl.c` | 1940-2170 |
| 传输选择 `repl_fullsync_send()` | `src/replication/kvs_repl.c` | 1060-1080 |
| 快照生成 `queue_snapshot()` | `src/main/kvstore.c` | 477-540 |
| 分块发送 `repl_send_chunked_ctx()` | `src/main/kvstore.c` | 456-475 |
| REPLSYNC 命令处理 | `src/main/kvstore.c` | 957-998 |
| FULLRESYNC 解析 | `src/main/kvstore.c` | 1608-1620 |
| REPLDONE 处理 | `src/main/kvstore.c` | 1008-1013 |
| Slave fullsync 状态跟踪 | `src/replication/kvs_repl.c` | 1315-1380 |
| Slave ACK 发送 | `src/replication/kvs_repl.c` | 1400-1440 |
| 混合模式入口 | `src/replication/kvs_repl.c` | 1620-1750 |

---

## 附录：关键设计决策总结

| 决策 | 原因 |
|------|------|
| 使用 IBV_WR_SEND（非 RDMA Write） | 双边语义更可控，slave 可以精确知道数据边界 |
| 同步等待 Send CQ（非异步） | 简化流控，避免发送队列溢出 |
| 共用同一个 CQ | 简化资源管理，通过锁串行化 poll 解决竞争 |
| 多槽位 recv buffer + pending 队列 | 解决 send 等待期间 recv completion 被覆盖的问题 |
| chunk_size=16KB（非整包） | 控制单次 RDMA 发送大小，便于流控和错误恢复 |
| RDMA 端口 = TCP端口 + 1 | 避免端口冲突，简化配置 |
| 混合模式 TCP 优先解析 | 确保 FULLRESYNC 头先于快照数据被解析 |
| 非阻塞 CM 事件排空 | 及时发现异步 disconnect，避免使用已断开的连接 |
