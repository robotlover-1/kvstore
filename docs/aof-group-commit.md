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
