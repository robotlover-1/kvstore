# Task 5 Report: queue_snapshot() FILE* -> fd, kvs_snapshot_to_fp -> kvs_dump_to_fd

## Status: Completed

## Changes

- 1 file modified: `src/main/kvstore.c`
- `queue_snapshot()` 内改动:
  1. `FILE *fp;` -> `int fd;`（变量声明）
  2. `kvs_snapshot_to_fp(fp)` -> `kvs_dump_to_fd(fd, snap_base_offset)`（生成 KVSD 二进制而非 RESP 文本）
  3. 3次 fopen/fclose 缩减为 1次 open/close 周期
  4. 文件大小由 `fread` 循环累加 -> `lseek(fd, 0, SEEK_END)` 一次获取
  5. `snap_base_offset` 传入 `kvs_dump_to_fd()` 作为 aof_offset 参数
  6. 所有错误路径显式 `close(fd)` + `unlink(tmp_path)` 后 goto out
  7. 删除 `out:` 标签下的 `if (fp) fclose(fp);`
- Header 格式（`+FULLRESYNC`）和 `repl_send_chunked` 调用保持不变

## Verification

- `make clean && make` -- 编译通过，零错误、零警告
- `queue_snapshot()` 内无 `fp` 残留引用
- `fcntl.h` / `unistd.h` 已通过 `kvstore/kvstore.h` 间接包含

## Risk

None.
