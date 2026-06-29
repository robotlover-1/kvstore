# KVStore 数据流文档

## 目录

1. [架构总览](#1-架构总览)
2. [客户端命令到主机的数据流](#2-客户端命令到主机的数据流)
3. [全量持久化（SAVE/BGSAVE）](#3-全量持久化savebgsave)
4. [增量持久化（AOF）](#4-增量持久化aof)
5. [全量同步（FULLRESYNC）](#5-全量同步fullresync)
6. [增量同步（eBPF+TCP / kprobe+RDMA）](#6-增量同步ebpf--tcp--kprobe--rdma)
7. [从机保存数据到磁盘](#7-从机保存数据到磁盘)
8. [关键数据结构](#8-关键数据结构)
9. [配置项速查](#9-配置项速查)

---

## 1. 架构总览

```
┌──────────┐                           ┌──────────┐
│  Client  │                           │  Client  │
│(redis-cli)│                          │(redis-cli)│
└────┬─────┘                           └────┬─────┘
     │ RESP                                  │ RESP
     ▼                                       ▼
┌─────────────────────────────────────────────────────────┐
│                       MASTER                           │
│  ┌──────────────────────────────────────────────────┐  │
│  │ 网络层: reactor(epoll) / proactor(io_uring) /    │  │
│  │         ntyco(coroutine)                         │  │
│  └──────────────────┬───────────────────────────────┘  │
│                     │                                  │
│  ┌──────────────────▼───────────────────────────────┐  │
│  │ RESP 协议解析 (parse_resp_stream)                 │  │
│  └──────────────────┬───────────────────────────────┘  │
│                     │                                  │
│  ┌──────────────────▼───────────────────────────────┐  │
│  │ 命令分发 (handle_parsed_command)                  │  │
│  └──┬────────┬─────────┬──────────┬────────────────┘  │
│     │        │         │          │                    │
│     ▼        ▼         ▼          ▼                    │
│  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────────┐             │
│  │Storage│ │Expire│ │AOF   │ │Repl      │             │
│  │5引擎 │ │TTL   │ │持久化│ │Broadcast │             │
│  └──────┘ └──────┘ └──────┘ └────┬─────┘             │
│                                  │                    │
│  Transport: TCP / RDMA / eBPF / kprobe+RDMA           │
└──────────────────────────────────┼────────────────────┘
                                   │
                    ┌──────────────┼──────────────┐
                    │              │              │
                    ▼              ▼              ▼
              ┌──────────┐  ┌──────────┐  ┌──────────┐
              │  SLAVE-1 │  │  SLAVE-2 │  │  SLAVE-N │
              │ AOF+Dump │  │ AOF+Dump │  │ AOF+Dump │
              └──────────┘  └──────────┘  └──────────┘
```

**核心源文件：**


| 文件                                            | 职责                                                         |
| ----------------------------------------------- | ------------------------------------------------------------ |
| `src/main/kvstore.c`                            | 入口、RESP 解析、命令分发、SNAPSHOT/DUMP 生成                |
| `src/persistence/kvs_persist.c`                 | AOF 写入、SAVE/BGSAVE、BGREWRITEAOF、恢复                    |
| `src/replication/kvs_repl.c`                    | 全量同步、增量广播、backlog、transport 抽象层、RDMA 全量同步 |
| `src/replication/kvs_repl_ebpf.c`               | eBPF sockmap 加载、fd 注册、统计                             |
| `src/replication/kvs_repl_kprobe.c`             | kprobe+RDMA WRITE 增量同步、client_capture                   |
| `src/replication/bpf/repl_sockmap.bpf.c`        | BPF sk_msg 程序（sockmap 重定向）                            |
| `src/replication/bpf/repl_kprobe.bpf.c`         | BPF kprobe 程序（tcp_sendmsg 拦截）                          |
| `src/replication/bpf/repl_client_capture.bpf.c` | BPF kprobe/kretprobe（tcp_recvmsg 拦截，全量同步期间缓存）   |

---

## 2. 客户端命令到主机的数据流

### 2.1 总流程

```
Client 发送 RESP 命令 (如 *3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n)
        │
        ▼
┌─── 网络层接收 ──────────────────────────────────────────────────┐
│  reactor: epoll_wait → EPOLLIN → recv → conn->inbuf            │
│  proactor: io_uring CQE → recv SQE → conn->inbuf               │
│  ntyco:    coroutine recv → conn->inbuf                         │
└──────────────────┬──────────────────────────────────────────────┘
                   │
                   ▼
┌─── RESP 协议解析 ───────────────────────────────────────────────┐
│  parse_resp_stream(c, buf, &len, from_replication=0)           │
│  解析 RESP 格式: *<n>\r\n$<len>\r\n<data>\r\n...              │
│  提取出 cmd, argv[], argl[], raw, rawlen                        │
└──────────────────┬──────────────────────────────────────────────┘
                   │
                   ▼
┌─── 命令分发 ────────────────────────────────────────────────────┐
│  handle_parsed_command(c, argc, argv, argl, raw, rawlen, 0)    │
│                                                                 │
│  kvs_ascii_upper(argv[0]) → "SET" / "HSET" / "RSET" 等       │
│                                                                 │
│  cmd_engine(cmd) → KVS_ENGINE_ARRAY / HASH / RBTREE / SKIPTABLE│
│  strip_prefix(cmd) → "SET" / "GET" / "DEL" 等                 │
└──────────────────┬──────────────────────────────────────────────┘
                   │
       ┌───────────┼───────────┐
       ▼           ▼           ▼
  ┌─────────┐ ┌─────────┐ ┌──────────────┐
  │ 读命令  │ │ 写命令  │ │ 管理命令      │
  │GET/HGET │ │SET/HSET │ │SAVE/INFO/... │
  └────┬────┘ └────┬────┘ └──────┬───────┘
       │           │             │
       ▼           ▼             ▼
  直接返回    完整写路径      直接执行
  engine_get  (见 2.2)       返回结果
  → queue_bytes
```

### 2.2 写命令处理路径（以 SET 为例）

```
handle_parsed_command(c, argc=3, argv=["SET","key","value"], from_replication=0)
    │
    ├─ 1. try_expire(engine, key)              清理可能过期的旧 key
    │
    ├─ 2. engine_set(engine, key, value)       写入存储引擎
    │      ├─ kvs_hash_set()    → 渐进式 rehash 的哈希表
    │      ├─ kvs_rbtree_set()  → 红黑树
    │      ├─ kvs_array_set()   → 定长数组
    │      ├─ kvs_skiptable_set() → 跳表
    │      └─ kvs_doc_set()     → 文档存储
    │
    ├─ 3. persist_note_write()                  g_dirty_counter++
    │      g_dirty_counter: 自上次 SAVE/BGSAVE 以来的写命令次数。
    │      用于 autosnap 自动快照判断（如 dirty>=10000 && elapsed>=3600s → BGSAVE）。
    │
    ├─ 4. persist_append_raw(raw, rawlen)       写入 AOF（见第 4 节）
    │
    ├─ 5. repl_broadcast(raw, rawlen)           广播到所有从机（见第 5、6 节）
    │      ├─ repl_backlog_feed()               写入 1MB 复制 backlog 环形缓冲
    │      ├─ repl_note_broadcast()             g_master_repl_offset += rawlen
    │      │      全局复制偏移量，单调递增的字节计数。slave 用它汇报同步进度。
    │      └─ 遍历 g_replicas 链表，只发给"已全量同步完、处于增量稳态"的 slave:
    │           ├─ 跳过 repl_draining:    该连接正在断开，发了也收不到
    │           ├─ 跳过 repl_fullsync_pending: 还没拿到全量基准数据
    │           ├─ g_repl_fullsync_in_progress 时跳过: 有 slave 在全量同步，
    │           │      增量数据暂由 client_capture 缓存，等全量完成后再补发
    │           └─ 对每条有效连接: repl_realtime_send(c, raw, rawlen)
    │                └─ transport ops → send()
    │                     ├─ TCP:    queue_bytes → reactor on_write → send()
    │                     ├─ eBPF:   queue_bytes → send() → BPF sk_msg 内核拦截重定向
    │                     └─ kprobe: 返回 -1，由内核态 kprobe 拦截 send() 后经
    │                               ringbuf→RDMA WRITE 转发; TCP 路径同时运行作保底
    │
    └─ 6. queue_bytes(c, response)              回复客户端 "+OK\r\n"
```

### 2.3 RESP 协议解析流程

```
parse_resp_stream(c, buf, &len, from_replication)
    │
    ├─ 逐字节扫描 buf[0..len]
    │
    ├─ 遇到 '+' → 简单字符串 (如 +FULLRESYNC ...)
    │     用于复制协议的控制消息
    │
    ├─ 遇到 '*' → 数组（RESP 命令）
    │     解析数组长度 n
    │     然后解析 n 个 bulk string ($<len>\r\n<data>\r\n)
    │     构建 argv[], argl[]
    │     调用 handle_parsed_command(c, argc, argv, argl, raw, rawlen, from_replication)
    │
    └─ from_replication 参数:
         ├─ 0: 来自客户端 → 执行持久化 + 广播
         └─ 1: 来自复制 → 只执行引擎操作，不广播，但写 AOF + 跟踪 offset
```

---

## 3. 全量持久化（SAVE/BGSAVE）

### 3.1 数据流概览

```
┌──────────────────────────────────────────────────────────┐
│                    SAVE / BGSAVE                          │
├──────────────────────────────────────────────────────────┤
│                                                          │
│  SAVE (同步)                                              │
│    │                                                     │
│    ├─ kvs_dump_to_fd(fd, aof_offset)                     │
│    │    ├─ 写入 8 字节 aof_offset（AOF 跳过基准）         │
│    │    └─ 遍历 5 个存储引擎，写入二进制条目:             │
│    │         [1B engine_id][4B klen][key][4B vlen][value]│
│    │    └─ fsync                                          │
│    └─ persist_mark_snapshot_success()                     │
│                                                          │
│  BGSAVE (异步)                                            │
│    │                                                     │
│    ├─ fork()                                             │
│    ├─ 子进程:                                             │
│    │    ├─ kvs_dump_to_fd(tmp_fd, aof_offset)            │
│    │    ├─ fsync + rename(tmp → dump_path)               │
│    │    └─ _exit(0)                                       │
│    └─ 父进程:                                             │
│         ├─ 返回继续服务                                   │
│         ├─ g_bgsave_pid = pid                            │
│         └─ persist_bgsave_poll() 轮询子进程状态           │
│                                                          │
│  自动快照 (autosnap)                                      │
│    │                                                     │
│    ├─ persist_autosnap_cron() 周期性调用                  │
│    ├─ 检查规则: dirty >= changes && elapsed >= seconds    │
│    └─ 触发 persist_bgsave_start()                         │
│                                                          │
└──────────────────────────────────────────────────────────┘
```

### 3.2 DUMP 文件格式（SAVE/BGSAVE 使用，二进制）

```
┌────────────────────────────────────────────────────────────────┐
│  [0..7]   uint64_t aof_offset    创建 dump 时的 AOF 文件大小   │
│                                  恢复时 AOF 从此处之后开始重放 │
├────────────────────────────────────────────────────────────────┤
│  [8..]    条目反复:                                            │
│    [1B]    uint8_t  engine_id    KVS_ENGINE_xxx                │
│    [4B]    uint32_t klen         key 长度                      │
│    [klen]  char[]   key          key 数据                      │
│    [4B]    uint32_t vlen         value 长度                    │
│    [vlen]  char[]   value        value 数据                    │
│                                                                │
│  DOC 引擎特殊处理:                                              │
│    value = "field1=val1 field2=val2 ..."                       │
│    (换行符 → 空格，避免破坏二进制格式)                         │
└────────────────────────────────────────────────────────────────┘
```

**aof_offset 的作用：**

```
时间线:  ───────┬────────────────┬─────────────────────→
               AOF 文件大小 = X  │  之后的新写命令
               dump 创建时刻     │
               
恢复时:  dump 恢复全量数据 → AOF 从偏移 X 处开始重放 → 恢复增量
         (aof_offset = X)       └─ 跳过这部分，因为 dump 已包含
```

### 3.3 SNAPSHOT 格式（全量同步、BGREWRITEAOF 使用，RESP 文本）

SNAPSHOT 是**另一种格式**，生成 RESP 命令序列，消费者（slave 或 AOF 重写目标文件）可以直接用 `parse_resp_stream` 解析。

生成方式：`snapshot_all_sink()` 遍历 5 个引擎，对每条 key 调用 `emit_cmd3_sink()` 生成标准 RESP：

```
  *3\r\n$4\r\nHSET\r\n$<klen>\r\n<key>\r\n$<vlen>\r\n<value>\r\n  ← hash 引擎
  *3\r\n$4\r\nRSET\r\n$<klen>\r\n<key>\r\n$<vlen>\r\n<value>\r\n  ← rbtree 引擎
  *3\r\n$3\r\nSET\r\n$<klen>\r\n<key>\r\n$<vlen>\r\n<value>\r\n   ← array 引擎
  *3\r\n$4\r\nXSET\r\n$<klen>\r\n<key>\r\n$<vlen>\r\n<value>\r\n  ← skiptable 引擎
  *4\r\n$6\r\nDOCSET\r\n...                                          ← doc 引擎
  *3\r\n$6\r\nHEXPIRE\r\n...                                         ← 附带 TTL
```

**为什么两种格式？**
- DUMP（二进制）：紧凑，仅用于本地 SAVE/BGSAVE 落盘和恢复
- SNAPSHOT（RESP）：能被 slave 和 AOF 重写文件直接消费，无需额外解析器

### 3.4 恢复流程

```
persist_recover()
    │
    ├─ 1. replay_dump_file(dump_path)
    │      ├─ mmap dump 文件
    │      ├─ 读取 8 字节 aof_offset
    │      ├─ 循环解析二进制条目 → 分配到对应的存储引擎
    │      └─ 返回 aof_offset 作为 AOF 跳过量
    │
    ├─ 2. replay_file(aof_path, aof_offset)
    │      ├─ mmap AOF 文件（fallback: fread）
    │      ├─ 从 aof_offset 处开始（跳过 dump 已包含的部分）
    │      ├─ parse_resp_stream(NULL, buf, &len, from_replication=1)
    │      └─ 重放所有增量 RESP 命令
    │
    ├─ 3. kvs_active_expire_cycle(1000000)
    │      清理恢复过程中已过期的 TTL key
    │
    └─ 4. 重置 dirty_counter、snapshot_ms 等状态
```

**为什么 DUMP 格式是二进制的？**

- 更紧凑，写入/读取更快
- 8 字节 aof_offset 精确标记 AOF 恢复起点
- 恢复时先读 DUMP→再读 AOF 从 offset 开始，不重放冗余命令

**为什么 SNAPSHOT 格式是 RESP 的？**

- 全量同步时可直接发给 slave 执行（parse_resp_stream 兼容）
- BGREWRITEAOF 时可直接写入 AOF 文件

---

## 4. 增量持久化（AOF）

### 4.1 AOF 写入路径

```
写命令执行
    │
    ▼
persist_append_raw(raw, rawlen)          raw = 原始 RESP 字节
    │
    ├─ 如果 g_aof_fd < 0 (AOF 禁用) → 直接返回 0
    │
    ├─ 如果 rawlen >= AOF_BUF_SIZE(64KB):
    │    │  单个命令超过缓冲区容量（极少见），不经过缓冲
    │    ├─ 先 flush 现有缓冲区
    │    └─ 直接 io_uring write+fsync 到磁盘
    │
    └─ 否则:
         ├─ 如果 g_aof_buf_len + rawlen > 64KB: 先 flush 缓冲区腾空间
         ├─ memcpy(raw → g_aof_buf + g_aof_buf_len)
         ├─ g_aof_buf_len += rawlen
         ├─ g_aof_dirty = 1   ← 标记"缓冲区有待刷数据"
         │     EVERYSEC 模式用此标记判断是否需要刷盘；
         │     进程退出时 persist_close() 检查是否有未刷数据
         │
         └─ fsync 策略:
              ├─ ALWAYS:
              │    ├─ 每次 reactor 迭代结束时调 persist_flush_pending()
              │    │   (一次迭代可能处理多条命令，batch flush 减少 fsync 次数)
              │    └─ 距上次 flush 超过 2ms → 强制 flush（延迟上限）
              │
              └─ EVERYSEC:
                   └─ persist_autosnap_cron() 中: 距上次 flush ≥ 1000ms → persist_force_aof_flush()
```

### 4.2 AOF Flush 到磁盘

```
persist_aof_flush_buffer()
    │
    ├─ 1. io_uring 批量提交: write SQE + fsync SQE
    │      ├─ io_uring_get_sqe → prep_write
    │      ├─ io_uring_get_sqe → prep_fsync (IORING_FSYNC_DATASYNC)
    │      ├─ io_uring_submit_and_wait(2)
    │      └─ 收集 2 个 CQE，按返回值区分 write/fsync
    │
    ├─ Fallback (io_uring 失败):
    │      ├─ pwrite + fsync
    │      └─ 或 fdatasync
    │
    └─ 2. 更新 g_aof_write_offset, g_aof_buf_len = 0
```

### 4.3 BGREWRITEAOF（AOF 重写）

```
persist_bgrewriteaof_start()
    │
    ├─ 1. persist_force_aof_flush()        先刷盘当前 AOF
    │
    ├─ 2. fork()
    │
    ├─ 子进程:
    │    ├─ persist_write_aof_snapshot_to(tmp)
    │    │    └─ kvs_snapshot_to_fd(fd) → RESP 格式遍历所有 key
    │    └─ _exit(0)
    │
    └─ 父进程:
         ├─ 继续服务
         ├─ append_to_rewrite_buffer()      新写命令同时写 rewrite buffer
         ├─ persist_bgrewriteaof_poll()     轮询子进程
         └─ 子进程退出后:
              └─ finalize_rewrite_parent()
                   ├─ 打开 tmp 文件
                   ├─ 追加 rewrite buffer 中的所有新命令
                   ├─ fsync
                   ├─ rename(tmp → aof_path)
                   ├─ 重新打开 g_aof_fd
                   └─ 释放 rewrite buffer
```

### 4.4 AOF 缓冲时序图

```
时间轴 ──────────────────────────────────────────────────→

ALWAYS 模式:
  reactor loop:
    [处理请求] → persist_append_raw → 写 buffer
    [处理请求] → persist_append_raw → 写 buffer
    ...
    persist_flush_pending() → flush 全部 buffer → io_uring write+fsync
    [下一个 reactor 迭代]
  
  (最长延迟: ~2ms，由 persist_append_raw 中的超时机制保证)

EVERYSEC 模式:
  reactor loop:
    [处理请求] → persist_append_raw → 写 buffer
    ...
    persist_autosnap_cron() → 距上次 flush >= 1000ms → flush
  (最长延迟: ~1s)
```

---

## 5. 全量同步（FULLRESYNC）

### 5.1 总体流程

```
SLAVE 启动                          MASTER
    │                                   │
    ├─ repl_slave_thread()              │
    │  后台线程循环                      │
    │                                   │
    ├─ TCP connect ────────────────────→ 接受连接
    │                                   │
    ├─ 发送 REPLSYNC <replid>           │
    │          <applied_offset> ────────→ handle_parsed_command()
    │          <durable_offset>          │   cmd = "REPLSYNC"
    │                                   │
    │                               ┌───┴──────────────┐
    │                               │ backlog 可续传?   │
    │                               │ replid 匹配?      │
    │                               │ offset 在范围内?   │
    │                               └───┬──────┬───────┘
    │                                   │ YES  │ NO
    │                                   ▼      ▼
    │                            +CONTINUE   queue_snapshot()
    │                            发送 backlog  (全量同步)
    │                                   │
    │                                   ▼
    │                          ┌─────────────────────┐
    │                          │ g_repl_fullsync_     │
    │                          │ in_progress = 1     │
    │                          │ (暂停实时广播)       │
    │                          └──────────┬──────────┘
    │                                     │
    │                          ┌──────────▼──────────┐
    │                          │ kvs_snapshot_to_fp()│
    │                          │ RESP 格式遍历全量数据│
    │                          └──────────┬──────────┘
    │                                     │
    │    +FULLRESYNC <replid>             │
    │    <snap_base_offset>   ←──────────┘
    │    <total_bytes>
    │                                     │
    │    snapshot data chunks ←───────────┘ (RDMA SEND 或 TCP)
    │                                     │
    ├─ parse_resp_stream(NULL,            │
    │   buf, len, from_replication=1)     │
    │   应用每条命令到引擎                  │
    │                                     │
    ├─ 达到 target_bytes:                  │
    │   repl_slave_finish_fullsync()       │
    │   ├─ kvs_dump_to_fd() 写 dump       │
    │   └─ repl_slave_state_save()         │
    │                                     │
    ├─ REPLACK <applied> ←──────────────→ 更新 ack offset
    │                  <durable>          │
    │                                     │
    └─ 进入增量同步模式 ←────────────────→ g_repl_fullsync_in_progress = 0
        (实时 AOF + 复制)                     恢复实时广播
```

### 5.2 全量同步期间的 client_capture

全量同步期间，客户端可能继续向 master 写入数据。这些数据需要被缓存，等全量同步完成后再发送给 slave。

```
全量同步期间 (g_repl_fullsync_in_progress = 1):

  BPF kprobe 挂载在 tcp_recvmsg 上
       │
  Client → Master 的 TCP 数据
       │
       ▼
  repl_client_capture.bpf.c:
    ├─ kprobe/tcp_recvmsg:  捕获 fd，记录到 per-CPU map
    ├─ kretprobe/tcp_recvmsg: 读返回值（收到的字节数）
    │
    ├─ L1 缓存 (BPF ringbuf):
    │    直接将数据写入 BPF ringbuf
    │    用户态 ring_buffer__poll 消费
    │
    └─ L2 缓存 (文件):
         当 ringbuf 满或 L1 flush 触发时
         写入临时文件
       
  全量同步完成后:
    ├─ client_capture L2 flush: 回放文件中的缓存命令
    ├─ client_capture L1 flush: 回放 ringbuf 中的剩余命令
    └─ REPLDONE 检测: 确认 slave offset 追上 master offset
```

### 5.3 部分重同步（CONTINUE）

**目的：** slave 短暂断开重连后，如果 backlog 还有它需要的增量数据，跳过全量同步，直接补发差额。

```
Slave 重连 → 发送 REPLSYNC <replid> <applied_offset> <durable_offset>

Master 检查（repl_backlog_can_continue）:
  ├─ replid 必须匹配（master 重启会重新生成 replid）
  ├─ applied_offset >= backlog.start_offset（slave 的断点还在 backlog 内）
  └─ applied_offset <= backlog.end_offset

  如果满足:
    → 发送 +CONTINUE <replid> <continue_offset>
    → repl_backlog_write_range(c, offset)
        从 backlog 环形缓冲中取出 [offset .. end] 的数据
        发送给 slave
        slave 收到后用 parse_resp_stream(from_replication=1) 逐条应用

  如果不满足（replid 变了 或 backlog 不够大）:
    → 发送 +FULLRESYNC → 全量同步
```

Backlog 是 1MB 环形缓冲，正常运行时**一直都**在记录。不是全量同步期间才启用的。它与 client_capture 的 BPF ringbuf 是不同层面的东西（见第 9 题）。

### 5.4 Replication Backlog

```
1MB 环形缓冲区 (g_repl_backlog):

  ┌─────────────────────────────────────────────┐
  │  环形缓冲区 (cap = 1MB)                      │
  │                                              │
  │  start_offset        head                    │
  │  ↓                   ↓                       │
  │  [已确认可丢弃][有效历史数据 (histlen)]       │
  │                                              │
  │  end_offset = start_offset + histlen         │
  └─────────────────────────────────────────────┘

  写入 (repl_backlog_feed):
    每次广播时追加 → end_offset += len, histlen += len
    如果溢出: head 前移，丢弃最旧数据

  读取 (repl_backlog_write_range):
    从 head + (offset - start_offset) 开始
    最多读到 end_offset
    环形 buffer 可能分两段读取
```

**Backlog vs BPF ringbuf（常见混淆点）：**

| | `g_repl_backlog` (1MB 用户态) | BPF ringbuf (内核→用户态) |
|---|---|---|
| **层** | 用户态环形缓冲 | 内核态共享内存 |
| **存什么** | 已广播出去的 RESP 命令原文 | kprobe/tcp_sendmsg 截获的网络数据 |
| **谁写入** | `repl_backlog_feed()` (每个写命令广播时) | BPF `bpf_ringbuf_submit()` (内核 kprobe) |
| **谁读取** | `repl_backlog_write_range()` (CONTINUE 时) | `ring_buffer__poll()` → `kprobe_ringbuf_cb()` |
| **用途** | 部分重同步 (CONTINUE) | kprobe+RDMA 零拷贝增量同步 |
| **生命周期** | 一直运行 | 仅 kprobe+RDMA 模式 |
| **与全量同步关系** | 一直记录；全量同步时暂停读取广播但继续写入 | 一直运行；与全量同步无关 |

**全量同步期间客户端数据的缓存**不是靠 backlog，而是靠 `client_capture`（BPF kprobe+ringbuf + L2 文件缓存，见 5.2 节）。

---

## 6. 增量同步（eBPF / kprobe+RDMA / eBPF+TCP）

增量同步有**两种数据路径**和**三种传输模式**。全量同步和增量同步的传输层**独立配置**。

### 6.1 传输模式总览

```
全量同步 (repl_fullsync_transport):         增量同步 (repl_realtime_transport):
  ┌─────────────┬──────────────────┐         ┌──────────────┬─────────────────────────┐
  │ 配置值      │ 传输方式          │         │ 配置值       │ 传输方式                 │
  ├─────────────┼──────────────────┤         ├──────────────┼─────────────────────────┤
  │ rdma (默认) │ RDMA SEND         │         │ tcp (默认)   │ TCP send                │
  │             │ fallback: TCP     │         │ ebpf/sockmap │ BPF sk_msg redirect     │
  │ tcp         │ TCP send          │         │              │ → sockmap egress        │
  │             │                   │         │              │ → 或 sockmap ingress    │
  └─────────────┴──────────────────┘         │ kprobe-rdma  │ kprobe 拦截 tcp_sendmsg │
                                              │              │ → ringbuf → RDMA WRITE │
                                              │ ebpf+tcp     │ BPF sk_msg redirect    │
                                              │              │ → 跨机 TCP 转发        │
                                              └──────────────┴─────────────────────────┘

两个配置独立，可以组合使用，例如:
  repl_fullsync_transport = rdma    (全量用 RDMA)
  repl_realtime_transport = ebpf    (增量用 eBPF sockmap)
```

### 6.2 模式 A: eBPF sockmap（本地 redirect）

**原理：** 利用 BPF sk_msg 程序在内核态拦截 `sendmsg`，将数据从 master socket 重定向到 slave socket 的发送/接收队列，**绕过用户态拷贝**。

```
MASTER (sock_map[0] = master_fd)          SLAVE (sock_map[1] = slave_fd)
                                                   
  repl_broadcast(raw, rawlen)                    
      │                                          
      ▼                                          
  queue_bytes → reactor on_write → send(master_fd)
      │                                           
      ▼                   [内核态]                
  BPF sk_msg 程序:         TCP 互联                
    ├─ 判断 role = MASTER                         
    ├─ redirect=1 且 ingress=1:                   
    │    bpf_msg_redirect_map(msg, sock_map,       
    │       redirect_key, BPF_F_INGRESS)          
    │    → 数据进入目标 socket 的接收队列 (本地)  
    │                                             
    └─ redirect=1 且 ingress=0:                   
         bpf_msg_redirect_map(msg, sock_map,       
            redirect_key, 0)                      
         → 数据进入目标 socket 的发送队列          
         → 走 TCP 传到远端 slave                    
                                                   
  sock_map 结构:                                  
    key 0 → master 端的 socket fd                
    key 1 → slave TCP 连接的 fd (redirect target) 
```

**跨机的工作原理：**

sockmap 本身不能跨机——它只能在同一内核内 redirect socket buffer。跨机的关键是：sock_map[redirect_key] 里存的是**已经连接到远端 slave 的 TCP socket fd**。`bpf_msg_redirect_map` 不带 `BPF_F_INGRESS` 时，数据进入目标 socket 的**发送队列**，然后内核 TCP 栈正常发包到远端。所以本质是：BPF 在 master 侧本地 redirect → 目标 socket 的 TCP 栈 → 跨机传输。

### 6.3 模式 B: eBPF+TCP（跨机转发）

这是 "ebpf+tcp" 配置对应的路径。与 6.2 一样用 `bpf_msg_redirect_map`，但 `ebpf_forward=1` 时设置 `KVS_EBPF_CTL_REDIRECT_INGRESS=0`（egress 模式），数据经过 TCP 到达远端 slave 后正常 recv+parse。

```
Master 侧:                                 Slave 侧:
  send(master_fd)                            recv(slave_fd)
    │                                          │
  BPF sk_msg:                                  ▼
    bpf_msg_redirect_map(msg, sock_map,   parse_resp_stream(NULL,
      redirect_key, 0)  ← flag=0           buf, len, from_replication=1)
    → sock_map[redirect_key] = slave TCP fd
    → 数据进入 slave socket 的发送队列
    → 内核 TCP 发包 ───────── TCP ──────────→ 远端 slave 进程 recv
```

### 6.4 模式 C: kprobe + RDMA WRITE
    │    bpf_msg_redirect_map(                └─ repl_slave_note_durable()
    │      msg, sock_map, redirect_key, 0)
    │    → 数据重定向到 sock_map[redirect_key]
    │      对应 socket 的发送队列
    │
    ├─ 如果 forward=1 (跨机):
    │    bpf_msg_redirect_map(
    │      msg, sock_map, redirect_key, 
    │      BPF_F_INGRESS=0) → egress redirect
    │    → 数据走 TCP 到远端 slave
    │
    └─ 否则: SK_PASS → 正常 TCP 路径

sock_map 结构:
  key 0 → master 监听 fd
  key 1 → slave TCP fd (redirect target)
```

### 6.3 模式 B: kprobe + RDMA WRITE 增量同步

```
MASTER                                          SLAVE

[用户态]                                  [用户态]
kprobe_rdma_forward_thread()              kprobe_rdma_slave_poll()
    │                                         │
    ├─ ring_buffer__poll(                    ├─ 轮询 MR 环形缓冲区
    │    ringbuf, timeout=5ms)               │   while (consumer_tail < producer_head)
    │    │                                    │     │
    │    ▼                                    │     ▼
    │  [内核 BPF kprobe]                     │   从 slot[consumer_tail] 读取数据
    │  tcp_sendmsg() 被调用时:               │    ︙
    │    ├─ 过滤: PID 匹配? fd 匹配?          │   parse_resp_stream(NULL,
    │    ├─ 从 msghdr->msg_iter->iov         │     slot_data, slot_len,
    │    │  读取发送数据                      │     from_replication=1)
    │    ├─ 写入 BPF ringbuf:                │    ︙
    │    │  [4B len][payload]                │   consumer_tail++
    │    └─ bpf_ringbuf_submit()             │   (volatile 写入, RDMA 可见)
    │                                        │
    │  [回到用户态回调]                       │
    │  kprobe_ringbuf_cb(ctx, data, size)    │
    │    │                                    │
    │    ├─ 解析 [4B len][payload]           │
    │    ├─ wr_slot_acquire() 取空闲 slot    │
    │    │   (8 slots × 512B, pipeline)      │
    │    ├─ memcpy → slot buf                │
    │    ├─ wr_submit_data(slot, len)        │
    │    │   RDMA WRITE data → slave MR      │
    │    │   (写入 slot[producer_seq])        │
    │    ├─ wr_submit_head(slot)             │
    │    │   RDMA WRITE producer_head        │
    │    │   → slave MR header               │
    │    └─ producer_seq++                   │
    │                                        │
    │  [TCP 路径同时运行]                    [TCP 路径同时接收]
    │  send() → TCP → slave                  recv() → parse_resp_stream
    │                                        │
    │                                        └─ repl_offset 去重
    │                                           (TCP 和 RDMA 可能收到相同数据)
    │                                           parse_resp_stream 中
    │                                           repl_slave_note_applied 检查
    │                                           offset 是否已应用
```

**kprobe+RDMA 数据流详解：**

```
Master 侧:
  tcp_sendmsg (内核)
    → BPF kprobe 拦截 (repl_kprobe.bpf.c)
    → 读取 msghdr 中的数据
    → 写入 BPF ringbuf: [4B len][cmd_bytes]
    → 用户态 ring_buffer__poll 回调
    → 获取 RDMA WRITE slot (8 slots pipeline)
    → ibv_post_send RDMA_WRITE:
        data → slave MR 的 slot 区域
        head → slave MR 的 producer_head
    → 注意: TCP 数据仍然正常发送到 slave (作为保底路径)

Slave 侧:
  ┌─────────────────────────────────────────────┐
  │  kprobe_rdma_ringbuf_t (MR 环形缓冲区)      │
  │                                              │
  │  [producer_head (volatile, 8B)]              │
  │  [consumer_tail (volatile, 8B)]              │
  │  [slot 0: 512B]                              │
  │  [slot 1: 512B]                              │
  │  ...                                         │
  │  [slot 1023: 512B]                           │
  └─────────────────────────────────────────────┘

  poll 线程:
    while running:
      读取 producer_head (RDMA 写入的 volatile 值)
      while consumer_tail < producer_head:
        从 slot[consumer_tail % 1024] 读数据
        parse_resp_stream(NULL, data, len, from_replication=1)
        consumer_tail++ (本地更新)
      usleep(100)
```

### 6.4 传输层抽象

```
repl_transport_ops_t:
  ├─ TCP:
  │    .send = repl_transport_tcp_send → queue_bytes → send()
  │    .connect_slave = socket + connect
  │    .disconnect_slave = close
  │
  ├─ eBPF:
  │    .send = repl_transport_ebpf_send → queue_bytes → send()
  │           (同样调用 send(), 但 fd 已注册到 sockmap,
  │            BPF sk_msg 在内核态拦截并重定向)
  │    .connect_slave = TCP connect + repl_ebpf_register_fd()
  │    .disconnect_slave = repl_ebpf_unregister_fd() + close
  │
  ├─ RDMA:
  │    .send = repl_rdma_try_send
  │           → ibv_post_send (RDMA SEND, pipeline 模式)
  │    .connect_slave = RDMA CM 完整握手流程
  │    .disconnect_slave = RDMA 资源清理
  │
  └─ kprobe-rdma:
       .send = 返回 -1 (不做实际发送, kprobe 在内核拦截)
       .connect_slave = kprobe RDMA 建链
       .disconnect_slave = kprobe RDMA 清理

传输选择逻辑:
  repl_transport_ops_for_context(KVS_REPL_SEND_FULLSYNC):
    → 根据 repl_fullsync_transport 配置选择
  repl_transport_ops_for_context(KVS_REPL_SEND_REALTIME):
    → 根据 repl_realtime_transport 配置选择

Fallback 机制:
  如果 RDMA/eBPF 发送失败 → 自动 fallback 到 TCP
  (cooldown 期内不再重试)
```

---

## 7. 从机保存数据到磁盘

### 7.1 Slave 端写路径（全量同步→增量同步）

**全量和增量在 slave 侧是时序上的先后关系，不能同时进行。**

```
Slave 接收数据 (from_replication=1):
    │
    ▼
parse_resp_stream(NULL, buf, len, from_replication=1)
    │
    ▼
handle_parsed_command(c=NULL, ..., from_replication=1)
    │
    ├─ 1. 应用到存储引擎
    │
    ├─ 2. 持久化:
    │    ├─ 全量同步阶段 (g_slave_loading_fullsync = 1):
    │    │    └─ 不写 AOF — 全量数据通过 repl_slave_finish_fullsync()
    │    │       中的 kvs_dump_to_fd() 一次性写入二进制 dump
    │    │
    │    └─ 增量同步阶段 (g_slave_loading_fullsync = 0):
    │         ├─ persist_append_raw(raw, rawlen) → AOF 缓冲
    │         └─ repl_slave_note_durable(rawlen)
    │              g_slave_repl_durable_offset += rawlen
    │              repl_slave_state_save() 保存状态
    │
    └─ 3. 跟踪 offset:
         └─ repl_slave_note_applied(rawlen)
              g_slave_repl_applied_offset += rawlen
              全量同步阶段:
                g_slave_fullsync_loaded_bytes += rawlen
                当 loaded >= target → repl_slave_finish_fullsync()
                  ├─ g_slave_loading_fullsync = 0  ← 切换到增量模式
                  ├─ kvs_dump_to_fd() 写二进制 dump
                  └─ 之后收到的数据走增量 AOF 路径（上面的分支）
```

**时间线：**

```
Slave 时间线:
  ────────────────────────────────────────────────────────→
  [全量同步加载]               [增量同步]
  AOF 不写                     AOF 写入
  loaded_bytes 累计             applied_offset 累计
                               durable_offset 累计
                               
  repl_slave_finish_fullsync() ← 分界线: 写 dump + 切换模式
```

### 7.2 全量同步完成时的 Slave 处理

```
repl_slave_finish_fullsync()
    │
    ├─ 1. g_slave_loading_fullsync = 0
    │
    ├─ 2. kvs_dump_to_fd(dump_fd, 0)
    │      全量数据写入二进制 dump 文件
    │      之后的增量数据通过 AOF 持久化
    │
    ├─ 3. repl_slave_state_save()
    │      写入 repl_state 文件: <replid> <applied_offset> <durable_offset>
    │      (kvstore.aof.replstate)
    │
    └─ 4. repl_slave_send_ack()
           → REPLACK <applied> <durable> 发送给 master
```

### 7.3 Slave 状态持久化

```
Slave 重启恢复:
  ├─ persist_recover()
  │    ├─ replay_dump_file(dump_path)  → 恢复全量数据
  │    └─ replay_file(aof_path, aof_offset) → 重放增量 AOF
  │
  ├─ repl_slave_state_load()
  │    读取 kvstore.aof.replstate:
  │      <master_replid> <applied_offset> <durable_offset>
  │    用于向 master 请求部分重同步
  │
  └─ repl_slave_thread()
       连接 master
       发送 REPLSYNC <replid> <applied_offset> <durable_offset>
```

### 7.4 Slave AOF 的 AOF 重写

```
Slave 同样支持 BGREWRITEAOF:
  ├─ 因为 AOF 只追加不压缩，文件会持续增长
  ├─ 但 slave 的 AOF 来自 master 的增量同步
  ├─ 实际上：
  │   全量同步完成后写 dump → 充当 AOF 压缩的替代
  │   增量同步期间的 AOF 记录增量命令
  └─ 重启时：dump(全量) + AOF(增量) = 完整数据
```

---

## 8. 关键数据结构

### 8.1 连接 (conn_t)

```c
// include/kvstore/kvstore.h:248-267
typedef struct conn_s {
    int fd;                          // socket fd
    int is_listener;                 // 是否监听 socket
    int is_replica;                  // 是否 replicate 连接
    int repl_draining;               // 是否正在断开
    int repl_fullsync_pending;       // 是否等待全量同步
    int repl_transport_kind;         // 传输层类型
    unsigned long long repl_offset_sent;      // 已发送 offset
    unsigned long long repl_applied_offset_ack;  // slave 已应用 offset
    unsigned long long repl_durable_offset_ack;  // slave 已持久化 offset
    long long repl_last_send_ms;     // 上次发送时间
    long long repl_last_ack_ms;      // 上次 ACK 时间
    unsigned char inbuf[65536];      // 输入缓冲
    unsigned char outbuf[65536];     // 输出环形缓冲
    size_t out_head, out_tail, out_len;
    struct conn_s *next_replica;     // replica 链表
} conn_t;
```

### 8.2 复制 Backlog

```c
// src/replication/kvs_repl.c:40-47
typedef struct repl_backlog_s {
    unsigned char *buf;              // 1MB 环形缓冲
    size_t cap;                      // 容量 (1MB)
    size_t histlen;                  // 有效历史长度
    size_t head;                     // 起始位置索引
    unsigned long long start_offset; // 起始 offset (全局)
    unsigned long long end_offset;   // 结束 offset (全局)
} repl_backlog_t;
```

### 8.3 kprobe RDMA 环形缓冲区

```c
// include/kvstore/replication/repl_kprobe.h
typedef struct kprobe_rdma_ringbuf_s {
    // 16 字节 header (cache line 对齐)
    volatile uint64_t producer_head;  // Master 写入 (RDMA WRITE)
    volatile uint64_t consumer_tail;  // Slave 读取后更新
    uint8_t _pad[48];                 // padding to 64B
  
    // slot 数组: 1024 slots × 512 bytes
    uint8_t slots[1024][512];
} kprobe_rdma_ringbuf_t;
```

### 8.4 AOF 缓冲区

```c
// src/persistence/kvs_persist.c:36-39
#define AOF_BUF_SIZE 65536
static unsigned char g_aof_buf[AOF_BUF_SIZE];  // 64KB 写缓冲
static size_t g_aof_buf_len = 0;               // 当前缓冲长度
static long long g_aof_write_offset = 0;       // AOF 文件写入偏移
static int g_aof_dirty = 0;                    // 脏标记
```

### 8.5 关键全局状态

```c
// 持久化
int g_aof_fd;                        // AOF 文件 fd
pid_t g_bgsave_pid;                  // BGSAVE 子进程
unsigned long long g_dirty_counter;  // 自上次快照以来的写次数
long long g_last_snapshot_ms;        // 上次快照时间

// 复制 (Master 侧)
char g_master_replid[41];            // Master replication ID
unsigned long long g_master_repl_offset;  // 全局复制 offset
conn_t *g_replicas;                  // replica 连接链表
volatile int g_repl_fullsync_in_progress; // 全量同步进行中

// 复制 (Slave 侧)
unsigned long long g_slave_repl_applied_offset;  // 已应用 offset
unsigned long long g_slave_repl_durable_offset;  // 已持久化 offset
int g_slave_loading_fullsync;        // 是否在全量同步中
unsigned long long g_slave_fullsync_target_bytes; // 全量同步目标字节
```

---

## 9. 配置项速查


| 配置项                    | 默认值          | 说明                                       |
| ------------------------- | --------------- | ------------------------------------------ |
| `role`                    | master          | master/slave                               |
| `repl_transport_backend`  | tcp             | tcp/rdma/ebpf/sockmap/kprobe-rdma/ebpf+tcp |
| `repl_fullsync_transport` | rdma            | 全量同步传输层                             |
| `repl_realtime_transport` | tcp             | 增量同步传输层                             |
| `appendfsync`             | always          | always/everysec                            |
| `autosnap`                | (无)            | 自动快照规则，格式:`seconds:changes,...`   |
| `ebpf_enabled`            | 0               | 是否启用 eBPF                              |
| `ebpf_redirect`           | 0               | 是否启用 eBPF 重定向                       |
| `ebpf_forward`            | 0               | 是否启用 eBPF 跨机转发                     |
| `ebpf_pin_path`           | /sys/fs/bpf/... | BPF map pin 路径                           |
| `kprobe_enabled`          | 1               | 是否启用 kprobe+RDMA                       |
| `rdma_dev`                | siw0            | RDMA 设备名                                |
| `rdma_port`               | port+1          | RDMA 端口                                  |
| `rdma_recv_slots`         | 64              | RDMA 接收 slot 数                          |
| `rdma_chunk_size`         | 256KB           | RDMA 分块大小                              |
| `rdma_qp_wr_depth`        | 64              | QP work request 深度                       |

---

## 附录：源码索引


| 功能                  | 关键函数                                                      | 源文件:行号                               |
| --------------------- | ------------------------------------------------------------- | ----------------------------------------- |
| RESP 解析入口         | `parse_resp_stream()`                                         | `src/main/kvstore.c:1790`                 |
| 命令分发              | `handle_parsed_command()`                                     | `src/main/kvstore.c:1000`                 |
| 写命令处理            | `engine_set()` → `persist_append_raw()` + `repl_broadcast()` | `src/main/kvstore.c:1655-1776`            |
| AOF 追加              | `persist_append_raw()`                                        | `src/persistence/kvs_persist.c:569`       |
| AOF 刷盘              | `persist_aof_flush_buffer()`                                  | `src/persistence/kvs_persist.c:199`       |
| SAVE                  | `persist_save_dump()`                                         | `src/persistence/kvs_persist.c:608`       |
| BGSAVE                | `persist_bgsave_start()`                                      | `src/persistence/kvs_persist.c:660`       |
| BGREWRITEAOF          | `persist_bgrewriteaof_start()`                                | `src/persistence/kvs_persist.c:724`       |
| 恢复                  | `persist_recover()`                                           | `src/persistence/kvs_persist.c:619`       |
| DUMP 生成             | `kvs_dump_to_fd()`                                            | `src/main/kvstore.c:2126`                 |
| SNAPSHOT 生成         | `kvs_snapshot_to_fd()`                                        | `src/main/kvstore.c:2102`                 |
| 复制广播              | `repl_broadcast()`                                            | `src/main/kvstore.c:477`                  |
| 全量同步              | `queue_snapshot()`                                            | `src/replication/kvs_repl.c:538` (approx) |
| Backlog 续传          | `repl_backlog_send_continue()`                                | `src/replication/kvs_repl.c:2127`         |
| Slave 线程            | `repl_slave_thread()`                                         | `src/replication/kvs_repl.c`              |
| Slave 全量完成        | `repl_slave_finish_fullsync()`                                | `src/replication/kvs_repl.c:1902`         |
| eBPF sockmap 加载     | `repl_ebpf_load_object()`                                     | `src/replication/kvs_repl_ebpf.c:155`     |
| kprobe BPF 加载       | `kprobe_load_bpf()`                                           | `src/replication/kvs_repl_kprobe.c:114`   |
| kprobe ringbuf 回调   | `kprobe_ringbuf_cb()`                                         | `src/replication/kvs_repl_kprobe.c`       |
| Slave MR poll         | `kprobe_rdma_slave_poll()`                                    | `src/replication/kvs_repl_kprobe.c`       |
| client_capture 初始化 | `repl_client_capture_init()`                                  | `src/replication/kvs_repl_kprobe.c`       |
| 传输层选择            | `repl_transport_ops_for_context()`                            | `src/replication/kvs_repl.c:1583`         |
| 自动快照定时器        | `persist_autosnap_cron()`                                     | `src/persistence/kvs_persist.c:897`       |
| TTL 过期              | `kvs_active_expire_cycle()`                                   | `src/expire/kvs_expire.c`                 |
