# eBPF Proxy 独立进程设计

> 日期: 2026-07-06
> 状态: 设计完成，待评审

## 1. 目标

将 eBPF client_capture（kprobe/tcp_recvmsg）从 kvstore master 进程中独立出来，作为单独的 `ebpf-proxy` 进程运行。同时修正全量同步完成（REPLDONE）的发送方：从 master 发送改为 slave 发送。

## 2. 背景与问题

### 2.1 当前架构问题

- eBPF 程序（sockmap、client_capture、kprobe）由 kvstore 主进程通过 libbpf 直接加载
- client_capture BPF 程序 hook `tcp_recvmsg`，捕获 master 收到的客户端写入数据
- eBPF 的生命周期与 kvstore 主进程绑定，不便于独立部署和调试

### 2.2 全量同步完成信号的设计问题

当前流程：
```
Master 发 KVSD → Master 发 REPLDONE
```

问题：master 发送完 KVSD 数据后立即发 REPLDONE，但不知道 slave 是否确实收到并完成了本地恢复。如果 slave 在接收 KVSD 过程中出错（磁盘满、数据损坏），master 已经认为全量同步完成，状态不一致。

## 3. 架构设计

### 3.1 组件

```
┌─────────────────────────── Master 机器 ──────────────────────────────────┐
│                                                                          │
│  ┌─────────────────────┐    ┌──────────────────────┐    ┌──────────────┐ │
│  │   kvstore master    │    │   ebpf-proxy 进程    │    │    slave     │ │
│  │                     │    │                      │    │              │ │
│  │  接收客户端 RESP    │    │  ringbuf poll +      │TCP │  接收增量数据│ │
│  │  处理命令           │    │  数据缓存/转发       │───→│  接收 KVSD   │ │
│  │  全量同步(KVSD)─────┼TCP─┼──────────────────────┼───→│  恢复后发    │ │
│  │                     │    │                      │    │  REPLDONE    │ │
│  │  recvmsg ◄──────────┼─kprobe─→ BPF client_capture   │              │ │
│  │  (客户端数据,       │    │ (内核态)             │    └──────────────┘ │
│  │   REPLSYNC,REPLDONE)│    │                      │                     │
│  └─────────────────────┘    └──────────────────────┘                     │
│               │                       │                                   │
│               └───────┬───────────────┘                                   │
│              /sys/fs/bpf/kvstore/ (共享 BPF maps)                        │
└──────────────────────────────────────────────────────────────────────────┘
```

### 3.2 组件职责

| 组件 | 进程 | 职责 |
|------|------|------|
| kvstore master | 主进程 | 接收客户端命令、处理业务逻辑、发送 KVSD 全量数据给 slave、解析 REPLDONE 更新 slave 状态 |
| ebpf-proxy | 独立进程 | attach kprobe 到 master tcp_recvmsg、截获增量数据转发 slave、检测 REPLSYNC/REPLDONE 管理缓存状态 |
| slave | 远端进程 | 接收增量数据 + KVSD、保存 KVSD 到本地并恢复后发送 REPLDONE |

### 3.3 通信路径

| 方向 | 机制 | 说明 |
|------|------|------|
| master → proxy（配置） | BPF maps (`proxy_cfg`, `client_ctl`) | master 将 PID、端口、slave 地址写入 pinned maps |
| BPF kernel → proxy（数据） | BPF ringbuf (`client_cache_ringbuf`) | kprobe 截获的 recvmsg 数据 |
| proxy → slave（增量） | 独立 TCP 连接 | proxy 将截获的客户端数据转发给 slave |
| master → slave（全量） | 独立 TCP/RDMA 连接 | master 直接发送 KVSD |
| slave → master（控制） | TCP | REPLSYNC/REPLDONE 经由 master recvmsg 被 eBPF 截获 |

## 4. 数据流与状态机

### 4.1 ebpf-proxy 状态机

```
        REPLSYNC 检测到
  FORWARDING ──────────→ BUFFERING
      ↑                      │
      │                      │ REPLDONE 检测到
      │                      │ (flush 缓存后)
      │                      ↓
      └──────────────────────┘
```

### 4.2 正常转发（FORWARDING 态）

```
Client → master recvmsg → BPF kprobe 抓数据 → ringbuf
                                                  ↓
                                            proxy poll
                                                  ↓
                                    [4B len][data] → proxy 直发 slave TCP
```

- BPF `kretprobe/tcp_recvmsg` 抓 iovec 数据后写 `[4B payload_len][payload]` 到 ringbuf
- proxy poll ringbuf，直接 `send(slave_fd, payload, payload_len, 0)`

### 4.3 全量同步中（BUFFERING 态）

```
Client → master recvmsg → BPF kprobe 抓 → ringbuf → proxy 挂到内存链表缓存
                                                      ↑
                                          Master → KVSD → slave (直接 TCP)
                                                      ↑
                                                proxy 不参与 KVSD 传输
```

- slave 发起全量同步前发送 REPLSYNC 给 master
- BPF kretprobe 检测到 REPLSYNC → 设置 `client_ctl[FULLSYNC_STATE] = 1`
- proxy poll 时发现状态切换 → 进入 BUFFERING，后续数据挂链表
- 缓存上限 256MB，超限时丢弃最旧节点（环形覆盖），记录丢计数

### 4.4 全量同步完成

```
Slave: KVSD 存本地 → 恢复到存储引擎 → 发 REPLDONE 给 master
         ↓
Master recvmsg ← BPF kprobe 检测到 REPLDONE
         ↓
BPF: client_ctl[FULLSYNC_STATE] = 0 + 写 magic(0xFFFFFFFF) 到 ringbuf
         ↓
Proxy: poll 到 magic → 遍历缓存链表 → 逐条 send 到 slave → 清空缓存 → FORWARDING
```

## 5. BPF Maps 设计

### 5.1 client_ctl（Array, max_entries=8）

| key | 名称 | 类型 | 说明 |
|-----|------|------|------|
| 1 | CTL_PID | u64 | master 进程 PID，0=禁用 |
| 2 | LISTEN_PORT | u64 | master 监听端口 |
| 3 | FULLSYNC_STATE | u64 | 0=正常转发, 1=全量同步中(缓存) |

Kernel 侧 BPF 负责写 key 3（检测 REPLSYNC/REPLDONE），userspace 负责读 key 3。

### 5.2 proxy_cfg（Hash, key=char[32], value=u64）

新建 map，定义在 `repl_client_capture.bpf.c` 中。虽然 BPF 内核代码不直接使用该 map，但定义在 BPF 程序中可以随 BPF 加载自动创建，无需额外 `bpf_map_create()` 调用。proxy 加载 BPF 程序时该 map 被创建并 pin，master 随后打开并写入。

| key | value 含义 |
|-----|-----------|
| "slave_addr" | slave IPv4 地址（32-bit host order） |
| "slave_port" | slave TCP 端口 |
| "master_pid" | master 进程 PID（master 启动后写入） |
| "master_port" | master 监听端口（master 启动后写入） |

master 在启动完成后打开 pinned `proxy_cfg` 写入 PID 和端口。slave 连接/配置变更时更新 slave 地址。

**时序问题处理：** proxy 启动后先加载 BPF 并 pin maps，然后轮询 `proxy_cfg["master_pid"]` 直到非零（master 已写入），确认 master 就绪后才 attach kprobe。如果 master 后启动（或重启），master 打开已有 pinned maps 写入即可，proxy 检测到值变化后自动更新。

### 5.3 client_stats（Array, max_entries=16）

| key | 名称 | 说明 |
|-----|------|------|
| 0 | HIT | 总命中次数 |
| 1 | SKIP_PID | PID 不匹配跳过 |
| 2 | RB_ERR | ringbuf 写入错误 |
| 3 | DATA_OVR | 数据超过上限 |
| 4 | READ_ERR | probe_read 失败 |
| 5 | CACHED | 缓存条目数（全量同步期间） |
| 6 | RETPROBE_MISS | kretprobe 未找到 entry 保存的 msg 指针 |
| 7 | REPLDONE_DETECT | 检测到 REPLDONE |
| 8 | REPLSYNC_DETECT | 检测到 REPLSYNC（新增） |
| 9 | CACHE_DROPPED | 缓存满丢条目数（新增） |
| 10 | CACHE_MAX_BYTES | 缓存峰值字节数（新增） |

### 5.4 client_cache_ringbuf（Ringbuf, 4MB）

不变，kprobe 截获的数据写入此 ringbuf。

## 6. BPF 程序改动

文件：[src/replication/bpf/repl_client_capture.bpf.c](src/replication/bpf/repl_client_capture.bpf.c)

### 6.1 移除

- `SEC("kprobe/tcp_sendmsg")` 函数 `kprobe_client_sendmsg` — 不再需要检测 master 发出的 REPLDONE
- `client_entry_msg` per-CPU map — 如果不再被 sendmsg probe 使用则移除

### 6.2 保留

- `kprobe/tcp_recvmsg`（entry）— 不变，保存 msg 指针到 per-CPU map
- `kretprobe/tcp_recvmsg`（return）— 保留基本抓包逻辑

### 6.3 新增：REPLSYNC 检测

在 kretprobe 中，读数据后扫描 "REPLSYNC" 子串（同现有 REPLDONE 检测方式）。匹配后设置 `client_ctl[3] = 1`。

### 6.4 改动：REPLDONE 检测

将原来在 `kprobe_client_sendmsg` 中的 REPLDONE 检测逻辑移到 kretprobe recvmsg 中。检测到 REPLDONE 后：
- 设置 `client_ctl[3] = 0`
- 写 magic value `0xFFFFFFFF` 到 ringbuf（通知 proxy flush）
- 更新 `client_stats[7]`（REPLDONE_DETECT）

## 7. ebpf-proxy 进程设计

### 7.1 命令行

```
ebpf-proxy --pin-path /sys/fs/bpf/kvstore_repl_sockmap
```

### 7.2 启动流程

1. 解析命令行参数（`--pin-path`、`--obj-path`）
2. `setrlimit(RLIMIT_MEMLOCK, RLIM_INFINITY)`
3. 加载 BPF 程序（`repl_client_capture.bpf.o`）→ maps 被创建
4. Pin maps 到 `--pin-path` 目录（`client_ctl`、`proxy_cfg`、`client_stats`、`client_cache_ringbuf`）
5. 轮询 `proxy_cfg["master_pid"]` 等待 master 写入（超时 30s，超时则退出报错）
6. 从 `proxy_cfg` 读取 master PID、master 端口、slave 地址
7. 将 master PID 写入 `client_ctl[1]`，master 端口写入 `client_ctl[2]`
8. Attach kprobe + kretprobe to `tcp_recvmsg`（通过 PID 过滤）
9. 连接 slave TCP（重试直到成功，指数退避初始 100ms/最大 5s）
10. 进入 `ring_buffer__poll` 主循环

步骤 5 等待 master 写入配置。如果 proxy 先于 master 启动，它会等待；如果 master 重启，master 重新打开 pinned maps 写入后 proxy 自动检测到。

### 7.3 ringbuf 回调

```c
void ringbuf_callback(void *data, size_t len) {
    uint32_t payload_len = *(uint32_t*)data;
    unsigned char *payload = (unsigned char*)data + 4;

    if (payload_len == 0xFFFFFFFF) {  // magic: flush signal
        flush_cache_to_slave();
        state = FORWARDING;
        return;
    }

    if (state == FORWARDING) {
        send(slave_fd, payload, payload_len, MSG_NOSIGNAL);
    } else {  // BUFFERING
        append_to_cache(payload, payload_len);
    }
}
```

### 7.4 缓存实现

- 单链表结构：`{struct cache_node *next; size_t len; unsigned char data[];}`
- 总大小上限 256MB
- 达到上限时从 head 丢弃最旧节点，`stats.dropped++`
- `flush_cache_to_slave()`：从 head 遍历，逐条 `send()`，成功后释放

### 7.5 主循环

```c
for (;;) {
    ring_buffer__poll(rb, 100 /* timeout_ms */);

    // 检查 fullsync_state 是否变化
    int fs = read_client_ctl(FULLSYNC_STATE);
    if (fs == 1 && state == FORWARDING) state = BUFFERING;

    // 检查 slave 连接健康
    if (slave_disconnected()) reconnect_slave();

    // 信号处理
    if (shutdown_requested) goto cleanup;
}
```

### 7.6 Slave 重连策略

指数退避：初始 100ms → 200ms → 400ms → ... → 最大 5s。连接成功后重置退避计数器。

### 7.7 退出清理（SIGINT/SIGTERM）

1. 设置 `shutdown_requested = 1`
2. detach kprobe + kretprobe
3. 如果 state == BUFFERING：尝试 flush 剩余缓存到 slave（best effort，超时 3s）
4. 关闭 slave TCP 连接
5. 关闭所有 BPF map fds
6. 退出

## 8. Master 侧改动

### 8.1 删除

- `repl_ebpf_init()` 调用及相关 sockmap/kprobe 初始化逻辑（[kvstore.c:2373-2400](src/main/kvstore.c#L2373-L2400)）
- `kvs_repl_kprobe.c` 中 master 侧 kprobe 加载和 ringbuf poll 逻辑
- `queue_snapshot()` 末尾发送 REPLDONE 的逻辑（如果存在）

### 8.2 新增

- master 启动后打开 pinned `proxy_cfg` map，写入 `master_pid`（自己的 PID）和 `master_port`（监听端口）。如果 pinned map 尚不存在（proxy 未启动），重试最多 5s，超时后跳过（proxy 后续启动时会发现 master 已运行并通过其他方式——如扫描 /proc——获取 PID；或者 master 周期性重试写入）
- slave 连接时，将 slave 地址写入 `proxy_cfg` map 的 `slave_addr` 和 `slave_port`
- `handle_parsed_command` 中新增 REPLDONE 响应：

```c
if (!strcasecmp(cmd, "REPLDONE")) {
    repl_on_slave_repldone(conn);  // 更新 slave 全量同步完成状态
    return resp_simple_string(response, cap, "OK");
}
```

### 8.4 slave 端发送 REPLSYNC

slave 建立连接后发送 REPLSYNC 请求全量同步。该命令经由 master 的 tcp_recvmsg 被 eBPF kprobe 截获，触发 BUFFERING 模式。现有的 REPLSYNC 发送逻辑（在 `slave_thread` 中）保持不变。

### 8.5 保留

- master 自己的 KVSD 发送逻辑不变
- master 自己的 TCP/RDMA 传输层代码不变

## 9. Slave 侧改动

### 9.1 全量同步完成流程修正

**当前错误流程：**
```
Master 发 KVSD → Master 发 REPLDONE
```

**修正后流程：**

1. Slave 发送 REPLSYNC 给 master
2. Master 开始发送 KVSD 给 slave（直接 TCP/RDMA）
3. Slave 接收 KVSD 写入临时文件（`.fullsync.recv.tmp`）
4. Slave 收到全部 KVSD → `fsync` → 关闭临时文件
5. Slave 重命名为正式 dump 文件
6. Slave 调用 `replay_dump_file()` 恢复到存储引擎
7. Slave 清理状态：`g_slave_loading_fullsync = 0`
8. **Slave 发送 REPLDONE 给 master** ← 关键改动
9. eBPF proxy 在 master recvmsg 中截获 REPLDONE → flush 缓存
10. Master 收到 REPLDONE → 标记 slave 同步完成

### 9.2 代码改动

`repl_slave_finish_fullsync()` 中，最后一步改为发送 REPLDONE：

```c
void repl_slave_finish_fullsync(void) {
    // ... KVSD 文件处理（不变）...
    // ... replay_dump_file（不变）...
    // ... repl_slave_state_save（不变）...
    repl_slave_send_repldone();  // 发送 *1\r\n$8\r\nREPLDONE\r\n
}
```

`repl_slave_send_repldone()` 通过 slave 到 master 的 TCP 连接发送 REPLDONE 命令。

## 10. 文件结构

```
src/
├── ebpf_proxy/                    # 新增：独立 eBPF proxy 进程
│   ├── main.c                     # 入口 + 命令行解析 + 主循环
│   ├── proxy_ringbuf.c            # ringbuf 回调 + 缓存/转发逻辑
│   ├── proxy_slave.c              # slave 连接管理 + 重连
│   └── Makefile                   # proxy 编译规则
├── replication/
│   ├── bpf/
│   │   └── repl_client_capture.bpf.c  # 修改：移除 sendmsg probe，新增 REPLSYNC 检测
│   ├── kvs_repl.c                     # 修改：repl_slave_finish_fullsync 发 REPLDONE
│   ├── kvs_repl_ebpf.c                # 删除/简化（不再加载 BPF 程序）
│   └── kvs_repl_kprobe.c              # 删除 master 侧 kprobe 加载代码
├── main/
│   └── kvstore.c                      # 修改：删除 eBPF/kprobe 初始化、新增 REPLDONE handler、写 proxy_cfg
└── include/
    └── kvstore/
        ├── kvstore.h                  # 不变
        └── replication/
            └── repl_kprobe.h          # 简化
```

根目录 `Makefile` 新增 `ebpf-proxy` 目标。

## 11. 构建

```makefile
# 新增 targets
EBPF_PROXY_SRC = src/ebpf_proxy/main.c src/ebpf_proxy/proxy_ringbuf.c src/ebpf_proxy/proxy_slave.c
EBPF_PROXY_OBJ = build/ebpf_proxy

ebpf-proxy: $(EBPF_PROXY_SRC)
	$(CC) $(CFLAGS) -o $(EBPF_PROXY_OBJ) $(EBPF_PROXY_SRC) $(LDFLAGS) -lbpf -lelf -lz

client_capture_bpf: src/replication/bpf/repl_client_capture.bpf.c
	clang -O2 -target bpf -g -c -o build/replication/bpf/repl_client_capture.bpf.o $<
```

## 12. 测试策略

### 12.1 单元测试

- 缓存链表：追加、flush、上限丢弃
- 重连退避算法

### 12.2 集成测试

- 启动 master → 启动 ebpf-proxy → slave 连入 → 客户端写入 → 验证 slave 收到数据
- 全量同步场景：写入一批数据 → slave 发起全量同步 → 全量同步期间继续写入 → 验证 REPLDONE 后增量数据完整
- 缓存上限触发：大量写入 → 验证丢弃行为和数据一致性
- ebpf-proxy 退出清理：验证 kprobe detach、缓存 flush、资源释放

### 12.3 现有测试回归

- `tests/test_repl_basic.c`
- `tests/test_repl_gap.c`

## 13. 风险与未决事项

| 风险 | 缓解措施 |
|------|---------|
| kprobe 在 proxy 独立进程后 detach 时机 | proxy 退出时保证 detach，避免 kernel panic |
| proxy 缓存 flush 期间 slave 断连 | best effort send，失败后丢弃缓存，记录 stats |
| master 和 proxy 竞争 BPF maps | client_ctl key 3 只由 BPF 程序写入，userspace 只读 |
| REPLSYNC/REPLDONE 跨 TCP segment 分片 | BPF 用同现有 REPLDONE 检测逻辑：在 ringbuf entry 的前 64 字节中扫描匹配 |
