# kprobe + RDMA 增量同步实现方案（v3）

> kprobe 透明拦截 TCP send 路径，捕获增量 RESP 数据，通过 BPF ringbuf 传递到用户态转发模块，再由 **RDMA WRITE（单边）** 直接写入 Slave 预置 MR 环形缓冲区。

---

## 目录

1. [整体架构](#1-整体架构)
2. [详细数据流](#2-详细数据流)
3. [核心设计要点](#3-核心设计要点)
4. [新增文件清单](#4-新增文件清单)
5. [修改文件清单](#5-修改文件清单)
6. [代码实现](#6-代码实现)
7. [关键数据结构](#7-关键数据结构)
8. [配置与构建](#8-配置与构建)
9. [验证方案](#9-验证方案)
10. [风险与边界](#10-风险与边界)

---

## 1. 整体架构

```
┌──────────────────────────────────────────────────────────────────────┐
│ Master 虚拟机                                                         │
│                                                                      │
│  kvstore Master 进程                                                  │
│    SET/DEL/... → handle_parsed_command()                             │
│      ↓ persist_append_raw (AOF)                                       │
│      ↓ repl_broadcast(raw, rawlen) ←── 完全不动，走正常 TCP send      │
│      ↓ queue_bytes() → on_write() → send(fd, raw, rawlen)            │
│                                                                      │
│  ┌── 内核态 ──────────────────────────────────────────────────────┐  │
│  │                                                                │  │
│  │  send() 系统调用触发 tcp_sendmsg(sk, msg, len)                 │  │
│  │    │                                                           │  │
│  │    ├─→ [正常 TCP 协议栈处理]  → 发到 Slave（保底路径）          │  │
│  │    │                                                           │  │
│  │    └─→ [kprobe 拦截] 检查 PID + fd 匹配 → 命中 replication 流量 │  │
│  │           │                                                    │  │
│  │           │ len ≤ 512B ✓ 完整读入 BPF 栈                        │  │
│  │           │ bpf_probe_read_kernel 从 msg_iter 读取 payload      │  │
│  │           │ bpf_ringbuf_output(&repl_ringbuf, data, len, 0)    │  │
│  │           │ 更新 kprobe_stats map                               │  │
│  │           ▼                                                    │  │
│  │  ┌──────────────────────────────┐                              │  │
│  │  │ BPF Ring Buffer (1MB)        │  ← 内核→用户态通道            │  │
│  │  └──────────────────────────────┘                              │  │
│  └───────────────────┬───────────────────────────────────────────┘  │
│                      │ 用户态                                        │
│  ┌───────────────────▼───────────────────────────────────────────┐  │
│  │ 用户态转发模块 (ring_buffer__poll)                              │  │
│  │                                                                │  │
│  │  kprobe_ringbuf_cb(void *ctx, void *data, size_t size)         │  │
│  │    1. wr_slot_acquire() → 获取空闲 RDMA WRITE slot              │  │
│  │    2. memcpy(slot->buf, data, size)                            │  │
│  │    3. ibv_post_send(IBV_WR_RDMA_WRITE,                         │  │
│  │         .remote_addr = slave MR 数据槽地址,                      │  │
│  │         .rkey = slave MR rkey)                                  │  │
│  │    4. ibv_post_send(IBV_WR_RDMA_WRITE,                         │  │
│  │         写入 slave MR producer_head, 带 FENCE)                   │  │
│  │    5. CQ poll 回收 slot                                         │  │
│  └──────────────────┬────────────────────────────────────────────┘  │
│                      │ RDMA WRITE (单边)                             │
│                      │ DMA 直达 Slave 内存，Slave CPU 零参与          │
└──────────────────────┼──────────────────────────────────────────────┘
                       │ RDMA 网络
                       ▼
┌──────────────────────────────────────────────────────────────────────┐
│ Slave 虚拟机                                                          │
│                                                                      │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │ 预置 MR 环形缓冲区 (带 IBV_ACCESS_REMOTE_WRITE)              │    │
│  │ 建链时注册，通过握手响应将 rkey + addr 告知 Master              │    │
│  │                                                             │    │
│  │  ┌──────────┬────────────────────────────────────────┐      │    │
│  │  │ head(8B) │ tail(8B) │ slot[0] | slot[1] | ...    │      │    │
│  │  │ RDMA写   │ 本地更新  │ 每个 512B                    │      │    │
│  │  └──────────┴────────────────────────────────────────┘      │    │
│  └──────────────────────┬──────────────────────────────────────┘    │
│                          │                                          │
│  ┌───────────────────────▼──────────────────────────────────────┐  │
│  │ Slave 轮询线程                                                │  │
│  │                                                              │  │
│  │  while (running) {                                           │  │
│  │    __sync_synchronize();  // 读内存屏障                        │  │
│  │    head = rb->producer_head;  // Master 通过 RDMA WRITE 更新  │  │
│  │    tail = rb->consumer_tail;                                  │  │
│  │                                                              │  │
│  │    while (tail != head) {                                     │  │
│  │      slot = &rb->slots[tail % N]                              │  │
│  │      len = *(uint32_t *)slot;  // 前 4 字节为长度              │  │
│  │      data = slot + 4;                                         │  │
│  │      parse_resp_stream(NULL, data, &len, 1);                  │  │
│  │      → handle_parsed_command(from_replication=1)              │  │
│  │      → 写入本地引擎 + AOF                                     │  │
│  │      tail++;                                                  │  │
│  │    }                                                          │  │
│  │    rb->consumer_tail = tail;  // 本地更新                     │  │
│  │    usleep(POLL_INTERVAL_US);                                  │  │
│  │  }                                                            │  │
│  └──────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 2. 详细数据流

### 一次 SET 命令的完整路径

```
Client:    SET k001 value001\r\n

Step 1 — Master 命令处理 (用户态)
  kvstore: recv → parse_resp_stream → handle_parsed_command()
    → engine_set("k001", "value001")
    → persist_append_raw(raw, 45)       ← AOF
    → repl_broadcast(raw, 45)           ← 广播给 replica
    → queue_bytes(replica_fd, raw, 45)
    → on_write() → send(replica_fd, raw, 45, 0)
    └─ (45 字节 RESP: *3\r\n$3\r\nSET\r\n$4\r\nk001\r\n$9\r\nvalue001\r\n)

Step 2 — kprobe 内核拦截
  内核: tcp_sendmsg(sk, msg, 45)
    → kprobe/tcp_sendmsg 触发:
      1. pid == kvstore_pid ✓
      2. fd == replication_fd ✓
      3. len = 45, 45 < 512 ✓ 可完整读取
      4. bpf_probe_read_kernel(stack_buf, 45, msg->msg_iter 数据)
      5. bpf_ringbuf_output(&repl_ringbuf, stack_buf, 45, BPF_RB_NO_WAKEUP)
      6. 更新 stats: hit_count++, hit_bytes += 45
    → 正常 TCP 协议栈继续处理 (作为保底路径)

Step 3 — Ringbuf → 用户态转发模块
  ring_buffer__poll() 检测到新数据
    → kprobe_ringbuf_cb(NULL, data, 45) 回调:
      1. wr_slot_acquire(5000, &slot) → 获取 slot #3
      2. memcpy(g_wr_slots[3].buf, data, 45)
      3. ibv_post_send(RDMA_WRITE,
           remote_addr = slave_ringbuf + 16 + (slot_idx * 512),
           rkey = slave_rkey)
      4. ibv_post_send(RDMA_WRITE,
           remote_addr = slave_ringbuf + 0,  (producer_head)
           data = new_head,
           rkey = slave_rkey, FENCE)

Step 4 — RDMA 网络传输
  RDMA 网卡:
    → DMA 读取 g_wr_slots[3].buf (45 字节)
    → 通过 RDMA RC 连接传输
    → DMA 写入 Slave MR 环形缓冲区 slot[head % N]

Step 5 — Slave 轮询消费
  Slave 轮询线程:
    while (1):
      head = rb->producer_head  (已更新到 1024)
      while (tail=1023 < head=1024):
        slot = &rb->slots[1023 % 1024]
        memcpy(&len, slot, 4)  → len=45
        parse_resp_stream(NULL, slot+4, &len, 1)
          → handle_parsed_command(from_replication=1)
          → engine_set("k001", "value001")
          → persist_append_raw(raw, 45)  ← Slave 也写 AOF
        tail++  → 1024
      rb->consumer_tail = 1024
      __sync_synchronize()
      usleep(100)
```

### 路径对比

```
TCP 保底路径（始终运行）:
  Master send() → 内核 TCP 栈 → 网卡 → Slave 网卡 → 内核 TCP 栈 → recv() → parse

RDMA 主路径（kprobe 透明拦截）:
  Master send() → kprobe/tcp_sendmsg → ringbuf → RDMA WRITE → Slave MR → 轮询 → parse

Slave 侧两条路径关系:
  • 优先消费 MR 环形缓冲区中的数据（低延迟、零拷贝）
  • TCP 路径数据到达后，通过 repl_offset 比较去重
  • RDMA 路径正常时，TCP 数据到 Slave 时已经被消费过，直接跳过
```

---

## 3. 核心设计要点

### 3.1 kprobe 透明拦截

| 要点 | 说明 |
|------|------|
| **Hook 点** | `kprobe/tcp_sendmsg` |
| **过滤条件** | PID == kvstore 进程 PID，fd == replication 连接 fd |
| **数据读取** | `bpf_probe_read_kernel()` 从 `msg_iter` 读取 payload 到 BPF 栈（≤512B ✓） |
| **输出** | `bpf_ringbuf_output(&repl_ringbuf, data, len, flags)` |
| **副作用** | return 0，不阻止 TCP 正常发送 |
| **对 `repl_broadcast()` 的影响** | **零**——完全透明，无需修改任何 kvstore 代码 |

### 3.2 RDMA WRITE 单边操作

```
ibv_post_send 第 1 次: 写入数据
  opcode      = IBV_WR_RDMA_WRITE
  remote_addr = slave_ringbuf_data_base + slot_idx * slot_capacity
  rkey        = slave_rkey
  send_flags  = IBV_SEND_SIGNALED | IBV_SEND_FENCE
  length      = payload_len + 4  (4 字节长度头 + 数据)

ibv_post_send 第 2 次: 更新 producer_head
  opcode      = IBV_RDMA_WRITE
  remote_addr = slave_ringbuf_base + 0  (producer_head 位于偏移 0)
  rkey        = slave_rkey
  send_flags  = IBV_SEND_SIGNALED
  data        = new_head (8 字节, 当前生产者序号)
```

**FENCE 的作用**：保证第 1 次 WRITE（数据）在第 2 次 WRITE（head）之前**对远端可见**。没有 FENCE，head 可能先于数据到达 Slave，导致 Slave 读到不完整数据。

### 3.3 Slave 轮询去重

由于 TCP 保底路径始终运行，Slave 可能收到两条相同的数据。去重策略：

```c
// Slave 侧维护已消费的最大 offset
// MR 中的每条数据前 4 字节 = payload_len
// 第 5~8 字节 = repl_offset（由 kprobe 附加）

// 轮询消费时记录最大 offset
uint64_t last_applied_offset = g_slave_repl_applied_offset;

// TCP 路径到达时比较
if (current_offset <= last_applied_offset) {
    // 已被 RDMA 路径消费过，跳过
    return 0;
}
```

### 3.4 为什么增量数据量小是重要前提

| 场景 | 典型大小 | 能否用 kprobe 完整拦截 |
|------|---------|---------------------|
| SET key value | ~30-200 字节 | ✅ 远小于 512B |
| DEL key | ~20-50 字节 | ✅ |
| EXPIRE key ttl | ~30-60 字节 | ✅ |
| DOCSET key field val | ~50-200 字节 | ✅ |
| LOCK key owner ttl | ~40-80 字节 | ✅ |
| 批量 MSET (100 keys) | ~2000-5000 字节 | ❌ 超出 512B，需分段 |

如果未来需要支持大命令，kprobe 可退化为"只拷贝前 512B + 记录总长度"，剩余数据由用户态补全，但当前增量场景不需要。

---

## 4. 新增文件清单

| 文件 | 用途 |
|------|------|
| `src/replication/bpf/repl_kprobe.bpf.c` | BPF kprobe 程序：hook tcp_sendmsg，拦截 replication 数据写 ringbuf |
| `src/replication/kvs_repl_kprobe.c` | 转发模块：BPF 加载、ringbuf 消费、RDMA WRITE 发送、Slave 轮询 |
| `include/kvstore/replication/repl_kprobe.h` | 头文件 |
| `tools/repl/run_repl_kprobe_rdma_smoke.py` | 冒烟测试 |
| `tools/repl/run_repl_kprobe_rdma_stress.py` | 压力测试 |

## 5. 修改文件清单

| 文件 | 修改内容 |
|------|---------|
| `Makefile` | BPF 编译目标、C 源文件、链接 `-lbpf` |
| `include/kvstore/kvstore.h` | 配置项、函数声明、`KVS_REPL_TRANSPORT_KPROBE_RDMA` |
| `src/main/kvstore.c` | 配置解析、初始化调用、`INFO kprobe` 输出 |
| `src/replication/kvs_repl.c` | 新增 transport ops、建链握手扩展（MR 信息交换） |

注意：**`repl_broadcast()` 不需要修改**——kprobe 在 TCP send 路径上透明拦截。

---

## 6. 代码实现

### 6.1 kprobe BPF 程序

**文件**: `src/replication/bpf/repl_kprobe.bpf.c`

```c
// ============================================================
// repl_kprobe.bpf.c
//
// 功能: kprobe/tcp_sendmsg 拦截 replication 流量
// 输入: tcp_sendmsg(sk, msg, len) 的参数
// 输出: BPF ringbuf — 拦截到的 RESP 数据
// 限制: 单次最大拦截 512 字节（BPF 栈限制）
//       当前增量数据量小，单次足够
// ============================================================

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include <linux/socket.h>

/* ---- BPF Maps ---- */

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 8);
    __type(key, __u32);
    __type(value, __u64);
} kprobe_ctl SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 16);
    __type(key, __u32);
    __type(value, __u64);
} kprobe_stats SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 20);  /* 1MB */
} repl_ringbuf SEC(".maps");

/* ---- Control Keys ---- */
#define KVS_KPROBE_CTL_ENABLED     0  /* 0/1 */
#define KVS_KPROBE_CTL_PID         1  /* kvstore 进程 PID */
#define KVS_KPROBE_CTL_REPL_FD     2  /* replication 连接 fd */

/* ---- Stats Keys ---- */
#define KVS_KPROBE_STAT_HIT_COUNT  0  /* 拦截命中次数 */
#define KVS_KPROBE_STAT_HIT_BYTES  1  /* 拦截总字节数 */
#define KVS_KPROBE_STAT_SKIP_PID   2  /* PID 不匹配跳过 */
#define KVS_KPROBE_STAT_SKIP_FD    3  /* fd 不匹配跳过 */
#define KVS_KPROBE_STAT_SKIP_SIZE  4  /* 超长跳过 */
#define KVS_KPROBE_STAT_RINGBUF_ERR 5 /* ringbuf 写入失败 */

/* ringbuf 数据格式:
 *   [4 字节: payload_len (uint32_t)]
 *   [8 字节: repl_offset (uint64_t)]
 *   [payload_len 字节: RESP 协议数据]  */
struct ringbuf_entry {
    __u32 len;
    __u64 offset;
    unsigned char data[480];  /* 512 - 4 - 8 - 填充 */
};

SEC("kprobe/tcp_sendmsg")
int kprobe_kvs_repl_tcp_sendmsg(struct pt_regs *ctx)
{
    __u64 *enabled, *target_pid, *target_fd;
    __u64 *stat;

    /* 1. 检查开关 */
    enabled = bpf_map_lookup_elem(&kprobe_ctl, &(__u32){KVS_KPROBE_CTL_ENABLED});
    if (!enabled || !*enabled) return 0;

    /* 2. PID 过滤 */
    __u32 pid = bpf_get_current_pid_tgid() >> 32;
    target_pid = bpf_map_lookup_elem(&kprobe_ctl, &(__u32){KVS_KPROBE_CTL_PID});
    if (!target_pid) return 0;
    if (pid != (__u32)(*target_pid)) {
        stat = bpf_map_lookup_elem(&kprobe_stats, &(__u32){KVS_KPROBE_STAT_SKIP_PID});
        if (stat) __sync_fetch_and_add(stat, 1);
        return 0;
    }

    /* 3. 获取参数 */
    struct sock *sk = (struct sock *)PT_REGS_PARM1(ctx);
    struct msghdr *msg = (struct msghdr *)PT_REGS_PARM2(ctx);
    size_t len = (size_t)PT_REGS_PARM3(ctx);

    if (len == 0) return 0;

    /* 4. 检查大小（≤512B，BPF 栈限制） */
    if (len > 480) {  /* 留出头部空间 */
        stat = bpf_map_lookup_elem(&kprobe_stats, &(__u32){KVS_KPROBE_STAT_SKIP_SIZE});
        if (stat) __sync_fetch_and_add(stat, 1);
        return 0;
    }

    /* 5. 从 msg_iter 读取数据 */
    struct ringbuf_entry entry = {};
    entry.len = (__u32)len;

    /* 简化: 读取 msg->msg_iter.iov->iov_base
     * 实际需要遍历 iov_iter，这里用 bpf_probe_read_kernel 逐段读取 */
    struct iov_iter iter;
    bpf_probe_read_kernel(&iter, sizeof(iter), &msg->msg_iter);
    /* ... iov_iter 遍历逻辑 ... */
    /* 实际实现时使用 bpf_probe_read_kernel_str 或逐 iov 读取 */

    /* 6. 写入 ringbuf */
    if (bpf_ringbuf_output(&repl_ringbuf, &entry,
            sizeof(entry.__u32) + sizeof(entry.__u64) + len, 0) != 0) {
        stat = bpf_map_lookup_elem(&kprobe_stats,
            &(__u32){KVS_KPROBE_STAT_RINGBUF_ERR});
        if (stat) __sync_fetch_and_add(stat, 1);
        return 0;
    }

    /* 7. 更新统计 */
    stat = bpf_map_lookup_elem(&kprobe_stats, &(__u32){KVS_KPROBE_STAT_HIT_COUNT});
    if (stat) __sync_fetch_and_add(stat, 1);
    stat = bpf_map_lookup_elem(&kprobe_stats, &(__u32){KVS_KPROBE_STAT_HIT_BYTES});
    if (stat) __sync_fetch_and_add(stat, len);

    return 0;  /* 放行 TCP 正常发送 */
}

char LICENSE[] SEC("license") = "GPL";
```

### 6.2 用户态转发模块

**文件**: `src/replication/kvs_repl_kprobe.c`

```c
// ============================================================
// kvs_repl_kprobe.c
//
// Master 侧:
//   1. 加载 BPF，attach kprobe/tcp_sendmsg
//   2. 设置 PID/fd 过滤
//   3. ring_buffer__poll 消费 ringbuf
//   4. RDMA WRITE 发送到 Slave MR
//
// Slave 侧:
//   1. 注册 MR（带 REMOTE_WRITE）
//   2. 建链响应中返回 MR 信息
//   3. 轮询线程消费 MR 环形缓冲区
// ============================================================

#include "kvstore/kvstore.h"
#include "kvstore/replication/repl_kprobe.h"

#if KVS_ENABLE_KPROBE_RDMA

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

/* ---- 常量 ---- */
#define KVS_KPROBE_RINGBUF_POLL_MS     5
#define KVS_RDMA_WRITE_SLOTS           8
#define KVS_RDMA_WRITE_SLOT_SIZE       512
#define KVS_KPROBE_RDMA_POLL_US        100

#define KPROBE_RDMA_SLOT_COUNT         1024
#define KPROBE_RDMA_SLOT_CAPACITY      512
#define KPROBE_RDMA_RINGBUF_SIZE       (16 + KPROBE_RDMA_SLOT_COUNT * KPROBE_RDMA_SLOT_CAPACITY)

/* ---- Slave MR 环形缓冲区信息（Master 侧） ---- */
static struct {
    uint64_t remote_data_base;     /* Slave slots 基地址 (rb + 16) */
    uint32_t rkey;                 /* Slave MR rkey */
    uint64_t remote_head_addr;     /* Slave rb->producer_head 地址 */
    size_t   slot_count;
    size_t   slot_capacity;
} g_slave_mr;

/* ---- RDMA WRITE Slot ---- */
typedef struct rdma_write_slot_s {
    unsigned char buf[KVS_RDMA_WRITE_SLOT_SIZE] __attribute__((aligned(64)));
    struct ibv_mr *mr;
    volatile int in_flight;
} rdma_write_slot_t;

static rdma_write_slot_t g_wr_slots[KVS_RDMA_WRITE_SLOTS];
static int g_wr_head = 0;
static int g_wr_in_flight = 0;

/* ---- RDMA 上下文（独立 QP） ---- */
static struct {
    struct rdma_event_channel *ec;
    struct rdma_cm_id *id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_comp_channel *comp_chan;
    volatile int connected;
} g_rdma_kprobe;

/* ---- BPF 资源 ---- */
static struct bpf_object *g_kprobe_obj = NULL;
static int g_kprobe_ctl_fd = -1;
static int g_kprobe_stats_fd = -1;
static struct ring_buffer *g_kprobe_ringbuf = NULL;
static volatile int g_kprobe_running = 0;

/* ---- Slave 侧环形缓冲区 ---- */
static kprobe_rdma_ringbuf_t *g_slave_ringbuf = NULL;
static struct ibv_mr *g_slave_ringbuf_mr = NULL;

/* ---- 统计 ---- */
static unsigned long long g_total_events = 0;
static unsigned long long g_total_bytes = 0;
static unsigned long long g_rdma_writes = 0;
static unsigned long long g_rdma_errors = 0;

/* ============================================================
 * RDMA WRITE 操作
 * ============================================================ */

static int wr_slot_acquire(int timeout_ms, int *out) {
    long long deadline = timeout_ms > 0 ? kvs_now_ms() + timeout_ms : 0;
retry:
    for (int i = 0; i < KVS_RDMA_WRITE_SLOTS; i++) {
        int idx = (g_wr_head + i) % KVS_RDMA_WRITE_SLOTS;
        if (!g_wr_slots[idx].in_flight) {
            g_wr_head = (idx + 1) % KVS_RDMA_WRITE_SLOTS;
            g_wr_slots[idx].in_flight = 1;
            *out = idx;
            return 0;
        }
    }
    /* Poll CQ 回收已完成 slot */
    if (g_rdma_kprobe.cq) {
        struct ibv_wc wc;
        while (ibv_poll_cq(g_rdma_kprobe.cq, 1, &wc) > 0) {
            if (wc.status == IBV_WC_SUCCESS) {
                int slot = (int)(wc.wr_id & 0xFFFF);
                if (slot < KVS_RDMA_WRITE_SLOTS) {
                    g_wr_slots[slot].in_flight = 0;
                    g_wr_in_flight--;
                }
            }
            goto retry;
        }
    }
    if (timeout_ms > 0 && kvs_now_ms() >= deadline) return -1;
    usleep(100);
    goto retry;
}

/* 提交 RDMA WRITE: 写数据到 Slave 槽位 */
static int wr_submit_data(int slot, size_t len) {
    struct ibv_sge sge;
    struct ibv_send_wr wr, *bad = NULL;
    uint64_t slot_idx = (uint64_t)(g_wr_in_flight % g_slave_mr.slot_count);

    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)g_wr_slots[slot].buf;
    sge.length = (uint32_t)len;
    sge.lkey = g_wr_slots[slot].mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = (uint64_t)slot;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_FENCE;
    wr.wr.rdma.remote_addr = g_slave_mr.remote_data_base
                           + slot_idx * g_slave_mr.slot_capacity;
    wr.wr.rdma.rkey = g_slave_mr.rkey;

    return ibv_post_send(g_rdma_kprobe.id->qp, &wr, &bad);
}

/* 提交 RDMA WRITE: 更新 Slave producer_head */
static int wr_submit_head(int slot) {
    struct ibv_sge sge;
    struct ibv_send_wr wr, *bad = NULL;
    uint64_t new_head = (uint64_t)(g_wr_in_flight + 1);

    /* 复用 slot buf 前 8 字节存 new_head */
    memcpy(g_wr_slots[slot].buf, &new_head, 8);

    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)g_wr_slots[slot].buf;
    sge.length = 8;
    sge.lkey = g_wr_slots[slot].mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = (uint64_t)slot | 0x10000;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = g_slave_mr.remote_head_addr;
    wr.wr.rdma.rkey = g_slave_mr.rkey;

    return ibv_post_send(g_rdma_kprobe.id->qp, &wr, &bad);
}

/* ============================================================
 * Ringbuf 回调
 * ============================================================ */
static int kprobe_ringbuf_cb(void *ctx, void *data, size_t size) {
    (void)ctx;

    if (size < 12) return 0;  /* 至少 len(4) + offset(8) */
    uint32_t payload_len;
    memcpy(&payload_len, data, 4);
    if (payload_len == 0 || payload_len + 12 > size) return 0;

    g_total_events++;
    g_total_bytes += payload_len;

    int slot;
    if (wr_slot_acquire(5000, &slot) != 0) {
        g_rdma_errors++;
        return -1;  /* 暂停 ringbuf 回调 */
    }

    /* 构造: [4B len][payload] */
    size_t write_len = payload_len + 4;
    memcpy(g_wr_slots[slot].buf, &payload_len, 4);
    memcpy(g_wr_slots[slot].buf + 4, (unsigned char *)data + 12, payload_len);

    /* Step 1: RDMA WRITE 数据 */
    if (wr_submit_data(slot, write_len) != 0) {
        g_wr_slots[slot].in_flight = 0;
        g_rdma_errors++;
        return -1;
    }

    /* Step 2: RDMA WRITE 更新 producer_head */
    if (wr_submit_head(slot) != 0) {
        g_rdma_errors++;
        return -1;
    }

    g_wr_in_flight++;
    g_rdma_writes++;
    return 0;
}

/* ============================================================
 * Slave 轮询线程
 * ============================================================ */
static void *kprobe_rdma_slave_poll(void *arg) {
    (void)arg;
    kprobe_rdma_ringbuf_t *rb = g_slave_ringbuf;
    unsigned char stream_buf[BUFFER_CAP];
    size_t stream_len = 0;

    while (g_kprobe_running) {
        __sync_synchronize();
        uint64_t head = rb->producer_head;
        uint64_t tail = rb->consumer_tail;

        if (tail == head) {
            usleep(KVS_KPROBE_RDMA_POLL_US);
            continue;
        }

        while (tail != head) {
            size_t idx = tail % KPROBE_RDMA_SLOT_COUNT;
            size_t off = idx * KPROBE_RDMA_SLOT_CAPACITY;

            uint32_t slot_len;
            memcpy(&slot_len, rb->slots + off, 4);
            if (slot_len == 0 || slot_len > KPROBE_RDMA_SLOT_CAPACITY - 4) {
                tail++;
                continue;
            }

            unsigned char *slot_data = rb->slots + off + 4;
            if (stream_len + slot_len > sizeof(stream_buf))
                stream_len = 0;
            memcpy(stream_buf + stream_len, slot_data, slot_len);
            stream_len += slot_len;
            parse_resp_stream(NULL, stream_buf, &stream_len, 1);
            tail++;
        }

        rb->consumer_tail = tail;
        __sync_synchronize();
    }
    return NULL;
}

/* ============================================================
 * Master 转发线程
 * ============================================================ */
static void *kprobe_rdma_forward_thread(void *arg) {
    (void)arg;
    while (g_kprobe_running && g_rdma_kprobe.connected) {
        if (g_kprobe_ringbuf) {
            int err = ring_buffer__poll(g_kprobe_ringbuf,
                KVS_KPROBE_RINGBUF_POLL_MS);
            if (err < 0) usleep(1000);
        } else usleep(10000);
    }
    return NULL;
}

/* ============================================================
 * 对外接口
 * ============================================================ */

/* 初始化 Master 侧 kprobe + RDMA */
int repl_kprobe_rdma_master_init(void) {
    /* 1. 打开 BPF 对象文件 */
    /* 2. 加载并 attach kprobe/tcp_sendmsg */
    /* 3. 设置 PID 过滤 */
    /* 4. 分配 RDMA WRITE slots */
    for (int i = 0; i < KVS_RDMA_WRITE_SLOTS; i++) {
        g_wr_slots[i].in_flight = 0;
    }
    return 0;
}

/* 初始化 Slave 侧 */
int repl_kprobe_rdma_slave_init(void) {
    g_kprobe_running = 1;
    return 0;
}

/* 建链: Master 侧建立 RDMA QP 并交换 MR */
int repl_kprobe_rdma_establish(const char *host, int port) {
    (void)host; (void)port;
    /* 1. 建立 RDMA QP (独立 QP) */
    /* 2. 通过 TCP 控制通道发送 REPLSYNC */
    /* 3. 从响应解析 Slave MR 信息 (rkey, addr, size) */
    /* 4. 启动转发线程 */
    return 0;
}

/* 建链: Slave 侧注册 MR 并返回握手响应 */
int repl_kprobe_rdma_slave_accept(struct ibv_pd *pd,
    char *resp, size_t resp_cap)
{
    g_slave_ringbuf = (kprobe_rdma_ringbuf_t *)
        kvs_malloc(KPROBE_RDMA_RINGBUF_SIZE);
    if (!g_slave_ringbuf) return -1;
    memset(g_slave_ringbuf, 0, KPROBE_RDMA_RINGBUF_SIZE);

    g_slave_ringbuf_mr = ibv_reg_mr(pd, g_slave_ringbuf,
        KPROBE_RDMA_RINGBUF_SIZE,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!g_slave_ringbuf_mr) {
        kvs_free(g_slave_ringbuf);
        g_slave_ringbuf = NULL;
        return -1;
    }

    g_slave_ringbuf->producer_head = 0;
    g_slave_ringbuf->consumer_tail = 0;

    snprintf(resp, resp_cap,
        "+KPROBERDMA %u %lu %zu %zu %zu\r\n",
        g_slave_ringbuf_mr->rkey,
        (unsigned long)g_slave_ringbuf,
        (size_t)KPROBE_RDMA_RINGBUF_SIZE,
        (size_t)KPROBE_RDMA_SLOT_COUNT,
        (size_t)KPROBE_RDMA_SLOT_CAPACITY);

    /* 启动轮询线程 */
    pthread_t tid;
    pthread_create(&tid, NULL, kprobe_rdma_slave_poll, NULL);
    pthread_detach(tid);

    return 0;
}

/* 清理 */
void repl_kprobe_rdma_cleanup(void) {
    g_kprobe_running = 0;
    if (g_kprobe_ringbuf) {
        ring_buffer__free(g_kprobe_ringbuf);
        g_kprobe_ringbuf = NULL;
    }
    if (g_kprobe_obj) {
        bpf_object__close(g_kprobe_obj);
        g_kprobe_obj = NULL;
    }
    /* RDMA 资源清理 */
    if (g_slave_ringbuf_mr) {
        ibv_dereg_mr(g_slave_ringbuf_mr);
        g_slave_ringbuf_mr = NULL;
    }
    if (g_slave_ringbuf) {
        kvs_free(g_slave_ringbuf);
        g_slave_ringbuf = NULL;
    }
}

/* 获取 kprobe 统计信息（从 BPF map 读取） */
int repl_kprobe_rdma_get_stats(kvs_repl_kprobe_stats_t *stats) {
    if (!stats) return -1;
    memset(stats, 0, sizeof(*stats));
    stats->total_events = g_total_events;
    stats->total_bytes = g_total_bytes;
    stats->rdma_writes = g_rdma_writes;
    stats->rdma_errors = g_rdma_errors;
    /* 从 BPF kprobe_stats map 读取 kprobe 命中数 */
    return 0;
}

#endif /* KVS_ENABLE_KPROBE_RDMA */
```

### 6.3 Transport ops 集成

在 `src/replication/kvs_repl.c` 中新增：

```c
#define KVS_REPL_TRANSPORT_KPROBE_RDMA 4

static int repl_transport_kprobe_rdma_send(conn_t *c,
    const unsigned char *buf, size_t len) {
    (void)c; (void)buf; (void)len;
    /* kprobe 透明拦截，此 send 函数不应被调用。
     * 如果被调用（回退场景），走 TCP 路径 */
    return -1;
}

static int repl_transport_kprobe_rdma_connect_slave(
    const char *host, int port) {
    return repl_kprobe_rdma_establish(host, port);
}

static void repl_transport_kprobe_rdma_disconnect_slave(int fd) {
    (void)fd;
    repl_kprobe_rdma_cleanup();
}

static const repl_transport_ops_t g_repl_transport_kprobe_rdma_ops = {
    .name = "kprobe-rdma",
    .supported = KVS_ENABLE_KPROBE_RDMA,
    .send = repl_transport_kprobe_rdma_send,
    .connect_slave = repl_transport_kprobe_rdma_connect_slave,
    .disconnect_slave = repl_transport_kprobe_rdma_disconnect_slave,
};
```

### 6.4 建链握手协议扩展

```
Master → Slave (TCP 控制通道):
  REPLSYNC ? 0 0

Slave → Master:
  +KPROBERDMA <rkey> <addr> <total_size> <slot_count> <slot_cap>\r\n

  字段说明:
    rkey:       Slave MR 的 rkey（Master 用于 RDMA WRITE）
    addr:       Slave MR 基地址（环形缓冲区起始）
    total_size: MR 总大小
    slot_count: 槽位数量 (1024)
    slot_cap:   每槽容量 (512)
```

Slave 侧处理 `REPLSYNC` 时判断 transport 为 `kprobe-rdma`：

```c
if (!strcasecmp(g_cfg.repl_realtime_transport, "kprobe-rdma")) {
    // 可以 partial resync → 返回 MR 信息
    char resp[256];
    if (repl_kprobe_rdma_slave_accept(pd, resp, sizeof(resp)) == 0) {
        queue_bytes(c, resp, strlen(resp));
        return;
    }
}
// 否则走全量同步
```

---

## 7. 关键数据结构

### 7.1 BPF Maps

| Map | 类型 | 用途 |
|-----|------|------|
| `kprobe_ctl` | BPF_MAP_TYPE_ARRAY (8×u64) | enabled, pid, repl_fd |
| `kprobe_stats` | BPF_MAP_TYPE_ARRAY (16×u64) | hit_count, hit_bytes, skip_* |
| `repl_ringbuf` | BPF_MAP_TYPE_RINGBUF (1MB) | 拦截数据从内核→用户态转发模块 |

### 7.2 Slave MR 环形缓冲区

```c
#define KPROBE_RDMA_SLOT_COUNT      1024
#define KPROBE_RDMA_SLOT_CAPACITY   512   /* 足够容纳增量 RESP 命令 */

typedef struct __attribute__((packed)) kprobe_rdma_ringbuf_s {
    volatile uint64_t producer_head;   /* 偏移 0,  Master RDMA WRITE 更新 */
    volatile uint64_t consumer_tail;   /* 偏移 8,  Slave 本地更新 */
    unsigned char slots[KPROBE_RDMA_SLOT_COUNT * KPROBE_RDMA_SLOT_CAPACITY];
} kprobe_rdma_ringbuf_t;

/* 每个 slot 格式:
 *   [4 bytes: payload_len (uint32_t)]
 *   [payload_len bytes: RESP 协议数据]
 * 总大小: 16 + 1024*512 = 524,304 字节 ≈ 512KB */
```

### 7.3 配置项

```c
/* kv_config_t 新增 */
char  repl_kprobe_obj_path[256];  /* BPF 对象文件路径 */
int   kprobe_enabled;             /* 0=禁用, 1=启用 */
```

---

## 8. 配置与构建

### 8.1 Makefile

```makefile
ENABLE_KPROBE_RDMA?=1

# BPF 编译
BPF_KPROBE_SRC=$(SRC_DIR)/replication/bpf/repl_kprobe.bpf.c
BPF_KPROBE_OBJ=$(patsubst $(SRC_DIR)/%.bpf.c, build/%.bpf.o, $(BPF_KPROBE_SRC))

# C 源文件
SRCS += $(SRC_DIR)/replication/kvs_repl_kprobe.c

# 编译标记
CFLAGS += $(if $(filter 1,$(ENABLE_KPROBE_RDMA)),-DKVS_ENABLE_KPROBE_RDMA=1,)
# kprobe 依赖 RDMA
ifeq ($(ENABLE_KPROBE_RDMA),1)
ENABLE_RDMA ?= 1
endif
LDFLAGS += $(if $(filter 1,$(ENABLE_KPROBE_RDMA)),-lbpf -lelf -lz,)
```

### 8.2 编译

```bash
# BPF 对象
clang -O2 -g -target bpf -D__TARGET_ARCH_x86 \
    -I./include \
    -c src/replication/bpf/repl_kprobe.bpf.c \
    -o build/replication/bpf/repl_kprobe.bpf.o

# kvstore
make ENABLE_KPROBE_RDMA=1
```

### 8.3 启动命令

```bash
# Master（需要 root 加载 BPF）
sudo ./kvstore --port 5160 --role master \
    --repl-fullsync-transport rdma \
    --repl-realtime-transport kprobe-rdma \
    --rdma-dev siw0 \
    --kprobe-enabled 1 \
    --repl-kprobe-obj-path build/replication/bpf/repl_kprobe.bpf.o

# Slave
./kvstore --port 5161 --role slave \
    --master-host 192.168.233.128 --master-port 5160 \
    --repl-fullsync-transport rdma \
    --repl-realtime-transport kprobe-rdma
```

---

## 9. 验证方案

### 9.1 冒烟测试

```bash
tools/repl/run_repl_kprobe_rdma_smoke.py \
    --master-host 192.168.233.128 --master-port 5160 \
    --slave-host 192.168.233.129 --slave-port 5161 \
    --pre 50000 --post 50000
```

### 9.2 验证矩阵

| 检查项 | 方法 | 预期 |
|--------|------|------|
| kprobe 拦截命中 | `bpftool map dump name kprobe_stats` | hit_count > 0 |
| Slave MR 建链 | Master 日志看到 `+KPROBERDMA` 响应 | 成功 |
| RDMA WRITE 成功 | `INFO` → `kprobe_rdma_writes` | = 增量命令数 |
| Slave 数据一致性 | 逐个 key 比对 | 100% |
| TCP 保底去重 | Slave repl_offset 检查 | 无重复 |
| Slave 零系统调用 | `strace -p <slave_pid>` | 无 recv 调用 |

### 9.3 监控

```bash
# kprobe 统计
bpftool map dump name kprobe_stats

# ringbuf 状态
bpftool map dump name repl_ringbuf

# INFO
redis-cli -p 5160 INFO | grep kprobe

# 实时追踪
bpftrace -e 'kprobe:tcp_sendmsg /pid == $1/ { @ = count(); }' $KVS_PID
```

---

## 10. 风险与边界

| 风险 | 影响 | 应对 |
|------|------|------|
| BPF 栈 512B 限制 | 大命令截断 | ✅ 增量数据量小，单次足够；未来如需支持大命令，kprobe 截取头部+记录总长 |
| kprobe 性能开销 | Master 吞吐下降 | ✅ PID+fd 精确过滤；纯内存操作开销可忽略 |
| TCP 和 RDMA 数据重复 | 重复写入 | ✅ Slave 通过 repl_offset 去重 |
| Slave 轮询 CPU | 额外消耗 | ✅ 无数据时 usleep(100μs)；可配置 |
| RDMA WRITE 可见性 | 读到不完整数据 | ✅ FENCE 保证数据先于 head 可见 |
| MR 溢出 | 覆盖未消费数据 | ✅ RC QP 有序保证 + head-tail < N 检查 |
| Fallback | 增量路径中断 | ✅ TCP 保底始终运行；backlog 补传 |
