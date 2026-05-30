# kvstore — 高性能键值存储系统

<div align="center">

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C](https://img.shields.io/badge/language-C-blue.svg)](https://en.wikipedia.org/wiki/C_(programming_language))
[![Build](https://img.shields.io/badge/build-4%20configs-brightgreen)]()
[![RDMA](https://img.shields.io/badge/RDMA-supported-orange)]()
[![eBPF](https://img.shields.io/badge/eBPF-supported-blueviolet)]()

kvstore 是一个用 **C 语言** 实现的类 Redis 键值存储系统，面向学习和研究。

**多存储引擎 · 多网络模型 · 多内存后端 · 持久化 · 主从复制 · 文档型 Value · TTL · RDMA · eBPF**

</div>

---

## 目录

- [快速开始](#快速开始)
- [项目结构](#项目结构)
- [核心能力](#核心能力)
- [命令参考](#命令参考)
- [配置说明](#配置说明)
- [文档索引](#文档索引)
- [测试体系](#测试体系)
- [测试产物路径](#测试产物路径)
- [性能基准](#性能基准)
- [开发指南](#开发指南)
- [常见问题](#常见问题)
- [许可证](#许可证)

---

## 快速开始

### 环境依赖

```bash
# Ubuntu/Debian
sudo apt install gcc make liburing-dev libjemalloc-dev

# RDMA 支持（可选）
sudo apt install librdmacm-dev libibverbs-dev

# eBPF 支持（可选，需 ENABLE_EBPF=1）
sudo apt install libbpf-dev libelf-dev clang
```

### 编译

```bash
git clone --recurse-submodules <repo-url>
cd kvstore
make clean && make
```

> 编译产物：`./kvstore`（单可执行文件）。编译选项见 Makefile 顶部 `ENABLE_RDMA`、`ENABLE_EBPF` 开关。

### 启动

```bash
./kvstore                           # 自动加载 ./kvstore.conf（如存在）
./kvstore --config kvstore.conf     # 显式指定配置
./kvstore --port 6380 --mem jemalloc  # 命令行覆盖配置
```

### 快速验证

```bash
# 启动服务后，用 nc 测试基本读写
printf '*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n' | nc 127.0.0.1 5000
printf '*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n' | nc 127.0.0.1 5000
```

或使用 Redis 客户端（如 `redis-cli`）直接连接 5000 端口。

---

## 项目结构

```
kvstore/
├── src/                          # 核心 C 源码
│   ├── main/kvstore.c            #   入口、RESP 协议、命令分发
│   ├── core/                     #   网络模型 (reactor / proactor / ntyco)
│   ├── storage/                  #   存储引擎 (array / hash / rbtree / skiptable / doc)
│   ├── memory/kvs_mem.c          #   内存后端 (libc / jemalloc / custom)
│   ├── expire/kvs_expire.c       #   TTL 过期管理
│   ├── persistence/kvs_persist.c #   持久化 (dump + AOF)
│   ├── replication/              #   主从复制、RDMA、eBPF、哨兵
│   └── utils/hash.c              #   哈希工具
├── include/kvstore/              # 公共头文件
├── NtyCo/                        # 协程库 (git submodule)
├── liburing/                     # io_uring 库
├── tools/                        # 测试 & 辅助脚本
│   ├── bench/                    #   性能基准脚本
│   ├── persist/                  #   持久化验证脚本
│   ├── repl/                     #   复制验证脚本 (TCP/RDMA/eBPF)
│   ├── rdma/                     #   RDMA 探测脚本
│   └── tests/                    #   通用测试辅助脚本
├── tests/                        # 测试代码
│   ├── integration/              #   集成测试脚本
│   ├── unit/                     #   单元测试
│   ├── test.c                    #   测试工具函数
│   └── testcase.c                #   C 测试用例框架
├── testdata/                     # 静态测试数据 (样例 AOF/dump/配置)
├── artifacts/                    # 测试运行时产物 (gitignored)
│   ├── persist/                  #   持久化测试产物
│   ├── repl/                     #   复制测试产物
│   ├── rdma/                     #   RDMA 测试产物
│   ├── bench/                    #   基准测试产物
│   └── legacy/                   #   旧版产物
├── benchmarks/                   # 基准测试数据与图表
│   ├── data/                     #   CSV 测试数据
│   └── plots/                    #   可视化图表
├── assets/diagrams/              # 架构图 / 流程图
├── clients/                      # 多语言客户端示例 (Go/Java/JS/Python/Rust)
├── docs/                         # 文档中心
│   ├── tech-roadmap.md           #   技术路线与实现详解 ← 新手必读
│   ├── rdma-fullsync-implementation.md  # RDMA 全量复制实现
│   ├── plan.md                   #   项目演进规划
│   ├── iteration-summary.md      #   迭代总结
│   └── examples/                 #   API 使用示例
├── kvstore.conf                  # 默认配置文件
├── Makefile                      # 构建入口
├── .github/workflows/ci.yml      # GitHub CI 配置
```

---

## 核心能力

### 存储引擎


| 引擎      | 前缀   | 说明                       |
| --------- | ------ | -------------------------- |
| Array     | 无前缀 | 基础数组存储，适合小数据量 |
| Hash      | `H*`   | 哈希表，适合大量 key 场景  |
| RBTREE    | `R*`   | 红黑树，有序存储           |
| Skiptable | `X*`   | 跳表，适合范围查询         |

> 例：`HSET key value` 使用哈希引擎，`RSET key value` 使用红黑树引擎。

### 内存后端


| 后端       | 特点                                    |
| ---------- | --------------------------------------- |
| `libc`     | 标准 malloc/free，最通用                |
| `jemalloc` | 高性能分配器，减少碎片                  |
| `custom`   | 自研 slab + mmap 分配器，可观测碎片统计 |

### 网络模型


| 模型     | 底层     | 适用场景   |
| -------- | -------- | ---------- |
| Reactor  | epoll    | I/O 密集型 |
| Proactor | io_uring | 高并发异步 |
| NtyCo    | 协程     | 海量连接   |

### 功能矩阵


| 功能                         | 状态      | 说明                                               |
| ---------------------------- | --------- | -------------------------------------------------- |
| RESP 协议                    | ✅ 完成   | 完整解析与响应                                     |
| 全量持久化 (dump)            | ✅ 完成   | 二进制`KVSD` 格式，优先 mmap 恢复                  |
| 增量持久化 (AOF)             | ✅ 完成   | RESP 命令格式，优先 io_uring 写入                  |
| SAVE / BGSAVE / BGREWRITEAOF | ✅ 完成   | 支持同步/异步持久化                                |
| 主从复制                     | ✅ 完成   | FULLRESYNC + partial resync + backlog              |
| RDMA 全量同步                | ✅ 完成   | 全量数据通过 RDMA 传输，与 eBPF 实时同步可同时启用 |
| eBPF 实时同步                | ✅ 完成   | sockmap 转发路径，实时增量命令通过 eBPF 加速       |
| TTL / 过期                   | ✅ 完成   | 哈希索引 + 最小堆调度                              |
| 文档型 value                 | ✅ 完成   | DOCSET/DOCGET 等 7 个命令                          |
| 分布式锁                     | ✅ 完成   | LOCK/UNLOCK/RENEW/OWNER                            |
| 哨兵模式                     | ⚠️ 基础 | 框架已有，自动故障转移待完善                       |
| 自动快照                     | ✅ 完成   | 按时间+变化数规则触发                              |

---

## 实现原理

### 网络模型

三种 I/O 模型通过 `--net` 或 `net_backend` 配置，编译时通过 `#include` 选择性启用。

#### Reactor (epoll) — 默认

基于 epoll 的事件驱动单线程模型。所有连接共享一个 epoll fd。

**核心数据结构**:
```c
// reactor.c 中每个连接一个 conn_t
typedef struct conn_s {
    int fd;
    unsigned char *inbuf;    // 读缓冲区 (动态增长)
    size_t in_len;
    queue *out_queue;        // 写队列 (queue_bytes 追加)
    // ...
} conn_t;
```

**事件循环**:
```
epoll_wait()
  ├─ EPOLLIN  → on_read(c)
  │               └─ read(fd, inbuf) → parse_resp_stream(c, inbuf, &in_len, 0)
  │                                     └─ handle_parsed_command(c, argc, argv, raw, rawlen, 0)
  │                                           ├─ 写命令 → 执行引擎 → queue_bytes(c, resp)
  │                                           └─ 写命令 + 主角色 → repl_broadcast(raw, rawlen)
  └─ EPOLLOUT → on_write(c)
                  └─ send(fd, out_queue) → 写完则关闭 EPOLLOUT
```

**核心特性**:
- 单线程无锁，所有操作顺序执行
- `queue_bytes(c, buf, len)` 将响应追加到连接的写队列，`on_write` 负责实际 send
- 复制时 `repl_broadcast()` 遍历 replica 链表调用 `queue_bytes`

**核心文件**: `src/core/reactor.c`

#### Proactor (io_uring)

基于 Linux io_uring 的异步 I/O 模型。提交 SQE (Submission Queue Entry) 后立即返回，完成后从 CQ (Completion Queue) 获取结果。

```c
// 提交读请求
struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
io_uring_prep_read(sqe, fd, buf, len, offset);
io_uring_submit(&ring);

// 等待完成
struct io_uring_cqe *cqe;
io_uring_wait_cqe(&ring, &cqe);
// cqe->res 包含实际读到的字节数
```

**核心文件**: `src/core/proactor.c`

#### NtyCo 协程

基于 NtyCo 协程库，将异步 I/O 以同步方式编写，每连接一个协程。

```
nty_co_create(func, arg)  → 创建协程
  func() {
    read(fd, buf, len)    → 内部调用 nty_co_yield() 让出 CPU
  }
nty_co_run()              → 调度所有就绪协程
```

**协程切换**: 通过 `setjmp`/`longjmp` 保存和恢复栈上下文，每次 `yield` 切换到调度器，调度器选择下一个就绪协程 `resume`。

**核心文件**: `src/core/ntyco.c`、`NtyCo/core/`

---

### 协议层 — RESP 编解码

RESP (Redis Serialization Protocol) 是 kvstore 的通信协议。

**解析入口**: `parse_resp_stream(conn_t *c, unsigned char *buf, size_t *len, int from_replication)`

```
buf 中可能包含多条命令，循环解析:
  while (pos < *len) {
    if (buf[pos] == '+') → 简单字符串: +OK\r\n
    if (buf[pos] == '*') → 数组: *3\r\n$3\r\nSET\r\n...
    if (其他)            → 内联命令: SET key value
    handle_parsed_command(c, argc, argv, raw, rawlen, from_replication)
  }
```

**`from_replication` 标志**:
- `=0`: 来自客户端，写命令触发 `repl_broadcast()` 广播给所有 slave
- `=1`: 来自主节点复制，写命令**不**广播，仅应用并调用 `repl_slave_note_applied(rawlen)` 更新 offset

**命令分发**: `handle_parsed_command()` 根据命令前缀路由到不同引擎:

```c
if (!strcmp(cmd, "SET"))    → engine = KVS_ENGINE_ARRAY
if (!strcmp(cmd, "HSET"))   → engine = KVS_ENGINE_HASH
if (!strcmp(cmd, "RSET"))   → engine = KVS_ENGINE_RBTREE
if (!strcmp(cmd, "XSET"))   → engine = KVS_ENGINE_SKIPTABLE
if (!strcmp(cmd, "DOCSET")) → engine = KVS_ENGINE_DOC
```

**核心文件**: `src/main/kvstore.c`

---

### 存储引擎

5 种引擎通过命令前缀自动路由，每种引擎实现 `set`/`get`/`del`/`exist`/`mod` 五个接口。

#### Array (数组)

```c
// 静态数组，线性扫描
typedef struct { char key[64]; char value[256]; } array_entry_t;
static array_entry_t global_array[KVS_ARRAY_SIZE];  // 默认 100000
static int global_array_count = 0;

// SET: 线性查找已有 key → 找到则覆盖，否则写入第一个空位
for (int i = 0; i < KVS_ARRAY_SIZE; i++)
    if (!global_array.table[i].key || strcmp(...) == 0) { ... }
```

#### Hash (哈希表)

```c
// FNV-1a 哈希 + 链地址法
#define KVS_HASH_SLOTS 65536
typedef struct hashnode_s {
    char *key, *value;
    struct hashnode_s *next;
} hashnode_t;
static hashnode_t **global_hash_nodes;  // 指针数组

// HSET: hash(key) → slot → 遍历链表 → 找到更新/未找到插入
uint32_t h = kvs_hash_fnv1a(key);
hashnode_t *n = global_hash_nodes[h % max_slots];
while (n) { if (!strcmp(n->key, key)) { ... } n = n->next; }
```

#### RBTree (红黑树)

```c
// 自平衡二叉搜索树，中序遍历有序
typedef struct rbtree_node {
    int color;  // RED=0 / BLACK=1
    char key[64];
    void *value;
    struct rbtree_node *left, *right, *parent;
} rbtree_node_t;

// RSET: 插入节点 → rbtree_insert(&root, node, nil)
// 再平衡: 左旋/右旋/颜色翻转 → 保证 5 条红黑树性质
```

#### SkipTable (跳表)

```c
// 多层索引链表，概率平衡
#define KVS_SKIPTABLE_MAX_LEVEL 32
typedef struct kvs_skiptable_node {
    char *key, *value;
    struct kvs_skiptable_node **forward;  // 每层指向下一个节点
    int level;
} kvs_skiptable_node_t;

// XSET: 随机层数 → 从最高层向下找插入位置 → 更新各层指针
```

#### Doc (文档型)

```c
// key → Hash( field → value )
// DOCSET key field value: 先找 key → 找到则在其 field 哈希表中插入
// DOCGET key field: 先找 key → 再找 field
```

**核心文件**: `src/storage/kvs_array.c`、`kvs_hash.c`、`kvs_rbtree.c`、`kvs_skiptable.c`、`kvs_doc.c`

---

### 内存管理

三种后端通过 `--mem` 或 `mem_backend` 配置，`kvs_malloc`/`kvs_free`/`kvs_realloc` 统一接口：

#### libc
```c
void *kvs_malloc(size_t s) { return malloc(s); }
void kvs_free(void *p)     { free(p); }
```

#### jemalloc
```c
// 通过 LD_PRELOAD 在进程启动时注入
// LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so ./kvstore
// 多 arena 设计减少多线程锁竞争
```

#### custom (自研)
```c
// slab 分类 + mmap 预分配
#define SLAB_CLASSES {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192}
// kvs_malloc(60) → 从 64B slab 分配
// 每个 slab 用位图管理空闲块
// 内存不足时 mmap 新申请 1MB 块
// 支持 MEMSTAT 命令查看各 slab 使用率
```

**核心文件**: `src/memory/kvs_mem.c`

---

### 持久化

#### 全量 Dump (KVSD)

**保存流程**:
```
kvs_save()
  └─ snapshot_all_sink(&sink)     ← 遍历 5 个引擎
       ├─ snapshot_array_sink()   → emit_cmd3_sink("SET", key, value)
       ├─ snapshot_rbtree_sink()  → emit_cmd3_sink("RSET", key, value)
       ├─ snapshot_hash_sink()    → emit_cmd3_sink("HSET", key, value)
       ├─ snapshot_skiptable_sink() → emit_cmd3_sink("XSET", key, value)
       └─ snapshot_doc_sink()     → emit_cmd4_sink("DOCSET", key, field, value)
  └─ 写入临时文件 → fflush → fsync → rename → 完成
```

所有 KV 序列化为 RESP 命令文本（`SET key value\r\n`、`HSET key value\r\n`），即 dump 文件 = RESP 命令序列。

**恢复流程**:
```c
kvs_load()
  ├─ mmap(dump_fd, ...)            ← 优先 mmap
  │   └─ parse_resp_stream(NULL, mapped, &len, 1)  ← 直接解析
  └─ mmap 失败 → read + parse      ← 回退路径
```

mmap 恢复优势：内核按需加载页，大文件无需全部读入内存。

#### 增量 AOF

每条写命令以 RESP 协议追加到 AOF 文件：

```c
// kvstore.c handle_parsed_command():
if (from_replication && is_write_cmd(cmd)) {
    persist_append_raw(raw, rawlen);  // 写入 AOF
    repl_slave_note_durable(rawlen);  // 记录 durable offset
}
```

**同步策略**:
- `appendfsync always`: 每条命令后 `fsync`，最安全但最慢
- `appendfsync everysec`: 每秒 `fsync`，可能丢 1 秒数据

**AOF 重写 (BGREWRITEAOF)**:
```
将当前内存快照写为新 AOF → fsync → rename 替换旧 AOF
与 SAVE 类似，但输出格式是 RESP 协议命令序列
```

**核心文件**: `src/persistence/kvs_persist.c`

---

### 主从复制

整个复制系统设计为**双通道模式**：全量同步和增量同步可独立配置传输层。

```
--repl-fullsync-transport rdma    ← 全量走 RDMA
--repl-realtime-transport ebpf    ← 增量走 eBPF sockmap
```

#### 复制协议 (REPLSYNC)

```
Slave → Master:
  REPLSYNC <replid> <offset> <durable_offset>

Master 响应:
  +FULLRESYNC <replid> <offset> <size>\r\n  ← 全量同步
  [snapshot data: SET/RSET/HSET/XSET...]
  +REPLDONE\r\n                              ← 全量结束

  或 (部分同步):
  +CONTINUE <replid> <offset>\r\n            ← 增量续传
  [backlog data: 从 offset 开始的增量命令]
```

#### Offset 跟踪

```c
// Master 侧: 每条广播的命令累加原始字节数
void repl_note_broadcast(size_t bytes) {
    g_master_repl_offset += (unsigned long long)bytes;
}

// Slave 侧: 每条应用的命令累加
void repl_slave_note_applied(size_t rawlen) {
    g_slave_repl_applied_offset += (unsigned long long)rawlen;
}
```

`repl_offset` 本质是 RESP 原始字节流的累计偏移量，用于**断线重连后的精确续传**。

#### Backlog 环形缓冲区

```c
static repl_backlog_t g_repl_backlog = {
    .buf = circular buffer (1MB),
    .head, .histlen,           // 环形队列指针
    .start_offset, .end_offset // 对应的 offset 范围
};

repl_broadcast() → repl_backlog_feed(raw, rawlen)
                → 写入环形缓冲区，更新 end_offset
```

Slave 重连时发送 `offset`，Master 检查 `offset >= start_offset` 则可部分同步，直接发送 backlog 中的增量数据。

#### 全量同步 (queue_snapshot)

```
queue_snapshot(c)
  ├─ kvs_snapshot_to_fp(tmp_file)    ← 写临时文件
  ├─ 第一遍: 计算 total_bytes         ← FULLRESYNC header 需要
  ├─ 发送: +FULLRESYNC <replid> <offset> <total>\r\n
  ├─ 第二遍: 按 chunk_cap 分片发送
  │    └─ repl_send_chunked(c, buf, r)
  │         └─ repl_fullsync_send(c, buf, chunk)
  │              └─ repl_rdma_try_send(buf, len)   ← RDMA SEND
  │                 或 repl_transport_tcp_send()    ← TCP fallback
  └─ 发送: +REPLDONE\r\n
```

#### RDMA 全量同步 (Pipeline 架构)

**四层 Pipeline 设计**:

```c
#define KVS_RDMA_PIPELINE_DEPTH  4   // 4 个 send_slot 交替使用
#define KVS_RDMA_CQ_BATCH        8   // 批量 poll

// send_slot: 预注册的 RDMA 缓冲区
repl_rdma_send_slot_t send_slots[4];
// 每个 slot 有独立 buf + MR，可同时有 4 个 outstanding SEND WR

repl_rdma_try_send(buf, len):
  ① slot = acquire_send_slot()       // 获取空闲 slot（忙等最多 5s）
  ② memcpy(send_slots[slot].buf, buf, len)  // 拷贝数据
  ③ ibv_post_send(qp, &wr, &bad_wr)  // 提交 SEND WR 到 SQ
                                      // wr_id = slot | PIPELINE_FLAG
  ④ return 0  // 非阻塞返回
```

**CQ 轮询线程** (后台独立线程):
```c
repl_rdma_cq_poll_thread():
  while (running) {
    ibv_get_cq_event(comp_chan, &ev_cq, &ev_ctx)  // 阻塞等待事件
    ibv_poll_cq(cq, KVS_RDMA_CQ_BATCH, wc_batch)   // 批量 poll
    for each wc:
      if (SEND completion): release_send_slot()     // 释放 slot
      if (RECV completion): pending_recv_push()     // 入队
    ibv_req_notify_cq(cq, 0)                        // re-arm
    ibv_poll_cq(...)                                // drain
  }
```

**接收端** (Slave):
```c
recv_slots[32]      ← 32 个预注册的接收缓冲区，提前 ibv_post_recv
pending_recv_slots  ← 环形队列，存已完成的 recv_slot 索引

slave loop:
  repl_rdma_wait_cq_recv_completion(100ms, &slot, &len)
    → 从 pending_recv 队列取出 slot
    → repl_rdma_dup_recv_payload(slot, len)  // 拷贝数据
    → repl_rdma_repost_recv(slot)            // 重新 post recv
    → parse_resp_stream(NULL, buf, &blen, 1) // 解析执行
```

**自适应深度调节**:
```c
repl_rdma_adjust_pipeline_depth():
  if (in_flight <= depth/2 && depth < 4) → depth++  // 加深度
  if (in_flight >= depth  && depth > 2) → depth--   // 减深度
```

#### eBPF 实时同步 (sockmap)

**BPF 程序** (`repl_sockmap.bpf.c`):
```c
SEC("sk_msg")
int kvstore_repl_sk_msg(struct sk_msg_md *msg) {
    int key = 0;
    // 从 role_map 查当前角色: 1=master, 0=slave
    int *role = bpf_map_lookup_elem(&role_map, &key);
    if (!role || *role != 1) return SK_PASS;  // 仅 master 转发
    
    // 从 sock_map 查 slave fd
    int *slave_fd = bpf_map_lookup_elem(&sock_map, &key);
    if (!slave_fd) return SK_PASS;
    
    // 重定向到 slave socket
    return bpf_msg_redirect_map(&sock_map, key, BPF_F_INGRESS);
}
```

**数据路径**:
```
redis-cli → Master TCP → reactor on_read → handle_parsed_command
  → repl_broadcast(raw, rawlen)
    → queue_bytes(c_slave, raw, rawlen)     // 写入 slave 连接的写队列
      → reactor on_write(c_slave)
        → send(c_slave_fd, ...)
          → 内核 BPF sk_msg 拦截
            → bpf_msg_redirect_map(sock_map, 0, 0)
              → 直接写入 Slave TCP socket
```

#### kprobe+RDMA 增量同步

**BPF kprobe** (`repl_kprobe.bpf.c`):
```c
SEC("kprobe/__sys_sendto")
int kprobe__sys_sendto(struct pt_regs *ctx) {
    // 捕获 sendto 系统调用的数据
    size_t len = (size_t)PT_REGS_PARM3(ctx);  // buf 长度
    // 数据写入 ring buffer
    bpf_ringbuf_output(&ringbuf, buf, len, 0);
}
```

**用户态路径**:
```
kprobe BPF → ring buffer → 用户态回调
  → repl_capture_ringbuf_cb(data, len)
    → repl_rdma_incr_write(buf, len)     // RDMA WRITE 到 Slave MR
                                         // (one-sided, 无需 recv WR)
Slave: 轮询线程(1ms) ← 读共享 MR 的 head 指针
       → 有新数据 → memcpy → parse_resp_stream()
```

**核心文件**: `src/replication/kvs_repl.c`、`kvs_repl_ebpf.c`、`kvs_repl_kprobe.c`、`bpf/`

---

### TTL 过期系统

**数据流**:
```
EXPIRE key 10
  → handle_parsed_command()
    → kvs_expire_set(&global_expire, engine, key, now + 10)
      → 更新过期哈希表: expire_table[hash(key)] = expire_time
      → 更新过期最小堆: expire_heap.push({expire_time, engine, key})

TTL key
  → kvs_expire_ttl(&global_expire, engine, key)
    → 查 expire_table → 返回 (expire_time - now) 或 -1/-2
```

**过期检查**:
```c
// 定时任务 (kvs_expire.c):
void kvs_expire_check(kvs_expire_t *exp) {
    while (!min_heap_empty(&exp->heap) && min_heap_top(&exp->heap).time <= now) {
        pop min_heap → 惰性检查 expire_table
        if 已过期 → kvs_engine_del(engine, key)  // 主动删除
    }
}

// 访问时惰性删除:
if (key_past_expire(key)) return KEY_NOT_FOUND;  // GET/DEL 时检查
```

**核心文件**: `src/expire/kvs_expire.c`

---

### 配置与命令行系统

`kvstore.conf` 是 key=value 格式的配置文件，支持 `#` 注释。

```c
// 配置解析链:
命令行参数 → parse_config_file(kvstore.conf) → 程序默认值
// 优先级: 命令行 > 配置文件 > 默认值

// 解析入口 (kvstore.c):
static int parse_config_kv(const char *key, const char *value) {
    if (!strcmp(key, "port")) g_cfg.port = atoi(value);
    else if (!strcmp(key, "role")) ...;
    else if (!strcmp(key, "rdma_dev")) ...;
    else if (!strcmp(key, "kprobe_enabled")) ...;
    // 共约 40 个配置项
}
```

**核心文件**: `src/main/kvstore.c` (parse_config_kv)

---

## 命令参考

### 基本键值


| 命令                   | 说明           |
| ---------------------- | -------------- |
| `SET key value`        | 设置键值       |
| `GET key`              | 获取键值       |
| `DEL key`              | 删除键         |
| `EXIST key`            | 检查键是否存在 |
| `MSET k1 v1 k2 v2 ...` | 批量设置       |
| `MGET k1 k2 ...`       | 批量获取       |
| `MOD key value`        | 修改已有键的值 |

### TTL / 过期


| 命令                 | 说明         |
| -------------------- | ------------ |
| `EXPIRE key seconds` | 设置过期时间 |
| `TTL key`            | 查询剩余 TTL |
| `PERSIST key`        | 移除过期时间 |

### 持久化


| 命令                 | 说明              |
| -------------------- | ----------------- |
| `SAVE`               | 同步保存 dump     |
| `BGSAVE`             | 后台保存 dump     |
| `BGREWRITEAOF`       | 重写 AOF          |
| `APPENDFSYNC policy` | 设置 AOF 同步策略 |

### 文档对象


| 命令                     | 说明         |
| ------------------------ | ------------ |
| `DOCSET key field value` | 设置字段     |
| `DOCGET key field`       | 获取字段     |
| `DOCDEL key field`       | 删除字段     |
| `DOCDROP key`            | 删除整个文档 |
| `DOCEXIST key`           | 文档是否存在 |
| `DOCCOUNT key`           | 字段数量     |
| `DOCGETALL key`          | 获取全部字段 |

### 分布式锁


| 命令                      | 说明       |
| ------------------------- | ---------- |
| `LOCK key owner seconds`  | 获取锁     |
| `UNLOCK key owner`        | 释放锁     |
| `RENEW key owner seconds` | 续期       |
| `OWNER key`               | 查看持有者 |

### 复制与集群


| 命令                | 说明         |
| ------------------- | ------------ |
| `SLAVEOF host port` | 设为从节点   |
| `SLAVEOF NO ONE`    | 提升为主节点 |
| `ROLE`              | 查看复制状态 |

### 监控


| 命令                   | 说明             |
| ---------------------- | ---------------- |
| `INFO`                 | 服务器综合信息   |
| `MEMSTAT`              | 内存统计         |
| `PING`                 | 连接测试         |
| `SNAPRULE sec changes` | 添加自动快照规则 |
| `SNAPRULES`            | 查看快照规则     |
| `SNAPRULECLEAR`        | 清除快照规则     |

---

## 配置说明

配置文件格式为 `key=value`，支持 `#` 注释。默认加载 `./kvstore.conf`。

### 全部配置项


| 配置项                    | 默认值         | 说明                                          |
| ------------------------- | -------------- | --------------------------------------------- |
| `port`                    | `5000`         | 监听端口                                      |
| `role`                    | `master`       | 角色：`master` / `slave`                      |
| `master_host`             | `127.0.0.1`    | 主节点地址                                    |
| `master_port`             | `5000`         | 主节点端口                                    |
| `dump_path`               | `kvstore.dump` | dump 文件路径                                 |
| `aof_path`                | `kvstore.aof`  | AOF 文件路径                                  |
| `mem_backend`             | `libc`         | 内存后端：`libc` / `jemalloc` / `custom`      |
| `net_backend`             | `reactor`      | 网络模型：`reactor` / `proactor` / `ntyco`    |
| `log_mode`                | `info`         | 日志级别：`debug` / `info` / `warn` / `error` |
| `appendfsync`             | `always`       | AOF 同步：`always` / `everysec`               |
| `repl_transport_backend`  | `tcp`          | 复制传输（单模式）：`tcp` / `rdma` / `ebpf`   |
| `repl_fullsync_transport` | `rdma`         | 全量同步传输：`rdma` / `tcp`                  |
| `repl_realtime_transport` | `ebpf`         | 实时增量同步传输：`ebpf` / `tcp`              |
| `autosnap`                | 无             | 自动快照规则，如`60:1000,300:10`              |
| `sentinel`                | `0`            | 启用哨兵模式                                  |
| `sentinel_master_name`    | `mymaster`     | 哨兵监控名称                                  |
| `sentinel_quorum`         | `1`            | 哨兵法定人数                                  |

> 命令行参数优先级高于配置文件。
> **双通道模式**：设置 `repl_fullsync_transport=rdma` + `repl_realtime_transport=ebpf` 可使 RDMA 负责全量同步、eBPF 负责实时增量同步，两者同时工作。

### 命令行参数

```
./kvstore --config <path> --port <n> --role <master|slave>
          --mem <libc|jemalloc|custom> --net <reactor|proactor|ntyco>
          --log-mode <debug|info|warn|error> --dump <path> --aof <path>
          --master-host <ip> --master-port <n> --repl-transport <tcp|rdma>
          --repl-fullsync-transport <rdma|tcp> --repl-realtime-transport <ebpf|tcp>
          --sentinel --sentinel-master-name <name>
```

---

## 文档索引

| 文档 | 说明 |
|------|------|
| [`docs/tech-roadmap.md`](docs/tech-roadmap.md) | ⭐ **技术路线与实现详解** — 新手必读，覆盖所有模块的架构、流程图、代码 |
| [`docs/rdma-fullsync-implementation.md`](docs/rdma-fullsync-implementation.md) | RDMA 全量复制的代码级实现分析 |
| [`docs/plan.md`](docs/plan.md) | 项目演进规划（各阶段目标） |
| [`docs/iteration-summary.md`](docs/iteration-summary.md) | 迭代总结（含 RDMA 稳定性修复记录） |
| [`docs/examples/kvs_skiptable.c`](docs/examples/kvs_skiptable.c) | Skiptable 引擎 API 使用示例 |

---

## 测试体系

### 快速验证

```bash
make check        # 运行全部基础测试 (resp + ttl + persist + doc)
```

### C 测试程序 (`tests/`)

`tests/` 目录下包含独立的 C 测试程序，通过 RESP 协议连接 kvstore 进行自动化验证。

这些 C 测试程序**不依赖 hiredis 等第三方库**，直接通过 TCP socket 构造 RESP 协议报文，可在任何 Linux 环境下编译运行。

编译方式：
```bash
# 通过 Makefile
make test_kvstore              # → ./test_kvstore
make test_repl_5w5w            # → tests/test_repl_5w5w
make test_persist_dump_demo    # → ./test_persist_dump_demo
make test_persist_aof_demo     # → ./test_persist_aof_demo
make test_uring_persist        # → ./test_uring_persist
make test_mmap_recover         # → ./test_mmap_recover
make test_repl_basic           # → ./test_repl_basic

# 或手动编译
gcc -I./include -o test_kvstore tests/test_kvstore.c
```

---

#### `test_kvstore` — 全功能 C 客户端测试

```
编译: make test_kvstore
运行: ./test_kvstore <host> <port>
```

连接 kvstore 后依次测试 PING、各引擎 SET/GET/DEL、MSET/MGET、TTL/EXPIRE/PERSIST、
LOCK/UNLOCK/RENEW、DOC 命令、PING 批量流水线、SAVE/BGSAVE 持久化、INFO 命令，
最后输出 PASS/FAIL 汇总报告。

```bash
# 终端 1: 启动 kvstore（任意端口）
./kvstore --port 5000

# 终端 2: 运行全功能测试
./test_kvstore 127.0.0.1 5000

# 或通过 Makefile 自动启动 + 测试
make check-kvstore TEST_PORT=5000
```

**验证**: 测试通过后，用 redis-cli 确认数据正确：
```bash
redis-cli -p 5000 PING
+PONG
redis-cli -p 5000 GET a:pre:1
"av:1"
redis-cli -p 5000 HGET h:pre:100
"hv:100"
redis-cli -p 5000 INFO
# 查看 role、mem、dirty 等信息
```

---

#### `test_repl_5w5w` — 5w+5w 主从同步测试

```
编译: make test_repl_5w5w          # → tests/test_repl_5w5w
运行: tests/test_repl_5w5w [选项]
```

测试流程：预存 5w 条数据到 Master → 监控 Slave 全量同步(RDMA) → 再写 5w 条增量 → 监控增量同步(eBPF) → 验证 Slave 最终 10w 条数据一致性。

**启动顺序（重要）**: ① Master → ② 本脚本 → ③ Slave（看到"等待 Slave 连接"提示后再启动）

```bash
# ── 方式一: RDMA 全量 + eBPF 增量（双虚拟机）──

# 终端 1 (VM1, 先启动 Master):
sudo ./kvstore --port 5160 --role master \
    --repl-fullsync-transport rdma \
    --repl-realtime-transport ebpf \
    --rdma-dev siw0 --rdma-recv-slots 64

# 终端 2 (任意机器, Master 启动后运行):
./test_repl_5w5w --master-host 192.168.233.128 --master-port 5160 \
    --slave-host 192.168.233.129 --slave-port 5161 \
    --pre 50000 --post 50000

# 终端 3 (VM2, 看到"等待 Slave 连接..."后再启动 Slave):
# 先清理旧数据文件，避免上次测试残留影响
rm -f kvstore.dump kvstore.aof
sudo ./kvstore --port 5161 --role slave \
    --master-host 192.168.233.128 --master-port 5160 \
    --repl-fullsync-transport rdma \
    --repl-realtime-transport ebpf


# ── 方式二: TCP 全量（单机，无 RDMA/eBPF）──

# 终端 1 (先启动 Master):
./kvstore --port 5160 --role master \
    --repl-fullsync-transport tcp --repl-realtime-transport tcp

# 终端 2 (Master 启动后运行):
./test_repl_5w5w --master-host 127.0.0.1 --master-port 5160 \
    --slave-host 127.0.0.1 --slave-port 5161 \
    --pre 50000 --post 50000

# 终端 3 (看到提示后再启动 Slave):
# 先清理旧数据文件，避免上次测试残留影响
rm -f kvstore.dump kvstore.aof
./kvstore --port 5161 --role slave \
    --master-host 127.0.0.1 --master-port 5160 \
    --repl-fullsync-transport tcp --repl-realtime-transport tcp
```

选项说明：

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `--master-host HOST` | 127.0.0.1 | Master 地址 |
| `--master-port PORT` | 5160 | Master 端口 |
| `--slave-host HOST` | 127.0.0.1 | Slave 地址 |
| `--slave-port PORT` | 5161 | Slave 端口 |
| `--pre COUNT` | 50000 | 全量同步前预存数据量 |
| `--post COUNT` | 50000 | 全量同步后增量数据量 |
| `--batch SIZE` | 1000 | 每批写入量 |
| `--poll MS` | 500 | 轮询间隔毫秒 |

**验证**: 测试通过后，确认主从数据一致：
```bash
# 在 Master 上查询
redis-cli -p 5160 HGET h:pre:50000
"hv:50000"
redis-cli -p 5160 HGET h:post:50000
"hv_post:50000"
redis-cli -p 5160 GET a:pre:512
"av:512"
redis-cli -p 5160 RGET r:pre:500
"rv:500"
redis-cli -p 5160 XGET x:pre:999
"xv:999"

# 在 Slave 上查询（结果应与 Master 完全一致）
redis-cli -p 5161 HGET h:pre:50000
"hv:50000"
redis-cli -p 5161 HGET h:post:50000
"hv_post:50000"
redis-cli -p 5161 GET a:pre:512
"av:512"
redis-cli -p 5161 RGET r:pre:500
"rv:500"
redis-cli -p 5161 XGET x:pre:999
"xv:999"
```

---

#### `test_persist_dump_demo` — 全量持久化演示

```
编译: make test_persist_dump_demo    # → ./test_persist_dump_demo
运行: ./test_persist_dump_demo [选项]
```

交互式流程：连接 kvstore → 写入 count 条数据 → 提示用户执行 `SAVE` → 提示用户停止并重启 kvstore → 自动验证数据从 dump 文件恢复。

```bash
# 终端 1: 启动 kvstore（必须 --appendfsync always 确保数据可恢复）
./kvstore --port 5170 --role master --appendfsync always \
    --dump kvstore.dump --aof kvstore.aof

# 终端 2: 运行全量持久化演示
./test_persist_dump_demo --port 5170 --count 100000

# 程序会写入数据，然后提示你:
#   >>> Please execute SAVE in kvstore (redis-cli SAVE or nc ...)
# 在终端 1 执行 SAVE 后，程序继续提示:
#   >>> Please stop kvstore (Ctrl+C) and restart it
# 停止并重启 kvstore，程序自动检测重连并验证数据恢复
```

**验证**: SAVE 后、重启前，用 redis-cli 确认数据已持久化：
```bash
redis-cli -p 5170 SAVE
+OK
redis-cli -p 5170 HGET bench:key:1
"value:1"
redis-cli -p 5170 HGET bench:key:50000
"value:50000"
```

选项说明：

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `--host HOST` | 127.0.0.1 | kvstore 地址 |
| `--port PORT` | 5170 | kvstore 端口 |
| `--count N` | 50000 | 写入数据量 |
| `--batch N` | 1000 | 每批写入量 |

---

#### `test_persist_aof_demo` — 增量持久化演示 (AOF)

```
编译: make test_persist_aof_demo     # → ./test_persist_aof_demo
运行: ./test_persist_aof_demo [选项]
```

交互式流程：连接 kvstore → 写入 count 条数据（**不执行 SAVE**）→ 提示用户停止并重启 kvstore → 自动验证数据从 AOF 文件恢复。

> **重要**: kvstore 必须使用 `--appendfsync always`，确保每条写入即时落盘。
> 使用 `--appendfsync everysec` 时，停止前需等最多 1 秒落盘，可能导致数据丢失。

```bash
# 终端 1: 启动 kvstore（必须 --appendfsync always）
./kvstore --port 5170 --role master --appendfsync always \
    --dump kvstore.dump --aof kvstore.aof

# 终端 2: 运行增量持久化演示
./test_persist_aof_demo --port 5170 --count 100000

# 程序写入数据后提示:
#   >>> Please stop kvstore (Ctrl+C) and restart it
# 停止并重启 kvstore，程序自动验证 AOF 恢复（注意: 不执行 SAVE，数据仅靠 AOF）
```

**验证**: AOF 恢复后，确认重启前后的数据一致：
```bash
# 重启前验证
redis-cli -p 5170 HGET bench:key:1
"value:1"
redis-cli -p 5170 HGET bench:key:50000
"value:50000"

# 停止并重启 kvstore 后，再次验证（数据应仍在）
redis-cli -p 5170 HGET bench:key:1
"value:1"
redis-cli -p 5170 HGET bench:key:50000
"value:50000"
redis-cli -p 5170 PING
+PONG
```

选项说明：

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `--host HOST` | 127.0.0.1 | kvstore 地址 |
| `--port PORT` | 5170 | kvstore 端口 |
| `--count N` | 50000 | 写入数据量 |
| `--batch N` | 1000 | 每批写入量 |

---

#### `test_uring_persist` — io_uring 持久化验证

```
编译: make test_uring_persist       # → ./test_uring_persist
运行: ./test_uring_persist [选项]
```

自动管理 kvstore 进程生命周期，测试 io_uring 写入路径的持久化正确性与性能。

流程：自动启动 kvstore → HSET 写入 N 条数据 → SAVE → 停止 kvstore → 重启 → 验证数据恢复 → 输出性能指标。

```bash
# 终端 1: 启动 kvstore（先启动）
./kvstore --port 5180 --role master --appendfsync always

# 终端 2: 运行测试
./test_uring_persist --port 5180 --count 10000

# 程序写入数据后提示:
#   >>> 请停止 kvstore (Ctrl+C) 并重新启动 (相同参数)
# 停止并重启 kvstore，程序自动验证数据恢复
```

**验证**: 测试完成后，用 redis-cli 手动确认：
```bash
redis-cli -p 5180 HGET uring:key:1
"value:1"
redis-cli -p 5180 HGET uring:key:5000
"value:5000"
redis-cli -p 5180 HGET uring:key:10000
"value:10000"
redis-cli -p 5180 INFO | grep mem
# 查看内存后端和统计信息
```

选项说明：

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `--host HOST` | 127.0.0.1 | kvstore 地址 |
| `--port PORT` | 5180 | kvstore 端口 |
| `--count N` | 10000 | 写入数据量 |
| `--batch N` | 1000 | 每批写入量 |

---

#### `test_mmap_recover` — mmap 恢复验证

```
编译: make test_mmap_recover        # → ./test_mmap_recover
运行: ./test_mmap_recover [选项]
```

自动管理 kvstore 进程生命周期，验证启动时通过 mmap 恢复 dump 文件的正确性与性能。
支持指定存储引擎（array/hash/rbtree/skiptable）。

流程：自动启动 kvstore → 按指定引擎写入 N 条数据 → SAVE → 停止 → 重启并计时 → 从 INFO 读取恢复统计（mmap 尝试次数/成功次数/回退次数/耗时）→ 验证数据一致性。

```bash
# 终端 1: 启动 kvstore（先启动）
./kvstore --port 5190 --role master --appendfsync always

# 终端 2: 运行测试（hash 引擎, 10000 条）
./test_mmap_recover --port 5190 --engine hash --count 10000

# 程序写入数据后提示:
#   >>> 请停止 kvstore (Ctrl+C) 并重新启动 (相同参数)
# 停止并重启 kvstore，程序自动验证数据恢复并显示 mmap 统计

# 使用其他引擎
./test_mmap_recover --port 5190 --engine rbtree --count 5000
./test_mmap_recover --port 5190 --engine array --count 10000   # 自动限制 1024
```

**验证**: 测试完成后，确认各引擎数据恢复正确：
```bash
# Hash 引擎（--engine hash）
redis-cli -p 5190 HGET mmap:key:10000
"value:10000"
redis-cli -p 5190 HGET mmap:key:1
"value:1"

# RBTREE 引擎（--engine rbtree）
redis-cli -p 5190 RGET mmap:key:5000
"value:5000"

# Skiptable 引擎（--engine skiptable）
redis-cli -p 5190 XGET mmap:key:5000
"value:5000"

# Array 引擎（--engine array，上限 1024）
redis-cli -p 5190 GET mmap:key:1024
"value:1024"
```

选项说明：

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `--host HOST` | 127.0.0.1 | kvstore 地址 |
| `--port PORT` | 5190 | kvstore 端口 |
| `--count N` | 10000 | 写入数据量（array 引擎上限 1024） |
| `--engine NAME` | hash | 引擎: array/hash/rbtree/skiptable |
| `--batch N` | 1000 | 每批写入量 |

---

#### `test_repl_basic` — 主从复制基本验证

```
编译: make test_repl_basic          # → ./test_repl_basic
运行: ./test_repl_basic [选项]
```

用户手动管理 Master/Slave 进程，程序负责写入、监控、验证。

流程：用户启动 Master → 程序跨引擎（Hash/Array/RBTREE/Skiptable）写入 N 条数据 → 提示用户启动 Slave → 等待全量同步完成 → 再写入增量数据 → 等待增量同步 → 验证各引擎数据一致性。

```bash
# ── 单机三终端模式 ──

# 终端 1: 启动 Master（先启动）
./kvstore --port 6379 --role master \
    --repl-fullsync-transport tcp --repl-realtime-transport tcp

# 终端 2: 运行测试（Master 启动后运行）
./test_repl_basic --master-port 6379 --slave-port 6380 --count 5000

# 程序会写入数据到 Master，然后提示启动 Slave:
#   >>> 请在另一个终端启动 Slave: ...
# 此时在终端 3 启动 Slave:

# 终端 3: 启动 Slave（看到提示后再启动）
# 先清理旧数据文件，避免上次测试残留影响
rm -f kvstore.dump kvstore.aof
./kvstore --port 6380 --role slave \
    --master-host 127.0.0.1 --master-port 6379 \
    --repl-fullsync-transport tcp --repl-realtime-transport tcp


# ── 双机部署（跨机器测试）──

# 终端 1 (VM1, 先启动 Master):
./kvstore --port 6380 --role master \
    --repl-fullsync-transport tcp --repl-realtime-transport tcp

# 终端 2 (本地, Master 启动后运行):
./test_repl_basic --master-host 192.168.233.128 --master-port 6380 \
    --slave-host 192.168.233.129 --slave-port 6381 \
    --count 5000

# 终端 3 (VM2, 看到提示后再启动 Slave):
# 先清理旧数据文件，避免上次测试残留影响
rm -f kvstore.dump kvstore.aof
./kvstore --port 6381 --role slave \
    --master-host 192.168.233.128 --master-port 6380 \
    --repl-fullsync-transport tcp --repl-realtime-transport tcp
```

**验证**: 测试通过后，用 redis-cli 确认主从数据完全一致：
```bash
# 在 Master 上查询
redis-cli -p 6380 HGET h:pre:5000
"hv:5000"
redis-cli -p 6380 HGET h:pre:50
"hv:50"
redis-cli -p 6380 HGET h:post:891
"hv_post:891"
redis-cli -p 6380 HGET h:post:1232
(nil)                       # 只写了 1000 条增量(post)，1232 不存在正常
redis-cli -p 6380 GET a:pre:1
"av:1"
redis-cli -p 6380 RGET r:pre:500
"rv:500"
redis-cli -p 6380 XGET x:pre:999
"xv:999"

# 在 Slave 上查询（结果必须与 Master 完全一致）
redis-cli -p 6381 HGET h:pre:5000
"hv:5000"
redis-cli -p 6381 HGET h:pre:50
"hv:50"
redis-cli -p 6381 HGET h:post:891
"hv_post:891"
redis-cli -p 6381 GET a:pre:1
"av:1"
redis-cli -p 6381 RGET r:pre:500
"rv:500"
redis-cli -p 6381 XGET x:pre:999
"xv:999"

# 检查 Slave 只读（写操作应被拒绝）
redis-cli -p 6381 SET should_fail x
-ERR read only slave
```

两种验证方法：
- **全量同步验证**: 查询 `h:pre:*`（预存 5000 条）— 确认 Slave 有全部预存数据
- **增量同步验证**: 查询 `h:post:*`（全量完成后写入 1000 条）— 确认增量数据也同步到了 Slave
- **跨引擎验证**: 分别用 `GET`/`HGET`/`RGET`/`XGET` 确认 Array/Hash/RBTREE/Skiptable 四个引擎的数据都一致

选项说明：

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `--master-host HOST` | 127.0.0.1 | Master 地址 |
| `--slave-host HOST` | 127.0.0.1 | Slave 地址 |
| `--master-port PORT` | 6379 | Master 端口 |
| `--slave-port PORT` | 6380 | Slave 端口 |
| `--count N` | 5000 | 预存/增量数据量 |
| `--batch N` | 500 | 每批写入量 |
| `--poll MS` | 500 | 轮询间隔毫秒 |

---

#### 辅助文件

| 文件 | 说明 |
|------|------|
| **testcase.c** | 测试框架工具库，提供 `send_msg()` / `recv_msg()` / `testcase()` / `connect_tcpserver()` 等 RESP 协议测试辅助函数，作为其他 C 测试的依赖引用 |
| **test.c** | （空文件，预留） |

### 全部测试目标


| 命令                               | 数据量        | 说明                                                          | 产物路径                            |
| ---------------------------------- | ------------- | ------------------------------------------------------------- | ----------------------------------- |
| `make check-all`                   | 全部          | **一键运行全部测试**（自动探测 RDMA/eBPF 环境，跳过不可用项） | —                                  |
| `make check-all-quick`             | 小+1w         | 快速全套（跳过 RDMA/eBPF/复制/10w demo）                      | —                                  |
| `make check`                       | 小            | 基础功能全套                                                  | —                                  |
| `make check-resp`                  | —            | RESP 协议测试                                                 | —                                  |
| `make check-ttl`                   | —            | TTL 过期测试                                                  | —                                  |
| `make check-persist`               | —            | 持久化基本测试                                                | —                                  |
| `make check-doc`                   | —            | 文档对象测试                                                  | —                                  |
| `make check-kvstore`               | 小            | C 客户端综合测试（`tests/test_kvstore.c`）                    | —                                  |
| `make check-bulk-1w`               | **1w**        | 批量 1w 级全套回归（HSET/HGET/TTL/SAVE+恢复/DOC）             | —                                  |
| `make check-10w`                   | **1w~**       | 10w 级大容量功能测试                                          | —                                  |
| `make check-mass-ttl`              | 1w            | 海量 TTL 压测                                                 | —                                  |
| `make check-uring-persist`         | 1w            | io_uring 持久化验证（Python 脚本）                            | `artifacts/persist/uring-bench/`    |
| `make check-uring-persist-c`       | 1w            | io_uring 持久化验证（C 程序，自动管理进程）                   | `artifacts/persist/uring-bench/`    |
| `make check-mmap-recover`          | 1w            | mmap 恢复验证（Python 脚本）                                  | `artifacts/persist/mmap-recover/`   |
| `make check-mmap-recover-c`        | 1w            | mmap 恢复验证（C 程序，支持指定引擎，自动管理进程）           | `artifacts/persist/mmap-recover/`   |
| `make check-repl`                  | 5k            | 主从复制基本验证（shell 脚本）                                | —                                  |
| `make check-repl-basic`            | 5k            | 主从复制基本验证（C 程序，自动管理 Master/Slave 进程）        | —                                  |
| `make test_repl_5w5w`<br>（仅编译） | **5w+5w**     | 5w+5w 主从同步 C 测试（`tests/test_repl_5w5w.c`）<br>编译后手动运行：`tests/test_repl_5w5w --master-host H ...` | —                                  |
| `make check-repl-metrics`          | 5w+5k         | 复制指标基线                                                  | `artifacts/repl/metrics/`           |
| `make check-repl-profile`          | 5w+5k         | 复制 profiling                                                | `artifacts/repl/profile/`           |
| `make check-repl-ebpf`             | 5w+5k         | eBPF 实时同步 profiling                                       | `artifacts/repl/profile/`           |
| `make check-repl-ebpf-env`         | —            | eBPF 环境探测                                                 | —                                  |
| `make check-repl-ebpf-sync`        | 64            | eBPF sockmap 同步验证                                         | `artifacts/repl/ebpf-sync/`         |
| `make check-repl-ebpf-sync-required` | 64          | eBPF 同步验证（要求 eBPF 可用）                               | `artifacts/repl/ebpf-sync/`         |
| `make check-repl-ebpf-redirect`    | 64            | eBPF ingress 重定向验证                                       | `artifacts/repl/ebpf-sync/`         |
| `make check-repl-rdma-unsupported` | 小            | RDMA 不可用时的优雅降级测试                                   | —                                  |
| `make check-repl-rdma-smoke`       | 小            | RDMA 冒烟测试                                                 | `artifacts/repl/rdma-smoke/`        |
| `make check-repl-rdma-stress`      | 中            | RDMA 压力测试（重启轮次 + 尾写验证）                          | `artifacts/repl/rdma-stress/`       |
| `make check-repl-rdma-soak`        | 中            | RDMA 长时浸泡（可配小时级）                                   | `artifacts/repl/rdma-stress/`       |
| `make check-repl-rdma-long-soak`   | 中            | RDMA 超长浸泡（默认 1800s）                                   | `artifacts/repl/rdma-stress/`       |
| `make check-repl-rdma-fallback`    | 小            | RDMA 强制降级到 TCP 验证                                      | —                                  |
| `make check-demo-full-dump`        | **10w**       | 全量持久化演示                                                | `artifacts/persist/full-dump-demo/` |
| `make check-demo-incr-aof`         | **10w**       | 增量持久化演示                                                | `artifacts/persist/incr-aof-demo/`  |
| `make check-demo-repl-sync`        | **5w+5w=10w** | 主从同步演示（可配 RDMA+eBPF 混合传输）                       | `artifacts/repl/sync-demo/`         |
| `make check-rdma-standalone-probe` | —            | RDMA 环境探测                                                 | `artifacts/rdma/probe/`             |
| `make check-rdma-pingpong-smoke`   | —            | RDMA pingpong 测试                                            | `artifacts/rdma/pingpong/`          |

> **注意**：若之前使用 `sudo make check-demo-repl-sync` 运行过，`artifacts/repl/sync-demo/` 下的文件属主为 root，再次运行时需先 `sudo rm -rf artifacts/repl/sync-demo` 清理，否则会报 `PermissionError`。

### 辅助测试脚本（非 Makefile 目标）

以下脚本位于 `tools/` 目录下，可直接运行，未绑定 Makefile 目标：

| 脚本 | 位置 | 说明 |
|------|------|------|
| `test_master_slave_multi_engine_nc.sh` | `tools/tests/` | 多引擎主从复制 nc 测试（手动指定 host/port） |
| `test_kv.sh` | `tools/tests/` | kvstore 基本功能测试 |
| `run_save_bgsave_perf_test.sh` | `tools/persist/` | SAVE/BGSAVE 性能测试 |
| `repl_ebpf_session.py` | `tools/repl/` | eBPF 复制会话管理（交互式调试） |
| `run_repl_rdma_unsupported.py` | `tools/repl/` | RDMA 不可用场景模拟测试 |
| `repl_ebpf_daemon.c` | `tools/ebpf/` | eBPF 独立守护进程（需编译） |

示例：
```bash
# 多引擎主从测试
bash tools/tests/test_master_slave_multi_engine_nc.sh 127.0.0.1 5000

# SAVE/BGSAVE 性能测试
bash tools/persist/run_save_bgsave_perf_test.sh
```

### 参数化运行

```bash
# 指定端口
make check TEST_PORT=6380

# 主从复制自定义端口
make check-repl REPL_MASTER_PORT=7000 REPL_SLAVE_PORT=7001

# 10w 级测试自定义数据量
make check-10w CHECK_10W_COUNT=50000

# 海量 TTL 自定义规模
make check-mass-ttl MASS_TTL_KEYS=5000 MASS_TTL_SECONDS=2

# io_uring 持久化自定义参数
make check-uring-persist URING_PERSIST_COUNT=5000 URING_PERSIST_APPEND_FSYNC=everysec

# mmap 恢复指定引擎
make check-mmap-recover MMAP_RECOVER_ENGINE=hash MMAP_RECOVER_COUNT=20000

# 批量 1w 级回归自定义规模
make check-bulk-1w BULK_COUNT=50000

# eBPF 同步测试
make check-repl-ebpf-sync REPL_EBPF_SYNC_COUNT=128

# RDMA 压力测试自定义参数
make check-repl-rdma-stress REPL_RDMA_STRESS_PRELOAD=256 REPL_RDMA_STRESS_TAIL_WRITES=64 REPL_RDMA_STRESS_RESTART_ROUNDS=5

# RDMA 浸泡测试自定义时长
make check-repl-rdma-soak REPL_RDMA_SOAK_SECONDS=300 REPL_RDMA_SOAK_WRITE_INTERVAL_MS=100

# RDMA 长浸泡（30 分钟）
make check-repl-rdma-long-soak

# RDMA 可调参数测试（recv slots / chunk size / QP depth）
make check-repl-rdma-stress REPL_RDMA_TUNABLE_RECV_SLOTS=64 REPL_RDMA_TUNABLE_CHUNK_SIZE=65536 REPL_RDMA_TUNABLE_QP_WR_DEPTH=128

# RDMA 强制降级验证
make check-repl-rdma-fallback REPL_RDMA_FORCE_FALLBACK=1

# 全量同步演示自定义传输方式
make check-demo-repl-sync REPL_SYNC_DEMO_FULLSYNC=rdma REPL_SYNC_DEMO_REALTIME=ebpf

# 一键运行全部测试
make check-all

# 快速全套（跳过 RDMA/eBPF/复制/demo）
make check-all-quick

# 只跑特定目标
python3 tools/tests/run_all_tests.py --only check,check-bulk-1w,check-mass-ttl
```

---

## 测试产物路径

所有测试脚本的输出统一存放在 `artifacts/` 目录下，按测试类型分子目录。


| 测试场景            | 产物目录                            | 典型内容                |
| ------------------- | ----------------------------------- | ----------------------- |
| 全量持久化 10w 演示 | `artifacts/persist/full-dump-demo/` | dump 文件、验证日志     |
| 增量持久化 10w 演示 | `artifacts/persist/incr-aof-demo/`  | AOF 文件、验证日志      |
| io_uring 持久化验证 | `artifacts/persist/uring-bench/`    | 耗时报告、恢复日志      |
| mmap 恢复验证       | `artifacts/persist/mmap-recover/`   | 恢复时间报告            |
| 复制指标基线        | `artifacts/repl/metrics/`           | INFO 快照、CPU/RSS 摘要 |
| 复制 profiling      | `artifacts/repl/profile/`           | perf 数据、调用栈       |
| 主从同步 10w 演示   | `artifacts/repl/sync-demo/`         | 同步一致性报告          |
| eBPF 同步测试          | `artifacts/repl/ebpf-sync/`           | eBPF 日志、验证报告           |
| eBPF 同步测试(ingress) | `artifacts/repl/ebpf-sync/`           | ingress 重定向验证报告        |
| RDMA 冒烟测试          | `artifacts/repl/rdma-smoke/`          | RDMA 全量同步状态报告         |
| RDMA 压力/浸泡测试     | `artifacts/repl/rdma-stress/`         | 状态报告、fullsync 日志、重启日志 |
| RDMA 手动测试          | `artifacts/repl/rdma-manual/`         | 手动 RDMA 测试日志            |
| RDMA 环境探测          | `artifacts/rdma/probe/`               | 环境可用性报告                |
| RDMA pingpong       | `artifacts/rdma/pingpong/`          | 延迟/吞吐报告           |
| 基准测试            | `artifacts/bench/`                  | CSV 数据、图表          |

> 此外，`testdata/` 存放手工编写的静态测试数据（样例 AOF、dump 文件、测试用配置文件），不会被脚本覆盖。

---

## 性能基准

> **测试环境**：Intel Core Ultra 7 155H (4 vCPU) / 7.7GiB RAM / Ubuntu 20.04.6 / Linux 5.15.0-139 / KVM 虚拟机
> 
> **测试方法**：`python3 tools/bench/bench_mem_backend.py`，每轮 200 次预热，`HSET` 命令写入

### 基准数据 (HSET, 50k~100k ops)

| 后端     | ops    | value 大小 | 耗时(s) | QPS  | VmRSS(KB) |
|----------|--------|-----------|---------|------|-----------|
| libc     | 50000  | 128B      | 25.66   | 1949 | 16716     |
| jemalloc | 50000  | 128B      | 25.27   | 1978 | 20208     |
| custom   | 50000  | 128B      | 25.90   | 1931 | 26080     |
| libc     | 50000  | 4KB       | 29.07   | 1720 | 211332    |
| jemalloc | 50000  | 4KB       | 28.30   | 1767 | 264920    |
| custom   | 50000  | 4KB       | 29.67   | 1685 | 413856    |
| libc     | 100000 | 128B      | 50.95   | 1963 | 28416     |
| jemalloc | 100000 | 128B      | 50.79   | 1969 | 32524     |
| custom   | 100000 | 128B      | 51.78   | 1931 | 46792     |

> 完整数据：`benchmarks/data/bench_fresh.csv`

### 运行基准测试

```bash
# 一键运行全部组合（需 sudo，脚本自动启动/停止 kvstore）
bash tools/bench/run_all_benchmarks.sh

# 或单独指定参数
sudo python3 tools/bench/bench_mem_backend.py \
  --binary ./kvstore --base-port 6500 \
  --ops 50000 --value-size 128 \
  --backends libc,jemalloc,custom \
  --csv my_bench.csv
```

---

## 开发指南

### 添加新命令

1. 在 `src/main/kvstore.c` 的 `handle_parsed_command()` 中添加处理分支
2. 若需持久化，调用 `persist_note_write()` + `persist_append_raw()`
3. 若需复制广播，调用 `repl_broadcast()`
4. 在 `tests/integration/` 下补充测试脚本

### 添加新存储引擎

1. 在 `include/kvstore/kvstore.h` 定义引擎 ID 和数据结构
2. 在 `src/storage/` 下实现 CRUD 操作
3. 更新 `Makefile` 的 `SRCS` 列表
4. 在 `handle_parsed_command()` 中集成新引擎路由

### 编译选项

```bash
# RDMA 支持（默认开启）
make ENABLE_RDMA=1

# eBPF 支持（默认关闭）
make ENABLE_EBPF=1

# 编译零警告策略
make CFLAGS="-Wall -Wextra -O2"
```

---

## 常见问题

### jemalloc TLS 问题

若重启时出现 `static TLS block` 错误，系统会自动通过 `LD_PRELOAD` 重启进程，无需手动干预。

### 端口冲突

默认端口 5000，修改方式：

```bash
./kvstore --port 6380
# 或修改 kvstore.conf 中 port=6380
```

### 内存观测

```bash
printf '*1\r\n$7\r\nMEMSTAT\r\n' | nc 127.0.0.1 5000
```

关注指标：`current_small_inuse`、`peak_small_inuse`、`internal_fragment_ppm`。

### RDMA / eBPF 环境要求

- RDMA 使用 Soft-RoCE (`rxe0`) 验证，需要 `librdmacm-dev`、`libibverbs-dev`
- eBPF 需要 `libbpf-dev`、`libelf-dev`、`clang`，通常需要 root 权限
- 默认稳定复制路径为 TCP，RDMA/eBPF 为实验性扩展

---

## 许可证

本项目采用 [MIT 许可证](LICENSE)。

## 参考资源

- [Redis 协议规范](https://redis.io/topics/protocol)
- [io_uring 文档](https://unixism.net/loti/)
- [jemalloc 文档](http://jemalloc.net/)
- [NtyCo 协程库](https://github.com/wangbojing/NtyCo)

---

*最后更新：2026 年 5 月 14 日*
