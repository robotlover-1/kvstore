# kprobe + RDMA 增量同步实现详解

> 本文档从代码层面逐层剖析 kvstore 中 kprobe+RDMA 增量同步的完整实现。
> kprobe 透明拦截 TCP send 路径，捕获增量 RESP 数据，通过 BPF ringbuf 传递
> 到用户态转发模块，再由 **RDMA WRITE（单边）** 直接写入 Slave 预置 MR 环形缓冲区。
>
> 全量同步使用独立 RDMA QP（RDMA SEND/RECV），见 `rdma-fullsync-implementation.md`。

---

## 目录

1. [整体架构](#1-整体架构)
2. [详细数据流](#2-详细数据流)
3. [核心数据结构](#3-核心数据结构)
4. [BPF kprobe 程序](#4-bpf-kprobe-程序)
5. [用户态转发模块](#5-用户态转发模块)
6. [Slave 侧处理](#6-slave-侧处理)
7. [建链与 MR 交换流程](#7-建链与-mr-交换流程)
8. [Transport 集成](#8-transport-集成)
9. [关键实现细节](#9-关键实现细节)
10. [调试与观测](#10-调试与观测)
11. [关键代码位置索引](#11-关键代码位置索引)

---

## 1. 整体架构

### 1.1 通道架构

kprobe+RDMA 增量同步使用**独立 RDMA WRITE QP** 进行数据加速，与 TCP 保底链路并行：

```
Master                                    Slave
┌──────────────────────┐                 ┌──────────────────────┐
│  TCP 端口 (5160)     │◄────TCP───────►│  TCP 连接             │
│  (控制命令+数据保底)  │   REPLSYNC     │  (接收复制命令)       │
│                      │   KPROBEMR     │                      │
│                      │   +KPROBERDMA  │                      │
│                      │   HSET/SET     │                      │
├──────────────────────┤                 ├──────────────────────┤
│  RDMA 端口 (5161)    │◄──RDMA SEND───►│  RDMA fullsync QP    │
│  (全量数据传输)       │   FULLRESYNC   │  (接收快照数据)       │
├──────────────────────┤                 ├──────────────────────┤
│  kprobe-rdma 端口    │◄─RDMA WRITE───►│  kprobe-rdma QP       │
│  (5172)              │  (单边写)       │  (预置 MR 环形缓冲区) │
│                      │  数据 → MR     │                      │
│                      │  head → MR     │                      │
└──────────────────────┘                 └──────────────────────┘
```

### 1.2 数据路径全景

```
 ┌─────────────────────────────────────────────────────────────────────┐
 │ Master 虚拟机                                                        │
 │                                                                     │
 │  kvstore Master 进程                                                 │
 │    SET/DEL/... → handle_parsed_command()                            │
 │      ↓ persist_append_raw (AOF)                                      │
 │      ↓ repl_broadcast(raw, rawlen)  ←── 完全不动，走正常 TCP send   │
 │      ↓ repl_realtime_send(c, buf, len)                               │
 │         ├── repl_transport_kprobe_rdma_send()                        │
 │         │   └── (返回 -1，不发送，让 TCP 保底)                        │
 │         └── repl_transport_tcp_send()                               │
 │             └── send(fd, buf, len) — TCP 发送（同时也是 kprobe 源） │
 │                                                                     │
 │  ┌── 内核态 ─────────────────────────────────────────────────────┐  │
 │  │                                                               │  │
 │  │  send() 系统调用触发 tcp_sendmsg(sk, msg, size)               │  │
 │  │    │                                                          │  │
 │  │    ├─→ [正常 TCP 协议栈] ──→ Slave（保底路径）                │  │
 │  │    │                                                          │  │
 │  │    └─→ [kprobe 拦截] PID 匹配 → 读取数据                     │  │
 │  │           │                                                   │  │
 │  │           ├─ ctx->dx = size（总长度）                          │  │
 │  │           ├─ ctx->si = msg（msghdr 内核指针）                  │  │
 │  │           ├─ bpf_probe_read_kernel(msg+40 = iov指针)          │  │
 │  │           ├─ bpf_probe_read_kernel(iov[0])                    │  │
 │  │           ├─ bpf_probe_read_user(iov_base) ← 用户数据          │  │
 │  │           └─ bpf_ringbuf_output([4B len][payload])            │  │
 │  │           │                                                   │  │
 │  │  ┌──────────────────────────────┐                             │  │
 │  │  │ BPF Ring Buffer (1MB)        │  ← 内核→用户态通道           │  │
 │  │  └──────────────────────────────┘                             │  │
 │  └───────────────────┬──────────────────────────────────────────┘  │
 │                      │ 用户态                                       │
 │  ┌───────────────────▼──────────────────────────────────────────┐  │
 │  │ 转发线程 (kprobe_rdma_forward_thread)                         │  │
 │  │  ring_buffer__poll() 消费 ringbuf                             │  │
 │  │                                                              │  │
 │  │  kprobe_ringbuf_cb(data, size)                               │  │
 │  │    1. 检查 g_slave_mr.rkey ≠ 0（MR 已交换）                   │  │
 │  │    2. wr_slot_acquire() → 获取空闲 RDMA WRITE slot            │  │
 │  │    3. wr_submit_data(slot)                                    │  │
 │  │       → RDMA WRITE 数据到 Slave MR 槽位                       │  │
 │  │    4. wr_submit_head(slot)                                    │  │
 │  │       → RDMA WRITE 更新 producer_head（带 FENCE）              │  │
 │  │    5. CQ poll 回收 slot（在后续 wr_slot_acquire 中）           │  │
 │  └──────────────────┬───────────────────────────────────────────┘  │
 │                     │ RDMA WRITE（单边，SiW0 软实现）               │
 │                     │ DMA 直达 Slave 内存，Slave CPU 零参与         │
 └─────────────────────┼─────────────────────────────────────────────┘
                       │ RDMA 网络
                       ▼
 ┌─────────────────────────────────────────────────────────────────────┐
 │ Slave 虚拟机                                                         │
 │                                                                     │
 │  ┌────────────────────────────────────────────────────────────┐    │
 │  │ 预置 MR 环形缓冲区 (IBV_ACCESS_REMOTE_WRITE)                │    │
 │  │ 建链时注册，通过 KPROBEMR 响应将 rkey + addr 告知 Master     │    │
 │  │                                                            │    │
 │  │  ┌──────────┬──────────┬──────────────────────────────┐    │    │
 │  │  │ head(8B) │ tail(8B) │ slot[0] | slot[1] | ...      │    │    │
 │  │  │ RDMA写   │ 本地更新  │ 每槽 512B                    │    │    │
 │  │  └──────────┴──────────┴──────────────────────────────┘    │    │
 │  └──────────────────────┬─────────────────────────────────────┘    │
 │                          │                                         │
 │  ┌───────────────────────▼─────────────────────────────────────┐  │
 │  │ Slave 轮询线程 (kprobe_rdma_slave_poll)                      │  │
 │  │                                                             │  │
 │  │  while (running) {                                          │  │
 │  │    __sync_synchronize();  // 读内存屏障                       │  │
 │  │    head = rb->producer_head;  // Master RDMA WRITE 更新      │  │
 │  │    tail = rb->consumer_tail;                                 │  │
 │  │                                                             │  │
 │  │    while (tail != head) {                                    │  │
 │  │      slot = &rb->slots[tail % 1024]                          │  │
 │  │      len = *(uint32_t *)slot;  // 前 4 字节为长度              │  │
 │  │      data = slot + 4;                                        │  │
 │  │      parse_resp_stream(NULL, data, &len, 1);                 │  │
 │  │      → handle_parsed_command(from_replication=1)             │  │
 │  │      → 写入本地引擎 + AOF                                    │  │
 │  │      tail++;                                                 │  │
 │  │    }                                                         │  │
 │  │    rb->consumer_tail = tail;  // 本地更新                    │  │
 │  │    usleep(POLL_INTERVAL_US);                                 │  │
 │  │  }                                                           │  │
 │  └─────────────────────────────────────────────────────────────┘  │
 └─────────────────────────────────────────────────────────────────────┘
```

### 1.3 路径对比

| 路径 | 数据方向 | 延迟特征 | 用途 |
|------|---------|---------|------|
| **TCP 保底** | Master send() → TCP 栈 → Slave recv() | 高（协议栈+中断） | 控制消息+数据保底 |
| **RDMA WRITE** | kprobe → ringbuf → RDMA WRITE → MR | 低（DMA 直达） | 增量数据加速 |

Slave 侧两条路径关系：
- **优先消费 MR 环形缓冲区**——数据到达时立即处理（低延迟）
- **TCP 路径数据到达后**——通过 `repl_offset` 比较，跳过已消费数据
- **RDMA 路径正常时**——TCP 数据到达 Slave 时已被 MR 路径消费过，直接跳过

---

## 2. 详细数据流

### 一次 SET 命令的完整路径

```
Client:    SET k001 value001\r\n

Step 1 — Master 命令处理 (用户态)
  kvstore: recv → parse_resp_stream → handle_parsed_command()
    → engine_set("k001", "value001")
    → persist_append_raw(raw, 45)           ← AOF
    → repl_broadcast(raw, 45)               ← 广播给 replica
    → repl_realtime_send(c, raw, 45)
       → repl_transport_kprobe_rdma_send()  ← 返回 -1，不发送
       → repl_transport_tcp_send(c, raw, 45) ← TCP 发送
    └─ (45 字节 RESP: *3\r\n$3\r\nSET\r\n...)

Step 2 — kprobe 内核拦截
  内核: tcp_sendmsg(sk, msg, 45)
    → kprobe/tcp_sendmsg 触发:
      1. PID == kvstore_pid ✓
      2. ctx->dx = 45 (size)
      3. ctx->si = msg 指针 (内核地址)
      4. bpf_probe_read_kernel(msg+40 = iov 指针)
      5. bpf_probe_read_kernel(iov[0] = {iov_base, iov_len})
      6. bpf_probe_read_user(iov_base, 45) → 读用户数据
      7. bpf_ringbuf_output([4B len=45][45B data])
    → 正常 TCP 协议栈继续处理 (作为保底路径)

Step 3 — Ringbuf → 用户态转发
  ring_buffer__poll() 检测到新数据
    → kprobe_ringbuf_cb(NULL, data, 49) 回调:
      1. 检查 g_slave_mr.rkey ≠ 0 ← MR 已就绪？
      2. wr_slot_acquire(5000, &slot) → 获取 slot #3
      3. memcpy(g_wr_slots[3].buf, [4B len][45B data])
      4. wr_submit_data(slot #3):
           ibv_post_send(RDMA_WRITE,
             remote_addr = slave_ringbuf + 16 + (idx * 512),
             rkey = slave_rkey)
      5. wr_submit_head(slot #3):
           ibv_post_send(RDMA_WRITE,
             remote_addr = slave_ringbuf + 0 (producer_head),
             data = new_head, FENCE)

Step 4 — RDMA 网络传输
  SiW0 RDMA 网卡:
    → 读取 g_wr_slots[3].buf (49 字节)
    → 通过 RDMA RC 连接传输
    → DMA 写入 Slave MR: 数据槽 + producer_head

Step 5 — Slave 轮询消费
  Slave 轮询线程:
    while (1):
      head = rb->producer_head  (已更新)
      while (tail=1023 < head=1024):
        slot = &rb->slots[1023 % 1024]
        len = *(uint32_t*)slot  → 45
        parse_resp_stream(NULL, slot+4, &len, 1)
          → handle_parsed_command(from_replication=1)
          → engine_set("k001", "value001")
          → persist_append_raw(raw, 45)  ← Slave 也写 AOF
        tail++  → 1024
      rb->consumer_tail = 1024
      __sync_synchronize()
      usleep(100)
```

---

## 3. 核心数据结构

### 3.1 Slave MR 环形缓冲区

```c
/* 文件: include/kvstore/replication/repl_kprobe.h */

#define KPROBE_RDMA_SLOT_COUNT      1024    /* 环形缓冲区槽数 */
#define KPROBE_RDMA_SLOT_CAPACITY   512     /* 每槽容量 */
#define KPROBE_RDMA_RINGBUF_SIZE    (16 + 1024 * 512)  /* ≈ 512KB */

typedef struct __attribute__((packed)) kprobe_rdma_ringbuf_s {
    volatile uint64_t producer_head;   /* 偏移 0  — Master 通过 RDMA WRITE 更新 */
    volatile uint64_t consumer_tail;   /* 偏移 8  — Slave 本地更新 */
    unsigned char slots[1024 * 512];   /* 数据槽区域 */
} kprobe_rdma_ringbuf_t;

/* 每个 slot 格式:
 *   [4 bytes: payload_len (uint32_t)]
 *   [payload_len bytes: RESP 协议数据] */
```

### 3.2 Master 侧 RDMA WRITE Slot

```c
/* 文件: src/replication/kvs_repl_kprobe.c */

#define KVS_RDMA_WRITE_SLOTS        8      /* 并发 RDMA WRITE slot 数 */
#define KVS_RDMA_WRITE_SLOT_SIZE    512    /* 每 slot 容量（与 BPF 匹配） */

typedef struct rdma_write_slot_s {
    unsigned char buf[512] __attribute__((aligned(64)));
    struct ibv_mr *mr;
    volatile int in_flight;
} rdma_write_slot_t;

static rdma_write_slot_t g_wr_slots[8];
```

### 3.3 Slave MR 信息（Master 侧）

```c
/* Master 侧保存的 Slave MR 信息 */
static struct {
    uint64_t remote_data_base;      /* Slave slots 基地址 (rb + 16) */
    uint32_t rkey;                  /* Slave MR rkey */
    uint64_t remote_head_addr;      /* Slave rb->producer_head 地址 */
    size_t   slot_count;            /* = KPROBE_RDMA_SLOT_COUNT */
    size_t   slot_capacity;         /* = KPROBE_RDMA_SLOT_CAPACITY */
} g_slave_mr;
```

### 3.4 kprobe-rdma QP 上下文

```c
/* 独立 QP，与 fullsync RDMA QP (g_repl_rdma_ctx) 分离 */
static struct {
    struct rdma_event_channel *ec;
    struct rdma_cm_id *id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    volatile int connected;
} g_rdma_kprobe;
```

---

## 4. BPF kprobe 程序

### 4.1 文件位置

`src/replication/bpf/repl_kprobe.bpf.c`

### 4.2 设计要点

| 要点 | 说明 |
|------|------|
| **Hook 点** | `kprobe/tcp_sendmsg` |
| **过滤条件** | PID == kvstore 进程 PID（通过 `kprobe_ctl` map 设置） |
| **寄存器参数** | x86_64: `di=sk`, `si=msg`, `dx=size` |
| **数据读取** | 三次 probe read 完成 |
| **输出格式** | `[4B payload_len][payload]`，单次 ≤ 500 字节 |

### 4.3 BPF 程序结构

```c
SEC("kprobe/tcp_sendmsg")
int kprobe_kvs_repl_tcp_sendmsg(struct pt_regs *ctx)
{
    /* 1. 检查开关 */
    enabled = bpf_map_lookup_elem(&kprobe_ctl, KEY_ENABLED);
    if (!enabled || !*enabled) return 0;

    /* 2. PID 过滤 */
    pid = bpf_get_current_pid_tgid() >> 32;
    target_pid = bpf_map_lookup_elem(&kprobe_ctl, KEY_PID);
    if (pid != *target_pid) return 0;

    /* 3. 数据大小 */
    size = ctx->dx;  /* 第3个参数 */
    if (size == 0 || size > 500) return 0;

    /* 4. 从 msghdr 读取数据 —— 关键部分 */
    msg_ptr = ctx->si;  /* 第2个参数，内核空间 msghdr 指针 */
    
    /* 使用 per-CPU 数组作为临时缓冲区 */
    entry = bpf_map_lookup_elem(&kprobe_tmpbuf, &key);
    
    /* 先写 4 字节长度头（失败时退化为通知模式） */
    payload_len = 0;
    memcpy(entry, &payload_len, 4);
    
    data_len = read_msg_data(msg_ptr, entry+4, 500);
    if (data_len > 0) {
        payload_len = data_len;
        memcpy(entry, &payload_len, 4);
        bpf_ringbuf_output(&repl_ringbuf, entry, 4 + data_len, 0);
    } else {
        bpf_ringbuf_output(&repl_ringbuf, entry, 4, 0);  /* 通知模式 */
    }
}
```

### 4.4 从 msghdr 读取 iovec 数据

这是最关键的逻辑。通过 `bpftool btf dump` 确认 kernel 5.15 的 struct 布局：

```
struct msghdr (size=96):
  offset  0: msg_name        (8B)
  offset  8: msg_namelen     (4B)
  offset 12: pad             (4B)
  offset 16: msg_iter        (struct iov_iter, 40B)
  offset 56: msg_control     (匿名 union, 很多字段)
  ...

struct iov_iter (size=40, kernel 5.15):
  offset  0: iter_type(1) + nofault(1) + data_source(1) + pad(5)  = 8B
  offset  8: iov_offset    (size_t, 8B)    ← 当前迭代偏移
  offset 16: count         (size_t, 8B)    ← 剩余字节数
  offset 24: iov           (union, 8B)     ← iovec 指针！
  offset 32: nr_segs       (union, 8B)     ← 段数
```

因此：
- **`iov 指针`** 在 `msg + 16 + 24 = msg + 40`
- **`nr_segs`** 在 `msg + 16 + 32 = msg + 48`

```c
static __always_inline int read_msg_data(unsigned long msg_ptr,
    unsigned char *buf, int max_len)
{
    /* 1. 读取 iov 指针 — bpf_probe_read_kernel(msg+40) */
    const struct { unsigned long b; unsigned long l; } *iov = 0;
    bpf_probe_read_kernel(&iov, sizeof(iov), msg_ptr + 40);
    if (!iov) return 0;

    /* 2. 读取 nr_segs — bpf_probe_read_kernel(msg+48) */
    unsigned long nr_segs = 0;
    bpf_probe_read_kernel(&nr_segs, sizeof(nr_segs), msg_ptr + 48);
    if (nr_segs == 0) return 0;

    /* 3. 读取第一个 iovec — bpf_probe_read_kernel(iov[0]) */
    /*    iov 是内核栈上的 iovec (send 系统调用创建) */
    struct { unsigned long b; unsigned long l; } vec;
    bpf_probe_read_kernel(&vec, sizeof(vec), &iov[0]);
    if (!vec.b || vec.l == 0) return 0;

    /* 4. 读取用户空间数据 — bpf_probe_read_user(iov_base) */
    unsigned long long safe_len = vec.l;
    if (safe_len > max_len) safe_len = max_len;
    bpf_probe_read_user(buf, safe_len, vec.b);

    return (int)safe_len;
}
```

### 4.5 BPF Maps

```c
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);       /* BPF_MAP_TYPE_ARRAY */
    __uint(max_entries, 8);
    __type(key, __u32);
    __type(value, __u64);
} kprobe_ctl SEC(".maps");                  /* 控制: [0]=enabled, [1]=pid */

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 16);
    __type(key, __u32);
    __type(value, __u64);
} kprobe_stats SEC(".maps");                /* 统计: hit, skip_pid, rb_err, ... */

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);     /* BPF_MAP_TYPE_RINGBUF = 27 */
    __uint(max_entries, 1 << 20);           /* 1MB */
} repl_ringbuf SEC(".maps");                /* 数据通道 */

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);/* 每个 CPU 一份 */
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, unsigned char[504]);       /* 4B header + 500B data */
} kprobe_tmpbuf SEC(".maps");               /* BPF 栈溢出替代缓冲区 */
```

### 4.6 编译注意事项

BPF 程序使用 clang 编译，目标 `bpf` 架构：

```makefile
BPF_CFLAGS?=-O2 -g -target bpf -D__TARGET_ARCH_x86 -I/usr/include/x86_64-linux-gnu
```

由于 `struct pt_regs` 在 BPF target 下不可用，需手动定义，且字段名必须与
`bpf_tracing.h` 中 PT_REGS_PARM 宏一致（x86_64 非内核模式下使用
`rdi/rdx/rsi` 等带 `r` 前缀的命名）。

注意不能使用 `bpf_tracing.h` 的 PT_REGS_PARM 宏——它们触发 CO-RE 重定位，
与手动定义的 `struct pt_regs` 不匹配。应直接访问寄存器：
- `ctx->dx` = `PT_REGS_PARM3` = size
- `ctx->si` = `PT_REGS_PARM2` = msg

---

## 5. 用户态转发模块

### 5.1 文件位置

`src/replication/kvs_repl_kprobe.c`

### 5.2 转发线程

```c
static void *kprobe_rdma_forward_thread(void *arg) {
    while (g_kprobe_running && g_rdma_kprobe.connected) {
        if (g_kprobe_ringbuf) {
            int err = ring_buffer__poll(g_kprobe_ringbuf,
                KVS_KPROBE_RINGBUF_POLL_MS);  /* 5ms */
            if (err < 0) usleep(1000);
        } else usleep(10000);
    }
    return NULL;
}
```

### 5.3 Ringbuf 回调

```c
static int kprobe_ringbuf_cb(void *ctx, void *data, size_t size) {
    /* 解析 payload_len */
    payload_len = *(uint32_t*)data;
    if (payload_len == 0 || payload_len + 4 > size) return 0;
    
    g_total_events++;
    g_total_bytes += payload_len;

    /* ★ MR 未就绪时跳过 —— KPROBEMR 还没交换完 */
    if (g_slave_mr.rkey == 0 || !g_rdma_kprobe.connected)
        return 0;

    /* 获取 WRITE slot */
    if (wr_slot_acquire(5000, &slot) != 0) return -1;

    /* 构造 [4B len][payload] */
    memcpy(g_wr_slots[slot].buf, &payload_len, 4);
    memcpy(g_wr_slots[slot].buf + 4, payload, payload_len);

    /* RDMA WRITE 数据 */
    if (wr_submit_data(slot, payload_len + 4) != 0) return -1;
    
    /* RDMA WRITE 更新 producer_head */
    if (wr_submit_head(slot) != 0) return -1;

    g_wr_in_flight++;
    g_wr_producer_seq++;
    g_rdma_writes++;
    return 0;
}
```

### 5.4 RDMA WRITE 提交

```c
/* 提交 RDMA WRITE 数据到 Slave 槽位 */
static int wr_submit_data(int slot, size_t len) {
    slot_idx = g_wr_producer_seq % g_slave_mr.slot_count;
    remote_off = slot_idx * g_slave_mr.slot_capacity;

    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_FENCE;
    wr.wr.rdma.remote_addr = g_slave_mr.remote_data_base + remote_off;
    wr.wr.rdma.rkey = g_slave_mr.rkey;
    
    return ibv_post_send(g_rdma_kprobe.id->qp, &wr, &bad);
}

/* 提交 RDMA WRITE 更新 Slave producer_head */
static int wr_submit_head(int slot) {
    new_head = g_wr_producer_seq + 1;
    memcpy(g_wr_slots[slot].buf, &new_head, 8);

    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = g_slave_mr.remote_head_addr;
    wr.wr.rdma.rkey = g_slave_mr.rkey;

    return ibv_post_send(g_rdma_kprobe.id->qp, &wr, &bad);
}
```

**FENCE 的作用**：保证数据 WRITE 完成后再更新 head。没有 FENCE，
head 可能先于数据到达 Slave，导致 Slave 读到不完整数据（全零或部分数据）。

### 5.5 CQ 回收机制

CQ 仅在 `wr_slot_acquire` 中当所有 slot 都 `in_flight` 时轮询：

```c
static int wr_slot_acquire(int timeout_ms, int *out) {
    /* 先找空闲 slot */
    for (int i = 0; i < 8; i++) {
        if (!g_wr_slots[idx].in_flight) {
            g_wr_slots[idx].in_flight = 1;
            *out = idx;
            return 0;
        }
    }
    /* 全部 in_flight → 轮询 CQ 回收 */
    pthread_mutex_lock(&g_kprobe_rdma_lock);
    while (ibv_poll_cq(g_rdma_kprobe.cq, 1, &wc) > 0) {
        if (wc.status == IBV_WC_SUCCESS) {
            slot = wc.wr_id & 0xFFFF;
            g_wr_slots[slot].in_flight = 0;
            g_wr_in_flight--;
        }
    }
    pthread_mutex_unlock(&g_kprobe_rdma_lock);
    /* 超时或重试 */
}
```

---

## 6. Slave 侧处理

### 6.1 Slave 轮询线程

```c
static void *kprobe_rdma_slave_poll(void *arg) {
    kprobe_rdma_ringbuf_t *rb = g_slave_ringbuf;
    unsigned char stream_buf[BUFFER_CAP];
    size_t stream_len = 0;

    while (g_kprobe_running) {
        __sync_synchronize();
        uint64_t head = rb->producer_head;
        uint64_t tail = rb->consumer_tail;

        if (tail == head) { usleep(100); continue; }

        while (tail != head) {
            idx = tail % 1024;
            off = idx * 512;
            slot_len = *(uint32_t*)(rb->slots + off);
            if (slot_len == 0 || slot_len > 508) { tail++; continue; }

            // 拼接 RESP 流
            if (stream_len + slot_len > sizeof(stream_buf))
                stream_len = 0;
            memcpy(stream_buf + stream_len, rb->slots + off + 4, slot_len);
            stream_len += slot_len;
            parse_resp_stream(NULL, stream_buf, &stream_len, 1);
            tail++;
        }
        rb->consumer_tail = tail;
        __sync_synchronize();
    }
}
```

### 6.2 Slave Listener

监听 `base_port + 12`（如 5160+12=5172），等待 Master 连接：

```c
static void *kprobe_rdma_slave_listener(void *arg) {
    /* 1. 创建 CM ID 并 bind/listen */
    rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP);
    rdma_bind_addr(listen_id, (struct sockaddr *)&addr);  // port+12
    rdma_listen(listen_id, 1);

    /* 2. 接受连接请求 */
    rdma_get_cm_event(ec, &event);  // CONNECT_REQUEST
    child_id = event->id;

    /* 3. 创建 PD + CQ + QP */
    ibv_alloc_pd(child_id->verbs);
    ibv_create_cq(child_id->verbs, 16, NULL, NULL, 0);
    rdma_create_qp(child_id, pd, &attr);  // RC QP

    /* 4. 注册 MR */
    repl_kprobe_rdma_slave_accept(pd, resp, sizeof(resp));

    /* 5. Accept */
    rdma_accept(child_id, &param);

    /* 6. 等待 ESTABLISHED */
    rdma_get_cm_event(ec, &event);  // ESTABLISHED

    /* 7. 保持连接直到断开 */
    while (g_kprobe_running) {
        rdma_get_cm_event(ec, &event);
        if (DISCONNECTED) break;
    }
}
```

---

## 7. 建链与 MR 交换流程

完整的建链时序：

```
Master                                  Slave
  │                                       │
  ├─ 启动 ──────────────────────────────► ─┤
  │  repl_kprobe_rdma_master_init()        │  repl_kprobe_rdma_slave_init()
  │  • 加载 BPF, attach kprobe             │  • 启动 listener(port+12)
  │  • 设置 PID 过滤                       │
  │  • 分配 WRITE slots                    │
  │                                       │
  ├─ 第一次 repl_broadcast ──────────────► ─┤
  │  repl_transport_kprobe_rdma_send()     │
  │  • dup(fd) → tcp_fd                    │
  │  • pthread_create(kprobe_mr_connect)   │
  │  • 返回 -1 → TCP 保底发送              │
  │                                       │
  ├─ 后台 kprobe_mr_connect_thread ──────► ─┤
  │  1. kprobe_rdma_qp_connect(host,port)  │
  │     • 创建 CM ID, 解析地址              │  ← listener 接受连接
  │     • rdma_connect → ESTABLISHED        │  → PD+CQ+QP+MR
  │     • 注册 WRITE slots MR               │  → rdma_accept
  │     • g_rdma_kprobe.connected = 1       │  → 启动 poll 线程
  │  2. 启动 forward thread                 │
  │  3. sleep(200ms)                        │
  │  4. send(tcp_fd, "KPROBEMR\r\n")        │  ← TCP 收到 KPROBEMR
  │  5. close(tcp_fd)                       │  → parse_resp_stream 处理
  │                                         │  → send(tcp_fd, "+KPROBERDMA ...")
  │  ← +KPROBERDMA 被 reactor 接收          │
  │  → repl_kprobe_rdma_parse_mr_info()     │
  │  → g_slave_mr.rkey 已就绪              │
  │                                         │
  ├─ 后续 repl_broadcast ────────────────── ─┤
  │  kprobe 拦截 → ringbuf → callback       │
  │  → g_slave_mr.rkey ≠ 0 ✓               │
  │  → RDMA WRITE ───────────────────────►  │
  │                                         │  → poll 消费 MR
  │                                         │  → parse 写入引擎
```

**关键点**：
- `g_slave_mr.rkey = 0` 时，回调跳过 RDMA WRITE（仅统计）
- KPROBEMR 通过 TCP 连接发送（dup 的 fd）
- +KPROBERDMA 响应通过同一 TCP 连接返回，被 reactor 的 `parse_resp_stream` 处理
- MR 信息在 `kvstore.c` 的 `handle_parsed_command` 中解析并调用 `repl_kprobe_rdma_parse_mr_info_direct()`

---

## 8. Transport 集成

### 8.1 transport ops

```c
/* src/replication/kvs_repl.c */
static const repl_transport_ops_t g_repl_transport_kprobe_rdma_ops = {
    .name = "kprobe-rdma",
    .supported = KVS_ENABLE_KPROBE_RDMA,
    .send = repl_transport_kprobe_rdma_send,
    .connect_slave = repl_transport_kprobe_rdma_connect_slave,
    .disconnect_slave = repl_transport_kprobe_rdma_disconnect_slave,
};
```

### 8.2 send 函数

```c
static int repl_transport_kprobe_rdma_send(conn_t *c, const unsigned char *buf, size_t len) {
    /* 只对有效 socket fd 发起连接 */
    if (!c || c->fd <= 2 || !KVS_ENABLE_KPROBE_RDMA) return 0;
    
    /* 首次调用：后台连接 Slave MR listener */
    static volatile int mr_connect_started = 0;
    if (!mr_connect_started) {
        getpeername(c->fd, &peer, &peer_len);     // 获取 slave 地址
        mr_connect_started = 1;
        // 创建后台连接线程
        a->port = g_cfg.port;
        a->tcp_fd = dup(c->fd);
        pthread_create(&tid, NULL, kprobe_mr_connect_thread, a);
    }
    
    /* 始终返回 -1，触发 TCP 保底发送 */
    return -1;
}
```

**设计原因**：`repl_realtime_send` 调用此函数后，如果返回非 0，
会自动尝试 `repl_transport_tcp_send` 作为保底。kprobe 拦截的就是
TCP send 路径上的数据，因此 TCP 保底是必需的——没有 TCP 发送就没有
kprobe 数据源，也就没有 RDMA WRITE。

---

## 9. 关键实现细节

### 9.1 为什么 iov 在 msg+40 而不是 msg+24？

通过 `bpftool btf dump id 1` 检查内核 BTF 发现 kernel 5.15 的 `iov_iter` 布局
与常见参考资料不同：

| 想当然的布局 | 实际布局 (kernel 5.15) |
|------------|----------------------|
| iter_type(1)+pad(7)=8B | iter_type(1)+nofault(1)+data_source(1)+pad(5)=8B |
| iov(8B) \*8B offset | **iov_offset**(8B) ← 不在这里！ |
| nr_segs(8B) \*16B offset | **count**(8B) ← 不在这里！ |
| iov_offset(8B) \*24B offset | **iov**(8B) ← 在这里！ |
| count(8B) \*32B offset | **nr_segs**(8B) ← 在这里！ |

因此 `iov` 在 `msg + 16 + 24 = msg + 40`，不是直觉的 `msg + 24`。

### 9.2 为什么数据读取会全部失败

早期版本一直 `payload_len=0` 的原因：

1. **偏移猜错** — `iov` 在 msg+40 而不是 msg+24/32
2. **`bpf_probe_read_user` 不能读内核内存** — `msg` 是内核指针
3. **`bpf_probe_read_kernel` 不能读用户内存** — 数据在 `iov_base`（用户空间）
4. **CO-RE 重定位失败** — 手动定义 `struct pt_regs` 字段名与 `bpf_tracing.h` 不匹配
5. **MR 未就绪时 RDMA WRITE 使 QP 崩溃** — rkey=0 导致写错误

### 9.3 关于 `kvstore_transport.log` 中的 "kprobe-rdma failed"

这是**误报**。`repl_transport_kprobe_rdma_send` 设计上始终返回 -1，
触发 TCP 保底（kprobe 需要 TCP 数据源）。`repl_realtime_send` 的 fallback
逻辑在 kprobe-rdma 情况下不应被视为"失败"。已修复。

### 9.4 为什么没有 fd 过滤

BPF 程序仅通过 PID 过滤。kvstore master 进程只有一个 replication socket
发送到 slave，PID 过滤已足够。添加 fd 过滤需要读取 `struct sock` 的
`skc_num/skc_dport` 字段，这需要 CO-RE 或硬编码偏移，增加复杂度。

### 9.5 关于 BPF 栈限制

BPF 程序的栈限制为 512 字节。`entry` 缓冲区需要 504 字节（4B header + 500B data），
加上其他局部变量会超出限制。解决方案：

- 使用 `BPF_MAP_TYPE_PERCPU_ARRAY` 作为临时缓冲区
- 每个 CPU 一份，无竞争
- 通过 `bpf_map_lookup_elem` 访问

---

## 10. 调试与观测

### 10.1 关键日志

```
Master:
  kprobe: BPF loaded and attached to tcp_sendmsg          ← BPF 加载成功
  kprobe rdma: master init OK, PID=xxxx                   ← 初始化完成
  kprobe rdma: QP connected                                ← kprobe 独立 QP 就绪
  kprobe rdma: forward thread started                      ← 转发线程运行
  kprobe rdma: [DBG] KPROBEMR sent to slave                ← MR 请求已发送
  kprobe rdma: +KPROBERDMA received                        ← MR 信息已获取
  kprobe rdma: MR info updated - rkey=... slots=... cap=...← MR 就绪
  kprobe rdma: [DBG] ringbuf_cb size=504 payload_len=500   ← BPF 抓到数据
  kprobe rdma: [DBG] slave poll data head=2 tail=0 diff=2  ← Slave 收到数据

Slave:
  kprobe rdma: slave listener ready on port 5172            ← listener 运行
  kprobe rdma: slave listener - CONNECT_REQUEST received    ← Master 连接
  kprobe rdma: slave MR registered, rkey=...                ← MR 已注册
  kprobe rdma: KPROBEMR received, sending MR info...         ← 响应 MR 请求
  kprobe rdma: [DBG] slave poll consuming slot_len=5        ← 正在消费数据
```

### 10.2 验证测试

```bash
# 编译
make clean && make -j$(nproc)

# 启动 Slave
sudo ./kvstore --port 5161 --role slave \
  --master-host 192.168.233.128 --master-port 5160 \
  --repl-fullsync-transport rdma \
  --repl-realtime-transport kprobe-rdma \
  --kprobe-enabled 2>&1 | tee /tmp/slave.log

# 启动 Master
sudo ./kvstore --port 5160 --role master \
  --repl-fullsync-transport rdma \
  --repl-realtime-transport kprobe-rdma \
  --rdma-dev siw0 --kprobe-enabled 2>&1 | tee /tmp/master.log

# 运行测试 (在测试机)
./test_repl_5w5w --master-host 192.168.233.128 --master-port 5160 \
  --slave-host 192.168.233.129 --slave-port 5161 \
  --pre 50000 --post 50000
```

### 10.3 BPF 调试

```bash
# 查看内核 BTF 中的 struct 定义
sudo bpftool btf dump id 1 | grep -A15 "msghdr'"
sudo bpftool btf dump id 1 | grep -A12 "iov_iter'"

# 直接加载测试 BPF 程序
sudo bpftool prog load build/replication/bpf/repl_kprobe.bpf.o /sys/fs/bpf/test

# 查看 kprobe 统计
sudo bpftool map lookup name kprobe_stats key 0 0 0 0
```

---

## 11. 关键代码位置索引

| 文件 | 行号范围 | 内容 |
|------|---------|------|
| `src/replication/bpf/repl_kprobe.bpf.c` | 1-240 | BPF kprobe 程序 |
| `src/replication/kvs_repl_kprobe.c` | 1-1040 | 用户态转发模块 |
| `include/kvstore/replication/repl_kprobe.h` | 1-50 | 头文件、常量定义 |
| `src/replication/kvs_repl.c` | 1336-1395 | kprobe-rdma transport ops |
| `src/replication/kvs_repl.c` | 1577-1585 | transport ops 选择逻辑 |
| `src/replication/kvs_repl.c` | 1602-1611 | repl_realtime_send |
| `src/main/kvstore.c` | 1035-1046 | +KPROBERDMA 处理（MR 信息解析） |
| `Makefile` | 102-106 | BPF 编译目标 |
