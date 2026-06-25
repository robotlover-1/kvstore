# 增量同步修复：ring buffer + 零拷贝解析导致数据丢失

## 问题现象

`test_repl_5w5w` 跨机测试（RDMA 全量 + TCP 增量）卡在 Phase 5：

```
master_link=up  master_offset=4538890  slave_offset=2238890  loading=0
追赶中...  落后 2.2MB
```

从机增量数据完全收不到或只收到部分，测试无法完成。

## 定位过程

通过二分 commit 定位到 `935c15b`（"perf: output ring buffer"）引入了问题。该 commit 之前（`7b3731f`）复制正常，之后失败。

对比两个版本，发现两个独立 bug 叠加。

---

## Bug 1：replica 的 EPOLLOUT 未被注册

### 根因

`935c15b` 将 `queue_bytes` 从链表改为 ring buffer 时，删除了末尾的 `mod_events(c, EPOLLIN | EPOLLOUT)` 调用。

```c
// 链表版本（工作版本）
int queue_bytes(conn_t *c, const unsigned char *buf, size_t len) {
    ...
    mod_events(c, EPOLLIN | EPOLLOUT);  // ← 注册 EPOLLOUT
    return 0;
}

// ring buffer 版本（935c15b，有问题）
int queue_bytes(conn_t *c, const unsigned char *buf, size_t len) {
    ...
    c->out_ring_tail = (tail + len) & (OUT_RING_SIZE - 1);
    c->out_ring_len += len;
    return 0;  // ← 没有注册 EPOLLOUT！
}
```

### 为什么会导致 replica 数据发不出去

```
repl_broadcast()                         reactor 事件循环
     │                                        │
     ├─ repl_realtime_send(replica)            │
     │   └─ queue_bytes(replica)              │
     │       └─ 数据写入 replica ring buffer   │
     │       └─ mod_events(EPOLLOUT) ← 旧版   │
     │                                        │
     │  新版没有 mod_events ──────────→       │  replica 的 EPOLLOUT
     │                                        │  永远不会被触发
     │                                        │
     ▼                                        ▼
  数据困在 ring buffer                    on_write 只处理 client 连接
```

`repl_broadcast` 在 client 连接的 `on_read` 上下文中把数据写入 replica 连接的 ring buffer。但 reactor 只为**当前连接**（client）检查并刷新输出缓冲。replica 连接的 EPOLLOUT 需要显式注册。

旧版 `queue_bytes` 每次都会调 `mod_events` 注册 EPOLLOUT，所以 replica 的数据能被 reactor 发送。新版删掉后，无人为 replica 注册 EPOLLOUT。

### 修复

`src/core/reactor.c` 的 `queue_bytes` 末尾补回一行：

```c
c->out_ring_tail = (tail + len) & (OUT_RING_SIZE - 1);
c->out_ring_len += len;
mod_events(c, EPOLLIN | EPOLLOUT);  // ← 补回
return 0;
```

---

## Bug 2：零拷贝解析器污染广播数据

### 根因

`parse_resp_stream` 的零拷贝优化将 `argv[i]` 直接指向 `inbuf`，通过 `buf[p + blen] = '\0'` 做字符串终结。这覆盖了原始 RESP 中的 `\r`。

```c
// 零拷贝版本（有问题）
argv[i] = (char *)(buf + p);        // 直接指 inbuf
buf[p + (size_t)blen] = '\0';       // 覆盖 \r → \0！
argl[i] = (size_t)blen;
p += (size_t)blen + 2;

// handle_parsed_command 收到的 raw 参数
// 指向已被修改的 buf，\r 变成了 \0
handle_parsed_command(c, ..., buf + start, ..., from_replication);
```

原始 RESP `SET\r\n` 变成 `SET\0\n`。`repl_broadcast(raw, rawlen)` 发送的是被污染的数据。

```c
// copy 版本（工作版本，935c15b 原版）
argv[i] = (char *)kvs_malloc((size_t)blen + 1);
memcpy(argv[i], buf + p, (size_t)blen);
argv[i][blen] = 0;                   // 在新内存上 null-terminate
// 原始 buf 保持不变
```

### 为什么会导致从机拒绝数据

从机收到 `SET\0\n` 后，`parse_resp_stream` 的 `\r\n` 终结符校验失败：

```c
if (!(buf[p + blen] == '\r' && buf[p + blen + 1] == '\n')) {
    malformed = 1;  // \0 ≠ \r，判定为 malformed，丢弃
    break;
}
```

### 修复

`src/main/kvstore.c` 的 `parse_resp_stream` 还原为 copy 方式：

```c
argv[i] = (char *)kvs_malloc((size_t)blen + 1);
memcpy(argv[i], buf + p, (size_t)blen);
argv[i][blen] = 0;
```

---

## 最终修复（2 个文件）

| 文件 | 改动 | 作用 |
|------|------|------|
| `src/core/reactor.c` | `queue_bytes` 补 `mod_events(EPOLLOUT)` | replica 数据能发出 |
| `src/main/kvstore.c` | `parse_resp_stream` 还原 copy 方式 | 广播数据不被污染 |

---

## 验证

`test_repl_5w5w` 跨机测试（Master 192.168.233.128 / Slave 192.168.233.129）：

```
Phase 5: 增量同步完成! slave offset (4538890) >= master offset (4538890)
Phase 6: 验证 Slave 数据一致性 — 24/24 PASS
全部通过! PASS: 24 FAIL: 0
```

## 关键教训

1. **ring buffer 替代链表时，EPOLLOUT 注册不能丢**。链表版的 `queue_bytes` 末尾有 `mod_events`，ring buffer 版必须保留这个行为。
2. **零拷贝优化需要保证原始 buffer 不被修改**，否则依赖原始 buffer 的下游逻辑（如 `repl_broadcast(raw, ...)`）会收到损坏数据。
3. 两个 bug 独立但叠加：Bug 1 导致数据发不出，Bug 2 导致发出的数据是坏的。修一个只能部分改善，两个都修才能完全恢复。
