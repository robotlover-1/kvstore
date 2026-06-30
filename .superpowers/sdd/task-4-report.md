# Task 4 Report: parse_resp_stream() 全量同步数据拦截

## Status: DONE

## 修改内容

### src/replication/kvs_repl.c

1. **声明变更**（第85-88行）：
   - `g_slave_loading_fullsync`: `static int` -> `int`（移除 static，允许 kvstore.c 通过 extern 访问）
   - `g_slave_fullsync_target_bytes`: `static unsigned long long` -> `unsigned long long`（同上）
   - `g_slave_fullsync_loaded_bytes`: `static unsigned long long` -> `unsigned long long`（同上）
   - 新增 `int g_slave_fullsync_tmp_fd = -1;` 全局变量，用于全量同步临时文件 fd

2. **临时文件管理**（`repl_slave_set_sync_state()` 函数内，`repl_slave_state_save()` 调用之前）：
   - 当 `fullsync_loading` 且临时文件未打开时：创建临时文件 `{dump_path}.fullsync.recv.tmp.{pid}`
   - 当不再加载全量同步且临时文件仍打开时：关闭并清理

### src/main/kvstore.c

3. **全量同步拦截**（`parse_resp_stream()` 函数体开头，`#define PARSE_SCRATCH` 之前）：
   - 添加 `extern` 声明引入 `g_slave_fullsync_tmp_fd`、`g_slave_loading_fullsync`、`g_slave_fullsync_target_bytes`、`g_slave_fullsync_loaded_bytes`
   - 当 `from_replication && g_slave_loading_fullsync` 时，拦截原始 KVSD 字节写入临时文件
   - 正确计算 `to_write = min(*len, remaining)`
   - 写入失败返回 -1
   - 边界处理：当 `to_write < *len` 时（最后一包含 REPLDONE 等尾随数据），只消费已写入部分，剩余数据用 `memmove` 保留在 buf 开头供正常 RESP 解析
   - 达到目标字节数时调用 `repl_slave_finish_fullsync()`

## 构建验证

```bash
make clean && make
```

结果：**编译通过，零警告，零错误。**

## 实现要点

- **单点拦截**：`parse_resp_stream()` 是所有 slave 接收路径（TCP/RDMA/kprobe）的唯一入口，在此拦截覆盖全部传输层
- **最小侵入**：仅在函数开头添加拦截块，后续代码完全不变
- **边界安全**：正确处理目标字节对齐（`to_write` 不超过 `remaining`），尾随数据保留供后续 RESP 解析
- **`repl_slave_finish_fullsync()` 通过 `kvstore.h` 已有声明**，无需额外声明

## 风险评估

- **低风险**：改动最小（两个文件，各一处关键修改），编译零警告
- `g_slave_loading_fullsync`/`g_slave_fullsync_target_bytes`/`g_slave_fullsync_loaded_bytes` 从 static 改为非 static：不影响功能，仅扩大可见性
- 临时文件路径通过 `g_cfg.dump_path` 构造，与 master 侧临时文件命名风格一致
