# Task 5 Report

## Exact Line Changes

File: `src/main/kvstore.c`

1. **Lines 627-642 removed** — Deleted the entire kprobe fwd connection setup block:
   - Removed `extern volatile int g_repl_broadcast_suppressed`
   - Removed `extern volatile time_t g_fwd_last_active`
   - Removed `extern volatile int g_fwd_healthy`
   - Removed `repl_kprobe_fwd_connect_from_replica(c, g_cfg.port)` call
   - Replaced with comment: `/* kprobe fwd 作为增量同步主路径（共享 c->fd），待 flush 成功后标记 healthy */`

2. **Lines 633-642 (new)** — Moved `cache_flushed` declaration up and added per-slave `fwd_healthy` initialization:
   - `int cache_flushed = repl_client_capture_flush_to_slave(c)` moved from old line 651 to new line 634
   - Added `c->fwd_healthy = 1`, `c->fwd_last_active = time(NULL)` on flush success
   - Added `c->fwd_healthy = 0` on flush failure
   - Uses per-slave `conn_t` struct fields instead of globals

3. **Old `cache_flushed` declaration removed** — The original `int cache_flushed` at old line 651 no longer exists; variable now declared only once at new line 634.

## Build Result

- `kvstore.o` compiled clean — no warnings or errors
- All errors in `kvs_repl_kprobe.c` only, referencing deleted globals (`g_kprobe_fwd_fd`, `g_fwd_healthy`, `g_fwd_last_active`) — expected, those are cleaned up in later tasks

## Commit

```
d5221311a168346d4a339eaec775ff0bef4c21b9
refactor: queue_snapshot sets per-slave fwd_healthy after flush, removes port+13 connect
```

## Status: DONE
