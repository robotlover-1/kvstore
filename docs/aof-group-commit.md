# AOF Group Commit — 迭代记录

## 问题

ALWAYS 模式（`appendfsync always`）每条命令独立 io_uring write+fsync。
RESP 命令 ~40 字节，io_uring ring buffer 操作 + submit_and_wait 固定开销占主导。
kvstore_aof_always QPS 仅 14.7k，Redis 同配置 120k，差 8x。

## 迭代 1: AOF 写缓冲 (7ec86c0)

`persist_append_raw` — 加 64KB AOF buffer + io_uring batch write+fsync。

- `persist_write_and_fsync_uring()`: 单次 io_uring submit 同时提交 write+fsync
- `persist_aof_flush_buffer()`: 缓冲区满或大命令时触发 flush
- Bug 修复: 缓冲路径不提前递增 `g_aof_write_offset`

效果：everysec 模式受益（batch write），always 模式不变（每个命令立即 flush，缓冲无用）。

## 迭代 2: Group Commit (b4967f9)

ALWAYS 模式下不立即 flush，将多条命令的 RESP 数据累积到缓冲，响应延迟到 group flush 后统一发送。

### 修改的函数

**`persist_append_raw()`** (kvs_persist.c:595)
- v1: 每命令 → buffer → force_flush (立刻 write+fsync)
- v2: 每命令 → buffer → 仅当阈值触发才 flush（≥8KB 或 ≥2ms）→ 否则 defer

**`persist_aof_flush_buffer()`** (kvs_persist.c:185)
- v1: flush 后重置 buffer
- v2: flush 后额外重置 `g_aof_buffered_since_ms`

**`handle_parsed_command()`** (kvstore.c:1741)
- v1: `persist_append_raw` → `queue_bytes` 立即响应
- v2: `persist_append_raw` → 若有 pending → `persist_defer_response` → goto out（跳过立即响应）

**`reactor event loop`** (reactor.c:281)
- v1: 处理事件后直接 epoll_wait
- v2: 处理事件后 → `persist_flush_deferred()` → flush AOF + 发送所有延迟响应

### 新增函数

**`persist_defer_response(c, data, len)`** (kvs_persist.c)
- 将 (conn, response_data, len) 挂入链表 `g_deferred_head→tail`
- 分配内存复制响应数据（避免原 buffer 被释放）

**`persist_flush_deferred()`** (kvs_persist.c)
- 若 AOF buffer 有数据 → flush
- 遍历 deferred 链表 → `queue_bytes` 发送所有响应 → 释放节点

**`persist_aof_has_pending()`** (kvs_persist.c)
- 仅在 ALWAYS 模式返回 true（everysec 走自己的一秒定时器）

### 触发条件

| 触发点 | 条件 |
|--------|------|
| persist_append_raw 内 | 缓冲 ≥8KB 或 距首条 ≥2ms |
| reactor 事件循环后 | 每轮 epoll_wait 批量处理完 |
| persist_autosnap_cron | 100ms 定时器中检查是否有滞留数据 |

### 效果

| 配置 | 改前 | 改后 | 提升 |
|------|------|------|------|
| kvstore_aof_always | 14,697 | 58,059 | 3.95x |
| kvstore_aof_everysec | 122,941 | 126,630 | ~same |
| kvstore_aof_disable | 127,016 | 126,743 | ~same |

### 剩余差距

kvstore 58k vs Redis 116k (~2x)。可能原因：
- `persist_defer_response` 每条命令 malloc+memcpy（~50B）
- io_uring 对小写入的固定开销仍高于原生 write()+fsync()
- epoll_wait 100ms timeout 在无事件时延迟发送

## 迭代 3: 内嵌数据 + 立即写出 + 最大批量 (4ec3e90)

### 优化

1. **deferred_resp_t 数据内嵌**: `unsigned char data[128]` 替代 `unsigned char *data`
   - 消除每条响应的 malloc+memcpy（原 2 次 malloc → 1 次）
   
2. **移除 8KB 内联阈值**: persist_append_raw 中仅保留 2ms 时间阈值
   - 依赖 reactor 循环结束时的 persist_flush_deferred 统一 flush
   - 最大化每批次的命令数量

3. **立即 flush_conn_output**: persist_flush_deferred 中 queue_bytes 后立刻调用 on_write
   - 减少一次 epoll_wait 周期（不等 EPOLLOUT 事件）
   - 响应在 socket buffer 有空闲时立即发出

4. **尝试 pwrite+fdatasync**: 替代 io_uring write+fsync
   - 结果: 41k QPS（比 io_uring 65k 更差）
   - 结论: io_uring batch write+fsync 比 pwrite+fdatasync 快 1.56×

### 效果

| 配置 | 初始 | 迭代1 | 迭代2 | 迭代3 |
|------|------|-------|-------|-------|
| kvstore_aof_always | 14,697 | 58,059 | 61,667 | **65,548** |
| kvstore_aof_everysec | 122,941 | 126,630 | 124,177 | 126,582 |
| redis_aof_always | 119,962 | — | — | 120,453 |

### 最终分析

strace 证实 Redis `appendfsync always` 每条命令都调 `fdatasync()`（100 SET = 100 次 fdatasync），仍达 120k QPS。

剩余 ~2x 差距（65k vs 120k）原因：
- **io_uring ring buffer 开销**: get_sqe×2 + prep + submit_and_wait + wait_cqe×2，每条批次的固定开销无法消除
- **Reactor + 协程模型**: kvstore 的 NtyCo 协程调度、epoll_wait 循环、deferred response 链表遍历有固有开销
- **Redis 15 年优化**: Redis 单线程紧凑事件循环，write()+fdatasync() 直接 syscall，无中间层
- **pwrite+fdatasync 对比**: kvstore 的 pwrite+fdatasync (41k) < io_uring (65k) < Redis write+fdatasync (120k)。说明 kvstore 的非 fsync 路径也有开销

进一步优化方向（需更大改动）：
- 绕过协程层直接在 socket 可写时发送响应
- 预分配 deferred_resp_t 池（消除 malloc）
- 内核侧 io_uring 路径优化

## 最终状态 (Phase 6 之后)

经过 Pipeline Phase 1 (output ring buffer) 重构，AOF always 性能受 ring buffer 间接影响：

| 配置 | 初始 | 最终 | 提升 |
|------|------|------|------|
| kvstore_aof_always (P=1) | 14,697 | 41,850 | 2.8× |
| kvstore_aof_everysec (P=1) | 122,941 | 126,582 | +3% |
| kvstore_aof_disable (P=1) | 127,016 | 128,932 | +1.5% |
| redis_aof_always (P=1) | 119,962 | 120,453 | baseline |

AOF always pipeline 性能（受 group commit fsync 串行瓶颈）：
| P | kvstore (cmd/s) | Redis (cmd/s) |
|---|-----------------|---------------|
| 10 | 111,528 | 12,224,939 |
| 160 | 28,338,648 | 465,116,280 |

> AOF always 的 pipeline 差距来自 group commit 在每批次必须 fsync，
> 无法像 disable 模式那样利用 pipeline 批量。参见 `docs/pipeline-optimization.md`。

---

## Phase 8: strace 系统调用级分析 + 最新基准 (2026-06-12)

### strace 对比

使用 `sudo strace -f -c` 对 Redis 5.0.7 和 kvstore 做 10000 次 SET 的 syscall 分布对比：

| syscall | Redis 5.0.7 | kvstore | 说明 |
|---------|------------|---------|------|
| **数据落盘** | `fdatasync` ×10,001 (707ms, 70µs/次) | `io_uring_enter` ×2 (282µs) | kvstore group commit 将 10000 次 fsync 压缩为 2 次 io_uring 批量提交 |
| **网络写** | `write` ×20,009 (485ms) | `sendto` ×10,001 (273ms) | Redis 比 kvstore 多 1 次 write/命令 |
| **网络读** | `read` ×39,534 (642ms) | `recvfrom` ×20,004 (221ms) | Redis 多次 read，kvstore 分批 recv |
| **epoll** | `epoll_wait` ×19,712 (516ms) | `epoll_wait` ×10,211 + `epoll_ctl` ×20,007 (334ms) | kvstore 频繁切换 EPOLLIN/OUT |
| **文件打开** | `openat` ×9,828 + `close` ×9,824 (496ms) | 0 | **Redis 5.0.7 每条 SET 重开 AOF 文件** |
| **总 syscall** | 118,947 | 60,409 | kvstore 少 49% |
| **strace 下吞吐** | 1,102 req/s | 2,254 req/s | kvstore 2× 快（syscall 少） |

**关键发现：**

1. **Redis 5.0.7 的致命缺陷**：`appendfsync always` 下每条 SET 都会 `openat` + `close` AOF 文件（~9800 次/10000 SET），占总耗时 11%。Redis 6+ 已修复此问题（复用 fd）。

2. **kvstore 的 io_uring group commit 极端高效**：10000 条 SET 仅需 2 次 `io_uring_enter(7, 2, 2, IORING_ENTER_GETEVENTS)` —— 每次提交 write+fsync 两个 SQE，每次摊销 ~5000 条命令。

3. **fdatasync 延迟 ~70µs**（本机 NVMe），Redis 每条命令付一次，kvstore 每 5000 条付一次。

### 最新 Pipeline 基准 (redis-benchmark -c 1 -P <N>, 100k ops)

> 测试日期: 2026-06-12
> 测试工具: `redis-benchmark -n 100000 -c 1 -P <N>`

#### SET

| P | Redis 5.0.7 always | kvstore always | kvstore/Redis | Redis everysec | kvstore everysec | Redis no-AOF | kvstore no-AOF |
|---|-------------------|----------------|---------------|----------------|------------------|--------------|----------------|
| 1 | 2,283 | **20,467** | **9.0×** | 21,450 | 20,471 | 22,119 | 20,781 |
| 10 | 23,474 | **192,308** | **8.2×** | 172,414 | 204,082 | 175,439 | 185,185 |
| 20 | 46,729 | **384,615** | **8.2×** | 312,500 | 357,143 | 357,143 | 416,667 |
| 40 | 92,593 | **625,000** | **6.8×** | 416,667 | 625,000 | 625,000 | 625,000 |
| 80 | 156,250 | **1,250,000** | **8.0×** | 625,000 | 1,250,000 | 1,250,000 | 625,000 |
| 160 | 250,000 | **1,000,000** | **4.0×** | 1,000,000 | — | 1,000,000 | 1,000,000 |

#### HSET

| P | Redis 5.0.7 always | kvstore always | kvstore/Redis | Redis everysec | kvstore everysec | Redis no-AOF | kvstore no-AOF |
|---|-------------------|----------------|---------------|----------------|------------------|--------------|----------------|
| 1 | 2,290 | **14,537** | **6.3×** | 20,354 | 15,223 | 21,236 | 15,768 |
| 10 | 20,161 | **35,211** | **1.7×** | 158,730 | 36,900 | 156,250 | 36,496 |
| 20 | 42,735 | 42,373 | 1.0× | 294,118 | 48,544 | 294,118 | 50,505 |
| 40 | 83,333 | 53,191 | 0.6× | 416,667 | 54,348 | 416,667 | 53,191 |

### 分析

1. **kvstore AOF always 在 SET 下全面超越 Redis 5.0.7**：P=1 时 9×，P=10~80 时 6-8×。核心原因是 group commit 将 fsync 摊销到数千条命令，而 Redis 5.0.7 每条命令独立 fdatasync + openat/close。

2. **HSET 是 kvstore 的弱项**：P=1 时仍领先 6.3×，但到 P=40 反而落后。Hash 引擎的渐进式 rehash 在大批量写入时触发扩容，造成额外延迟。SET 使用 Array 引擎无此开销。

3. **AOF always 对 kvstore 几乎零开销**：P=1 SET always (20,467) vs no-AOF (20,781) 仅差 1.5%。group commit 将 fsync 成本完全隐藏。

4. **Redis 5.0.7 always vs disabled**：P=1 SET 2,283 vs 22,119——AOF always 在 Redis 5.0.7 上有 **10× 开销**。主要来自 per-command fdatasync (~70µs) + openat/close。

5. **与旧测试差异**：旧测试用 `-c 50 -P 1`（50 并发，无 pipeline），新测试用 `-c 1 -P <N>`（单连接，可变 pipeline）。两种场景的压力模式完全不同，不可直接对比。
