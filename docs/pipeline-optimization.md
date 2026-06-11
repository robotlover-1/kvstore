# Pipeline 性能优化 — 迭代记录

## 初始状态

Pipeline 性能极差：P=10 echo 仅 11k QPS（0.11M cmd/s），vs Redis 1.27M QPS（12.7M cmd/s），
差距 113×。P=1 无 pipeline 时正常（131k）。

## Phase 1: Output Ring Buffer (935c15b)

### 问题
`queue_bytes` 每响应 2 次 malloc（out_node_t + data），`on_write` 链表逐节 send。

### 改动
- `conn_t` 移除 `out_head`/`out_tail` 链表 → 64KB 环形缓冲 `out_ring[65536]`
- `queue_bytes` malloc+malloc+memcpy → 1 次 memcpy 入环
- `on_write` 逐节 send → 环形缓冲批量 send/writev
- 影响 reactor/proactor/ntyco 三个网络模型
- `on_read` 后立即 `on_write` 尝试写出（减少 epoll_wait 周期）
- `queue_bytes` 移除每响应 `mod_events`（避免每命令 epoll_ctl）

### 效果

| P | 改前 | 改后 | 提升 |
|---|------|------|------|
| 10 echo | 11,154 | 921,659 | **83×** |
| 160 echo | 178,891 | 1,434,720 | 8× |
| 10 HSET disable | 11,146 | 419,112 | 38× |
| 160 HSET disable | 176,710 | 891,266 | 5× |

## Phase 2: Zero-Copy RESP Parsing (0b1b71c)

### 问题
`parse_resp_stream` 每参数 kvs_malloc+memcpy+kvs_free（HSET=3 alloc/free per cmd）。

### 改动
- argv[i] 直接指向 inbuf，原地写 `\0` 覆盖 `\r`
- 消除每参数 malloc/copy/free
- inline 路径已为零拷贝（split_inline_argv 原地修改）

### 效果

| P | 改前 | 改后 | 提升 |
|---|------|------|------|
| 10 echo | 921,659 | 956,938 | +3.8% |
| 160 HSET disable | 891,266 | 952,381 | +6.9% |

累计 Phase 1+2: echo P=10 11k→957k（87×）

## Phase 3: Command Path Micro-Optimizations (75c7be4)

### 3A: Pre-encoded RESP Literals
- `resp_simple_string("OK")` snprintf → `memcpy(RESP_OK, 5)`
- 静态字面量: `"+OK\r\n"`, `":1\r\n"`, `"-ERR not found or exists\r\n"`, `"-ERR operation failed\r\n"`
- HSET P=160: 952k→1.03M (+8.1%)

### 3B: First-Char Switch Dispatch
- 10 个 `strcmp` 链式比较 → `switch(op[0])` + 长度匹配
- HSET P=160: 1.03M→1.04M (+1.4%)
- strcmp 开销已被现代 CPU SSE/AVX 硬件优化

### 效果
Phase 3 累计: HSET P=160 952k→1.04M (+9.7%)

## 总结

| 阶段 | 优化 | Echo P=10 | HSET P=160 | 核心改动 |
|------|------|-----------|------------|----------|
| 初始 | — | 11,154 | 176,710 | — |
| P1 | Output ring buffer | 921,659 | 891,266 | conn_t 环形缓冲 |
| P2 | Zero-copy parsing | 956,938 | 952,381 | argv 指入 inbuf |
| P3A | RESP literals | — | 1,029,866 | 静态字面量 |
| P3B | Fast dispatch | — | 1,043,841 | switch 首字符 |

| P | 初始 kv | 优化后 kv | redis | kv/redis |
|---|---------|-----------|-------|----------|
| 10 echo | 0.11M | 9.6M | 12.7M | 75% |
| 160 echo | 28.6M | 230.6M | 541M | 43% |
| 10 HSET | 0.11M | 4.4M | 12.4M | 35% |
| 160 HSET | 28.3M | 167M | 457M | 37% |

## 待优化方向

- `handle_parsed_command` resp buffer 按需分配（当前每条命令都 malloc 64KB，仅用 5 字节）
- 哈希引擎批量操作（pipeline 时批量 rehash/lookup）
- AOF always 模式 pipeline 适配（当前 group commit fsync 串行瓶颈）
