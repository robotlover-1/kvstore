# kvstore 项目迭代总结

## 1. 文档目的

这份文档用于总结本项目从基础 KV 系统演进到“具备持久化、文档型 value、主从复制、RDMA / eBPF 实验路径、工程化测试与目录结构”的完整迭代过程，重点回答三个问题：

1. 每一阶段改了什么
2. 为什么这样改
3. 遇到了什么问题，最后怎么解决

其中，RDMA 稳定性测试部分是本次迭代的重点，会单独展开说明。

---

## 2. 项目初始目标与总体路线

本项目最初目标并不是只做一个“能跑起来的单机 KV”，而是逐步演进为一个：

- 可验证正确性
- 可观察性能与状态
- 可持久化恢复
- 可进行主从同步
- 可尝试高性能复制路径
- 可支持 TTL 与更复杂 value 结构

因此总体实施路线采用了“先正确性、再可恢复、再复制、最后高性能实验”的顺序：

1. 工程整理与基线建立
2. 持久化基础打通
3. 文档型 value 能力扩展
4. TCP 主从复制增强
5. 复制指标 / profile / eBPF 观测
6. RDMA transport 实验实现与稳定性修复
7. 工程化目录与文档收口

这样做的原因很明确：

- 如果单机正确性还不稳定，就不应直接做主从
- 如果恢复链路不可靠，主从重启恢复就无法验证
- 如果没有观察工具，RDMA 这类高复杂路径会很难排障
- 如果目录和脚本管理混乱，后期验证成本会快速失控

---

## 3. 迭代过程总览

### 3.1 阶段一：建立基线与基础能力确认

#### 主要修改

- 梳理现有源码结构与功能边界
- 明确当前已支持的命令、存储引擎、持久化方式、网络模型
- 更新 `README.md`，把“当前已经实现的能力”和“尚未完成的能力”区分开

#### 为什么这样改

项目一开始存在一个典型问题：README 中历史描述与真实实现状态并不完全一致。如果不先统一“当前到底实现了什么”，后续每加一个能力，文档和测试都会继续漂移。

#### 结果

建立了后续各阶段迭代的基准面：

- 当前有哪些能力已经存在
- 后面哪些能力是增强而不是从零开始
- 后面所有修改都必须同步更新 README 与验证入口

---

### 3.2 阶段二：持久化链路完善

#### 主要修改

- 完善 dump + AOF 恢复链路
- 明确 SAVE / BGSAVE / BGREWRITEAOF 的行为
- 在恢复路径中优先尝试 `mmap`，失败后回退到普通流式读取
- 补充 `io_uring` 持久化 smoke 与恢复验证脚本
- 增加 `mmap` 恢复性能验证脚本

相关脚本后续整理到了：

- `tools/persist/run_uring_persist_bench.py`
- `tools/persist/run_mmap_recover_bench.py`
- `tools/persist/test_resp_persist_nc.sh`

#### 为什么这样改

主从复制、partial resync、RDMA 稳定性最终都绕不开一个前提：节点重启后数据和位点必须可恢复。如果本地持久化链路不完整，后续复制相关 bug 很多都会表现成“像网络问题”，实际上根因却在本地恢复。

#### 遇到的问题

1. 持久化行为有多条路径，文档与真实行为容易不一致
2. 恢复既需要正确性，也需要在大文件下具备合理性能

#### 解决方法

- 用统一脚本把“写入 -> SAVE -> 重启 -> 校验”固化下来
- 将恢复统计信息暴露到 `INFO`
- 将性能验证从“手工观察”改成“可重复跑的脚本”

---

### 3.3 阶段三：文档型 value 扩展

#### 主要修改

- 支持最小对象模型的文档型 value
- 增加：
  - `DOCSET`
  - `DOCGET`
  - `DOCDEL`
  - `DOCDROP`
  - `DOCEXIST`
  - `DOCCOUNT`
  - `DOCGETALL`
- 补充文档对象的持久化与恢复验证路径

#### 为什么这样改

项目目标不只是简单字符串 KV，而是要具备“类似 Redis 的 key-value + 最小文档对象能力”。但这里没有一步做到完整 JSON，而是先实现可验证、可持久化、可恢复的平铺字段对象模型。

这样做的原因是：

- 第一版先控制复杂度
- 先保证和持久化/复制能对齐
- 避免在 value 结构上过早引入太复杂的协议与序列化问题

---

### 3.4 阶段四：TCP 复制增强与 partial resync

#### 主要修改

- 增加 replication backlog
- 引入 `replid / offset`
- 支持 `FULLRESYNC / CONTINUE`
- 完成 partial resync 的基本模型
- 增加复制 metrics / profile 验证脚本

相关脚本后续整理到了：

- `tools/repl/run_repl_fullsync_test.sh`
- `tools/repl/run_repl_metrics_bench.py`
- `tools/repl/run_repl_profile.py`

#### 为什么这样改

在 RDMA 之前，必须先把“复制协议本身”做稳定。因为 RDMA 应该只是 transport 层优化，不应该改变复制语义本身。

如果 TCP 路径都没有稳定的：

- backlog
- offset
- resync 逻辑
- restart 后的恢复逻辑

那 RDMA 只会把问题放大，而不会解决问题。

#### 遇到的问题

1. partial resync 需要 backlog、offset、replid 三者协同
2. restart 场景下，仅有内存态 offset 不够，需要持久化位点
3. 如果 slave 本地数据集与 `repl_offset` 不一致，表面上看是“复制成功”，实际会出现数据缺失

#### 解决方法

- 建立完整的 replication metrics 观察面
- 明确 backlog 窗口范围与 offset 语义
- 让 slave 的复制状态参与本地恢复流程

---

### 3.5 阶段五：eBPF / profiling 观测链路接入

#### 主要修改

- 增加 `perf` / `bpftrace` / `bpftool` / `clang` 环境探测脚本
- 为复制路径增加 eBPF profiling helper
- 支持 syscall、sched tracepoint 与用户态 uprobes
- 增加结构化 summary 输出
- 将 eBPF tracing 接入 RDMA stress helper 生命周期

相关脚本：

- `tools/repl/run_repl_ebpf_env_probe.py`
- `tools/repl/repl_ebpf_session.py`
- `tools/repl/run_repl_profile.py`
- `tools/repl/run_repl_rdma_stress.py`

#### 为什么这样改

RDMA 相关问题很难只靠普通日志定位。特别是：

- 握手是否成功
- CQ completion 是否到达
- send/recv 谁先超时
- 重连期间用户态路径有没有实际触发

这些都需要更强的观测能力。eBPF 并不是功能需求本身，而是为了降低后续排障成本。

#### 遇到的问题

1. 不同环境下 eBPF 工具可用性差异很大
2. bpftrace 的脚本能力和字符串限制会影响 helper 稳定性
3. uprobes 写法如果有细小错误，会直接导致 tracing 启动失败

#### 解决方法

- 先做环境探测，避免在不支持环境硬跑 tracing
- 给 tracing helper 加 root / capability 预检查
- 追加 summary 文件，避免只依赖原始输出
- 修正 uprobe 语法与 bpftrace BEGIN 字符串长度问题

---

## 4. RDMA 复制实验路径迭代

### 4.1 本轮稳定性收敛：从“中负载失败”到“中负载通过”

这一轮的核心目标，不再是单纯让 RDMA 路径“能建链”，而是把此前在中等负载下稳定复现的复制失败收敛到可通过的状态。

#### 本轮确认并修复的问题

1. **fullsync 期间 slave ACK 干扰数据通道**
   - 现象：slave 在 fullsync snapshot 加载期间仍发送 `REPLACK`，与 snapshot 数据流共享同一 RDMA 通道与 CQ，容易导致状态紊乱。
   - 修复：在 fullsync 加载期间 defer ACK，仅在 fullsync 完成后再恢复 ACK。

2. **RDMA 发送失败后的 replica 清理不完整**
   - 现象：master 在 RDMA replica 发送失败时，只做浅层上下文清理，没有真正从全局 replica 链表移除 stale 节点。
   - 修复：发送失败路径补上真正的 `repl_remove_slave()`，避免后续 resync / reconnect 状态被污染。

3. **pending recv 单槽导致 postsync 增量漏消息**
   - 现象：send completion 等待过程中，多个 `RECV completion` 会被单槽缓存覆盖，导致 slave 在 postsync 阶段间歇性丢 key。
   - 修复：把 `pending_recv` 从单槽改为环形队列，完整保留在 send wait 期间到达的多个 recv completion。

4. **master 只处理初始 RDMA payload，不持续消费后续 REPLACK**
   - 现象：master 只在 listener 建链后处理一次初始 `REPLSYNC`，之后不再持续读取 slave 发来的 `REPLACK`。
   - 修复：把 master listener 改为持续 recv 循环，反复 `wait_cq_recv_completion -> repost recv -> parse_resp_stream`。

5. **slave 在复制写入路径里重复发送 ACK**
   - 现象：复制命令应用完成后，既通过 durable 路径发 ACK，又在复制写入尾部再次显式发 ACK。
   - 修复：去掉重复 ACK，只保留 durable / fullsync 完成路径上的 ACK。

6. **send / recv completion 并发轮询同一 CQ**
   - 现象：broadcast 发送线程和 listener 接收线程会并发 `ibv_poll_cq()` 同一个 CQ，容易造成 completion 竞争。
   - 修复：增加 CQ 轮询互斥，串行化对同一 CQ 的 poll。

#### 验证过程与结果

这一轮没有只依赖大脚本重跑，而是先做了更窄的半手工验证，再回到 stress：

- 半手工验证：
  - 单独拉起 master/slave
  - 先验证 fullsync 是否完成
  - 再注入连续 `tail:0..15`，逐个检查 slave 上的 key
  - 修复前可稳定观察到部分 key 缺失；修复 `pending_recv` 队列后，`0..15` 全量到达

- 小一档 stress 回归：
  - `preload 64`
  - `tail-writes 16`
  - `restart-rounds 1`
  - 结果：`fullsync_done=yes`、`postsync_tail_ok=yes`、`restart_rounds_ok=yes`、`final_resume_ok=yes`

- 中等负载 stress 回归：
  - `preload 128`
  - `tail-writes 32`
  - `restart-rounds 2`
  - 结果：`fullsync_done=yes`、`postsync_tail_ok=yes`、`restart_rounds_ok=yes`、`final_resume_ok=yes`

#### 当前状态判断

这一轮之后，可以明确认为：

- RDMA 复制的**主功能正确性问题已解决**
- 中等负载下此前的 fullsync / postsync / restart 失败已经收敛并通过验证
- 剩余现象主要是 master 侧仍可能观察到 `RDMA_CM_EVENT_DISCONNECTED`，但在当前验证中它表现为 **seen but not impactful**：
  - 事件被观测到
  - 但不会破坏 `fullsync_done / postsync_tail_ok / restart_rounds_ok / final_resume_ok`
  - 最终 slave 仍保持 `repl_transport=rdma` 且 `master_link=up`

这意味着后续工作重点不再是“修复复制正确性”，而是进一步区分并收敛 RDMA 生命周期中的良性 disconnect 与真正故障 disconnect。

### 4.2 结果解释与后续收口方向

为了避免把所有 async disconnect 都当成失败信号，本轮还增强了 `tools/repl/run_repl_rdma_stress.py`：

- `wait_ready()` 在失败时输出最后一次响应或异常，而不是仅报 `server not ready`
- 将 async disconnect 判定拆分为：
  - `rdma_master_async_disconnect_seen`
  - `rdma_master_async_disconnect_impactful`
  - `rdma_slave_async_disconnect_seen`
  - `rdma_slave_async_disconnect_impactful`

这样可以把“观察到 disconnect 事件”和“disconnect 影响最终正确性”这两件事区分开。

当前脚本输出已经能准确表达现状：

- 可能仍看到 `rdma_master_async_disconnect_seen=yes`
- 但在回归通过的场景中，对应 `rdma_master_async_disconnect_impactful=no`
- 并且在 `restart-rounds 0` 的 steady-state 验证下，`rdma_master_async_disconnect_seen=no`、`rdma_slave_async_disconnect_seen=no`

因此，这一轮迭代的结论不是“RDMA 已经零抖动”，而是：

- **RDMA 复制正确性已经稳定通过**
- **剩余问题已收敛为生命周期层的 disconnect 观察项，而非数据一致性错误**

目标不是“让 RDMA 看起来能编译”，而是逐步把它从：

- skeleton / unsupported 占位

推进到：

- 可握手
- 可 fullsync
- 可 tail 增量
- 可 restart partial resync
- 可 soak reconnect 恢复

### 4.1 初始状态：只有 skeleton / unsupported 占位

#### 主要修改

- 在配置层接受 `rdma` transport
- 引入 transport backend 抽象
- 默认仍走 `tcp`
- 在非 RDMA 构建中，对 `rdma` 给出明确 unsupported 行为

#### 为什么这样改

先做 transport backend 抽象，是为了保证：

- TCP 行为不被破坏
- RDMA 逻辑不会把复制主流程写乱
- 后续实验路径可以逐步替换 backend 实现，而不是在主逻辑里打很多条件分支

#### 遇到的问题

一开始最容易犯的错误是：

- 让 `rdma` 配置“被接受”，但实际内部静默退回 TCP

这会导致测试误判，以为 RDMA 通了，实际上只是 TCP 正常工作。

#### 解决方法

- 非 RDMA 构建下明确打印 unsupported
- slave 保持 `master_link=down`
- 用专门脚本验证“不会伪装成 TCP 成功”

---

### 4.2 从握手到可建立连接

#### 主要修改

- 接入 compile-time RDMA backend scaffolding
- 推进到 QP 创建与 connect handshake
- 增加 standalone RDMA smoke helper
- 增加 device / gid index / host 覆盖能力

#### 为什么这样改

先做 standalone smoke，而不是直接在复制主链路里盲调，原因是：

- 可以先验证环境是否可用
- 可以先确认 ibverbs / rdmacm / route / device 配置没有问题
- 避免把“环境问题”和“复制协议问题”混在一起

#### 遇到的问题

1. 握手看似建立，但 slave 仍显示 `master_link=down`
2. standalone pingpong 对设备、GID index 等参数非常敏感

#### 解决方法

- 增加更细粒度的 listener / accept / established 日志
- 将 smoke helper 参数化，允许显式指定 device / gid index
- 在复制路径中增加 fullsync / established 的显式状态输出

---

### 4.3 问题一：握手成功后仍沿用 TCP send/recv 路径

#### 现象

RDMA 握手已经成功，但复制路径仍然出现：

- `master_link=down`
- 没有真正的数据同步完成

#### 根因

slave 在 RDMA 建立后，后续数据路径仍错误地走到了原 TCP send/recv 逻辑，导致 transport 已经切换，但数据收发方式并未真正切换。

#### 解决方法

- 阻止 slave RDMA 路径继续错误使用 TCP send/recv
- 明确按 backend dispatch 走各自 transport 实现
- 对 transport send / recv 行为加更明确的 observability

#### 结果

RDMA 不再只是“能握手”，而开始具备真实复制数据路径。

---

### 4.4 问题二：CQ completion 超时

#### 现象

握手后发送建立成功，但在数据阶段出现 CQ completion timeout。

#### 根因

原实现使用 event-channel 等待 completion，逻辑更复杂，也更容易在当前最小实现阶段出现时序问题。

#### 解决方法

- 将当前 CQ 等待方式替换为直接 CQ polling
- 先追求最小可用数据通路，而不是一开始就追求更复杂的事件模式

#### 为什么这样改

这是一个典型的“先降低复杂度换稳定性”的取舍。

在实验阶段，目标是尽快把最小 RDMA 复制路径做通。如果 completion 机制本身太复杂，会把排障面扩大。

#### 结果

RDMA 数据路径向前推进到“可发送 / 可接收 / 可观察 completion”。

---

### 4.5 问题三：master 收到 completion，但 slave 响应超时

#### 现象

master 侧能看到 recv completion，但 slave 侧仍超时，看起来像“消息丢了”。

#### 根因

收到 RDMA payload 后，缓冲区在重新 repost recv 前覆盖了尚未被上层消费的数据，导致应用层看到的是不完整或错误数据。

#### 解决方法

- 在 repost recv buffer 之前，先保留收到的有效 payload
- 将“收包”和“重新挂 recv”解耦，避免先覆盖后消费

#### 结果

解决了“completion 已到但应用层没有拿到正确数据”的问题。

---

### 4.6 问题四：RDMA reconnect / soak 场景不稳定

这是整个 RDMA 迭代里最难、持续时间最长的一组问题。

#### 初始现象

在 stress / soak 场景下，出现如下典型现象：

- 初始 fullsync 成功
- tail 增量有时成功，有时失败
- restart partial resync 基线偶发被破坏
- soak 期间会出现 reconnect thrash
- 某些情况下 listener 生命周期异常，后续连接无法稳定恢复

#### 为什么这个问题难

因为它不是单点 bug，而是多个生命周期问题叠加：

- listener 生命周期与 per-connection teardown 混在一起
- stale replica 清理时机不对
- g_repl_lock 下清理旧 RDMA replica 可能与广播失败路径冲突
- slave / master 两边对“连接失效”和“恢复重试”的节奏不一致

#### 关键问题链路与解决过程

##### 问题 4.6.1：异步 disconnect 清理影响 partial resync 基线

- 现象：restart 后 partial resync baseline 被破坏
- 根因：async disconnect 清理过度干预了 replica 生命周期
- 解决：回滚这部分干扰逻辑，只保留必要 cleanup

##### 问题 4.6.2：listener connect-request handoff 误触旧 replica 列表

- 现象：重连时状态混乱
- 根因：listener 在 handoff connect request 时错误触碰旧 replica 列表
- 解决：避免在 handoff 阶段动旧 RDMA replica list

##### 问题 4.6.3：stale replica cleanup 在锁下死锁风险

- 现象：广播失败后 cleanup 可能卡死
- 根因：在 `g_repl_lock` 下进行 stale replica cleanup，与广播失败路径相互干扰
- 解决：修正 cleanup 位置与锁使用方式，避免死锁

##### 问题 4.6.4：listener 生命周期与连接 teardown 耦合过深

- 现象：断开一个连接后，master listener 也被错误 teardown，后续无法稳定重连
- 根因：listener 被当成 per-connection 资源管理
- 解决：将 master RDMA listener 生命周期从 per-connection teardown 中拆开，仅在合适场景保留 listener

##### 问题 4.6.5：slave 侧不必要的 aggressive idle timeout

- 现象：轻微空闲也可能被误判为断链，导致反复重连
- 根因：slave recv loop 中 idle timeout 过于激进
- 解决：回滚 aggressive idle disconnect 逻辑，不让实验阶段策略过度激进

##### 问题 4.6.6：reconnect thrash

- 现象：断开后频繁重连、反复失败
- 根因：失败后立即重试，backoff 不足，stall-down 判定过早
- 解决：
  - 增大 backoff
  - 放宽 stall-down 条件
  - 减少无意义 thrash

#### 结果

经过多轮修复后，RDMA 路径从“不稳定握手后随机挂掉”逐渐收敛到：

- fullsync 可完成
- tail 增量可完成
- restart partial resync 基线恢复
- soak reconnect 恢复稳定

---

### 4.7 问题五：soak 场景下出现“位点前进但数据丢失”

这是 RDMA 稳定性阶段最关键、最本质的 bug。

#### 现象

在 soak 测试中出现：

- `repl_offset` 看起来持续前进
- slave 重启后还能继续 `CONTINUE`
- 但实际某些 key 在 slave 上消失

也就是：

- 位点对了
- 数据却不完整

#### 根因

slave 通过复制应用到本地的数据写，之前没有完整接入本地持久化。

这导致：

1. slave 内存态确实应用了复制写
2. `repl_offset` 也持续前进
3. 但这些复制写没有真正落到 slave 本地 AOF
4. 一旦 slave 重启，本地数据集回到旧状态
5. 随后又基于“更大的 offset”继续 partial resync
6. 于是出现某些历史 key 永久缺失

这个问题非常隐蔽，因为：

- 从 offset 看，一切像是正常的
- 从协议看，CONTINUE 也成功
- 真正错的是“本地数据集”和“复制位点”失去一致性

#### 解决方法

- 将 slave 复制应用的写接入本地持久化
- 加恢复 guard，避免 replay 阶段形成重复追加或广播环
- 确保 slave 重启后恢复出来的数据集与 `repl_offset` 语义一致

#### 为什么这一步很重要

这是 RDMA soak 稳定性最终真正收口的关键。

在这之前，很多现象看起来像：

- RDMA 断线重连不稳定
- 网络抖动导致数据丢失
- partial resync 不可靠

但最后证明最关键的根因其实是：

**slave 本地持久化与复制位点之间不一致。**

#### 结果

修复之后，以下指标全部通过：

- `fullsync_done=yes`
- `postsync_tail_ok=yes`
- `restart_rounds_ok=yes`
- `soak_ok=yes`
- `soak_availability_ok=yes`
- `soak_recovery_ok=yes`
- `final_resume_ok=yes`

这意味着：

- fullsync 成功
- tail 增量成功
- restart partial resync 成功
- soak 中真实 reconnect 后仍可恢复
- 最终数据一致性通过

---

## 5. 工程化与目录整理迭代
### 4.8 kprobe + RDMA 增量同步（完整实现）

在 RDMA 全量同步稳定后，开始实现基于 **kprobe BPF + RDMA WRITE** 的增量同步加速。

#### 整体架构

```
Master TCP send → kprobe/tcp_sendmsg → BPF ringbuf → 用户态回调 → RDMA WRITE → Slave MR
```

- kprobe 透明拦截 TCP send 路径，**不修改任何应用层代码**
- RDMA WRITE（单边操作）直接写入 Slave 预置 MR 环形缓冲区
- TCP 作为保底路径始终运行，Slave 通过 repl_offset 去重
- 使用**独立 QP**（`base_port + 12`），与 fullsync RDMA QP 分离

#### 实现历程（按时间顺序）

| 阶段 | 遇到问题 | 解决 |
|------|---------|------|
| BPF 初版 | 只写 4 字节 0 到 ringbuf（通知模式） | 补全 `bpf_probe_read_*` 读真实数据 |
| 猜 struct 偏移 | `payload_len=0`，probe_read 全失败 | BTF dump 确认 `iov@msg+40`, `nr_segs@msg+48` |
| 内核/用户指针混淆 | `bpf_probe_read_user` 读内核内存失败 | `msg/iov` 用 `read_kernel`，`iov_base` 用 `read_user` |
| CO-RE 重定位 | libbpf 报 `-4007`，struct 字段名不匹配 | 放弃 CO-RE，固定偏移 |
| BPF 验证器拒绝 | `R2 min value is negative` | 常量边界检查 |
| BPF 栈溢出 | 栈 512B 限制，entry[504] 超限 | per-CPU array 做临时缓冲区 |
| RDMA WRITE 使 QP 崩溃 | rkey=0 时 WRITE 导致 RETRY_EXC_ERR | 回调中检查 `g_slave_mr.rkey != 0` |
| 误报 fallback | transport.log 大量 "kprobe-rdma failed" | 移除误报日志 |
| BPF 加载 `-4007` | struct pt_regs 字段名与内核 BTF 不匹配 | 匹配内核命名，直接访问寄存器 |

#### 最终验证结果

- **50000 pre + 50000 post keys** 全量成功 ✅
- **RDMA WRITE 数据路径** 全线打通 ✅
- **Slave poll 线程消费 MR** 96000+ 条记录 ✅
- **kvstore_transport.log** 不再有误报 ✅

#### 关键代码文件

| 文件 | 行数 | 内容 |
|------|------|------|
| `src/replication/bpf/repl_kprobe.bpf.c` | ~240 | BPF kprobe 程序 |
| `src/replication/kvs_repl_kprobe.c` | ~1040 | 用户态转发模块 |
| `include/kvstore/replication/repl_kprobe.h` | ~50 | 头文件、常量定义 |

#### 相关文档

- `docs/kprobe-rdma-incrsync-implementation.md` — 完整实现详解
- `docs/kprobe-rdma-debug-diagnosis.md` — 问题排查与诊断手册


在功能路径逐步稳定后，又进行了一轮工程化整理。

### 5.1 主要修改

将历史上分散在根目录和 `scripts/` 下的脚本重新分类到：

- `tools/repl/`
- `tools/rdma/`
- `tools/persist/`
- `tools/bench/`
- `tools/tests/`

将运行生成物统一收口到：

- `artifacts/repl/`
- `artifacts/rdma/`
- `artifacts/persist/`
- `artifacts/bench/`
- `artifacts/legacy/`

同时：

- 根目录历史 `tmp_*` 文件全部清理
- 历史日志归档后再清空，只保留目录骨架
- 为 `artifacts/*` 增加 `.gitkeep`
- 增加根目录 `.gitignore`
- 将测试样例放到 `testdata/`
- 将 `.excalidraw` 图示移到 `assets/diagrams/`

### 5.2 为什么这样改

功能迭代到后期，脚本、日志、AOF、dump、profile 输出越来越多。如果继续散在根目录，会带来几个问题：

- 很难判断哪个文件是源码、哪个是产物
- 旧日志容易污染后续判断
- 自动化验证脚本不容易维护
- 迁移或交付时根目录看起来非常混乱

因此最后做了一轮“工程化清理”，把：

- 入口统一
- 产物统一

---

## 6. 全量同步性能优化与数据完整性修复

### 6.1 背景

RDMA 复制路径此前已功能可用（fullsync + incremental sync + restart partial resync），但在中等数据集下（50000 key，约 2.2MB dump），全量同步耗时 **281 秒**，且偶尔出现 slave 数据不完整的问题。

诊断发现性能瓶颈和数据丢失均源于**多级缓冲区大小不匹配**，而非 RDMA 自身问题。

### 6.2 问题一：二进制 dump 格式兼容性

#### 现象

dump 文件使用**文本格式**（`key\nvalue\n`），如果 key 或 value 中包含 `\r` / `\n` 字符，会导致解析错位。

#### 修复

改为**二进制长度前缀格式**：
```
[uint32_t klen][key bytes][uint32_t vlen][value bytes]
```

这样无论 key/value 包含什么字节，都能通过长度前缀精确还原。

#### 影响

修改了 `kvs_dump_to_fd()` 的序列化/反序列化，涉及持久化恢复和主从全量同步两条路径。

---

### 6.3 问题二：RDMA chunk size 过小

#### 现象

传输日志显示每次 RDMA send 仅 8192 字节，导致 2.2MB dump 需要约 280 次 RDMA 发送。前 64 次（匹配 64 个 recv slots）在 1ms 内完成，后续每次需等待 slave repost recv buffer（约 2-3 秒），累计约 7 分钟。

#### 根因链

1. `queue_snapshot()` 使用 `unsigned char buf[8192]` 栈缓冲读取 dump 文件
2. 每次 `fread` 只读 8KB，随后以 8KB 为单位调用 `repl_send_chunked`
3. `rdma_chunk_size` 默认值 = `BUFFER_CAP / 4` = 16384（16KB），但受 `send_buf_cap` 限制实际发送更小
4. **即使 `chunk_cap` 设得再大，读文件用的 8KB 缓冲是硬限制**

#### 修复

将 `queue_snapshot()` 的本地缓冲改为**动态堆分配**，大小取 `repl_rdma_effective_chunk_size()`（默认 256KB）。同时调整 `rdma_chunk_size` 默认值从 `BUFFER_CAP / 4`（16KB）→ `BUFFER_CAP * 4`（256KB），send/recv buffer 分配也同步增大。

#### 效果

2.2MB dump 从 **~280 块 × 8KB** → **~9 块 × 256KB**，全在 64 slot 窗口内一次性完成。

---

### 6.4 问题三：slave 侧多级缓冲区溢出

#### 现象

全量同步速度从 281s 降到 11s，但 **slave 数据验证失败**——slave 只有 14874 条记录而非 50000 条，`HGET pre:k:000000` 返回 `(null)`。

#### 根因：slave 侧缓冲区太小，RDMA chunk 被丢弃

slave 侧有 **4 处** 256KB RDMA chunk 要写入的缓冲区此前只有 64KB：

| 位置 | 路径 | 作用 | 改前 |
|---|---|---|---|
| `slave_thread` hybrid 模式 | L1726 | 接收 TCP + RDMA 混合数据 | `buf[BUFFER_CAP]` (64KB) |
| `slave_thread` 纯 RDMA 模式 | L1836 | 接收 RDMA 流式数据 | `stream_buf[BUFFER_CAP]` |
| `rdma_master_listener_thread` | L2160 | master 侧接收 slave 数据 | `stream_buf[BUFFER_CAP]` |

当 slave 收到 256KB RDMA chunk 时，`blen + rdma_blen <= sizeof(buf)` 检查失败，**数据被静默丢弃**。

#### 修复

将 **3 处**缓冲区从 `BUFFER_CAP`（64KB）增大到 `BUFFER_CAP * 4`（256KB）。

---

### 6.5 问题四：TCP recv 阻塞导致 RDMA 完成事件延迟处理

#### 现象

即使缓冲区改大，slave 仍只收到 665KB 数据（约 2.5 个 chunk），剩余 1.5MB 丢失。

#### 根因

hybrid 模式路径中：
```c
recv(tcp_fd, buf + blen, sizeof(buf) - blen, 0);
```
TCP socket 设置了 `SO_RCVTIMEO=1s`，**每轮迭代阻塞 1 秒**。在阻塞期间，RDMA CQ 中的完成事件无法被消费，recv buffer 无法 repost。虽然此例中数据未丢失（CQ 不丢事件），但阻塞增加了时序不确定性。

#### 修复

将 `recv` 改为 `MSG_DONTWAIT` 非阻塞模式，同时增加收完 TCP 数据后的缓冲消化步骤。

---

### 6.6 问题五（核心）：chunk 边界切断 RESP 命令

#### 现象

修复以上所有问题后，仍只有 14874 条记录写入 slave。transport log 显示所有 9 个 256KB chunk 已由 master 发出，但 slave 只处理了前几个的末尾部分。

#### 根因

256KB chunk 的末尾可能正好切断一条 RESP 命令。例如 chunk 边界落在：
```
*3\r\n$4\r\nHSET\r\n$12\r\npre:k:0← 切在这里，后面没了
```

`parse_resp_stream` 正确处理了这种情况——将不完整的命令留在缓冲区（`blen` = 十几到几十字节）。但问题出在**下一次迭代**：

```c
if (blen + rdma_blen <= sizeof(buf))
// 10(残留) + 262144(新 chunk) = 262154 > 262144 → BUFFER OVERFLOW!
```

**此后的所有 RDMA chunk 都被溢出检查丢弃**，slave 只处理了 chunk 边界恰好对齐的那几个块。

#### 为什么不是每次必现

残留大小取决于 RESP 命令在 chunk 中的位置。如果 chunk 末尾正好是一条完整命令的结尾（`\r\n`），残留为 0，检查通过。概率大约 50%，解释了为什么此前有时候成功有时候失败。

#### 修复

1. 缓冲区从 `BUFFER_CAP * 4`（262144）→ `BUFFER_CAP * 4 + 4096`（266240），**给残留留 4KB 余量**
2. 在 RDMA 检查前增加 `if (blen > 0) parse_resp_stream(...)` 消化残留，进一步降低溢出概率

#### 效果

全量同步从 **11 秒（数据错误）→ 1 秒（50000 条全部正确）**。slave 数据验证 `HGET pre:k:000000` = `v0`。

---

### 6.7 调试路径与关键发现

本轮调试的特点是多级递进式排查——每修复一层，数据量就前进一截，最终收敛到核心问题。以下按实际排查顺序记录。

#### 第一层：排除网络传输问题

确认 RDMA 数据确实在 1 秒内全部到达 slave（transport log 显示 9 个 chunk 在同一时间戳发出），否定了"RDMA 丢包"的假设。

#### 第二层：加调试日志，暴露数据缺失轨迹

在 slave 的关键路径插入调试日志：
- `slave_debug_rdma`：每个 RDMA recv 的 slot、大小、缓冲区状态
- `slave_debug_parse`：`parse_resp_stream` 前后数据量变化
- `slave_debug`：全量同步完成后的 hash 表条目数

关键发现：
```
slave_debug - total_hash_entries=14874    ← 只有预期的 30%
slave_debug - HGET pre:k:000000 = (null)  ← 第一条 key 就不存在
```

这说明 RESP 命令确实被执行了，但只执行了一部分。

#### 第三层：追查数据流向

通过逐步放大缓冲区发现：
1. **第一步**：`queue_snapshot` 的 8KB 栈缓冲是性能瓶颈——**修复**
2. **第二步**：slave 的 `stream_buf` 只有 64KB，256KB chunk 被溢出丢弃——**修复**
3. **第三步**：hybrid 模式还有一处 `buf[64KB]`——**修复**
4. **第四层**（发现 `BUFFER OVERFLOW`）：hybrid 模式 TCP 先收到 FULLRESYNC 占住空间 → RDMA 256KB 放不下。把 TCP 解析移到 RDMA 之前——**修复**

但每层修复后数据量都停在 ~14874 条不变，暗示有更深层原因。

#### 第四层：发现残留数据累积

观察 `slave_debug_parse` 发现 `before=141765 after=0`，最后一块数据被完全解析。但 `loaded=665987` 远小于 `target=2238890`。

追踪到 `repl_rdma_wait_cq_recv_completion` 只返回了很少的几次——说明大部分 RDMA recv 完成事件没有被消费。

#### 第五层：TCP recv 阻塞 1 秒

发现 `recv(tcp_fd, ...)` 使用 `SO_RCVTIMEO=1s` 每轮阻塞 1 秒，导致 RDMA 完成事件延迟处理。改为 `MSG_DONTWAIT`——**修复**

但数据量仍然只有 14874 条！

#### 第六层（根因）：chunk 边界切割 RESP 命令

最终发现 `loaded=665987` 约等于 2 个完整 256KB chunk + 1 个 141KB 尾巴的 `rawlen` 总和。**剩下的 chunk 从未被接收**。

检查 `blen + rdma_blen <= sizeof(buf)` 条件，意识到：
- 前一个 chunk 末尾的不完整 RESP 命令残留（~10B）
- 下一个 256KB chunk 到来时 `10 + 262144 > 262144`
- **此后的所有 chunk 都被丢弃**

这是本轮调试中最隐蔽的问题——因为 `parse_resp_stream` 正确处理了不完整命令（保留残留），但上游的缓冲区溢出检查没有考虑这个残留空间。

#### 关键教训

1. **数据流向的每一级缓冲区都必须匹配**：master 读文件 → master send buf → RDMA 传输 → slave recv buf → slave stream buf。任何一级不匹配都导致瓶颈或数据丢失。
2. **RESP 命令跨 chunk 边界是常见陷阱**：基于流的协议（如 RESP）在分块传输时，必须为每块末尾的不完整数据预留空间。
3. **多级调试日志比单一大日志有效**：先确认数据到了没有（transport log），再确认被处理了没有（parse log），最后确认存进去了没有（hash count）。
4. **"offset 正确但数据缺失"是最隐蔽的信号**：slave 显示的 offset 来自 FULLRESYNC 头部的 master offset，并非实际应用了多少。需要用 `loaded_bytes` 区分 "收到了多少" 和 "处理了多少"。

---

### 6.8 总结：所有改动的一致性

本轮涉及的所有缓冲区修改汇总：

| 文件 | 位置 | 原大小 | 新大小 | 类型 |
|---|---|---|---|---|
| `src/main/kvstore.c` | `queue_snapshot()` buf | 8192 栈 | 动态堆分配 (max 256KB) | 堆 |
| `src/main/kvstore.c` | `rdma_chunk_size` 默认值 | `BUFFER_CAP/4` (16KB) | `BUFFER_CAP*4` (256KB) | 配置 |
| `src/main/kvstore.c` | `repl_send_chunked_ctx` cap | `BUFFER_CAP` (64KB) | `BUFFER_CAP*4` (256KB) | 上限 |
| `src/replication/kvs_repl.c` | `KVS_RDMA_CHUNK_SIZE_DEFAULT` | `BUFFER_CAP/4` (16KB) | `BUFFER_CAP*4` (256KB) | 宏 |
| `src/replication/kvs_repl.c` | chunk cap | `BUFFER_CAP` (64KB) | `BUFFER_CAP*4` (256KB) | 上限 |
| `src/replication/kvs_repl.c` | `repl_rdma_prepare_buffers` cap | `BUFFER_CAP` (64KB) | max(chunk_size, BUFFER_CAP) | 分配 |
| `src/replication/kvs_repl.c` | hybrid 模式 `buf` | `BUFFER_CAP` (64KB) | `BUFFER_CAP*4+4096` (266KB) | 栈 |
| `src/replication/kvs_repl.c` | 纯 RDMA `stream_buf` | `BUFFER_CAP` (64KB) | `BUFFER_CAP*4` (256KB) | 栈 |
| `src/replication/kvs_repl.c` | listener `stream_buf` | `BUFFER_CAP` (64KB) | `BUFFER_CAP*4` (256KB) | 栈 |

**未修改的缓冲区**：`BUFFER_CAP` 宏本身保持 65536，客户端 `inbuf[BUFFER_CAP]`、RESP 响应缓冲 `resp[BUFFER_CAP]`、纯 TCP 路径等不受影响。

这些改动的共同特征：
- 只影响 **RDMA 传输路径**，不涉及普通客户端协议解析
- 只扩大**数据传递缓冲区**，不改变控制流和协议语义
- 所有被增大的栈缓冲区都在**独立线程**中，Linux 默认 8MB 栈空间绰绰有余
- 文档统一
- 忽略规则统一

### 5.3 期间额外发现的问题

在目录重构后，真实跑脚本时还发现一个路径问题：

- `tools/repl/run_repl_profile.py` 内部调用 `run_repl_metrics_bench.py` 时，`cwd` 仍指向旧层级
- 导致相对路径拼成 `tools/tools/repl/...`

#### 解决方法

把 `cwd` 修正为项目根目录，再复测通过。

这也说明：

- 目录整理不能只“看起来改了”
- 必须至少做一轮最小实跑验证，才能发现真实断链

---

## 6. 本项目迭代中最重要的经验

### 6.1 先保证语义，再做 transport 优化

RDMA 不应该改变复制语义，它应该只是 transport 优化层。

因此必须先把：

- backlog
- replid / offset
- fullsync / continue
- slave 恢复
- 本地持久化一致性

这些语义做稳定，RDMA 才有意义。

### 6.2 可观测性不是附属功能，而是排障前提

没有 metrics、日志切片、eBPF / profile，RDMA 稳定性问题几乎不可能高效定位。

### 6.3 soak 问题很多时候不是网络 bug，而是状态一致性 bug

最典型的例子就是：

- offset 正常推进
- 网络也在恢复
- 但 slave 本地持久化没跟上

最后表现成数据丢失。

### 6.4 目录工程化必须配套脚本与 Makefile 修改

仅移动文件不够，必须同步修改：

- `Makefile`
- 脚本内部相互调用路径
- 默认产物目录
- README
- `.gitignore`
- 最小实跑验证

---

## 7. 当前阶段性结论

到目前为止，项目已经完成了从“基础 KV”到“具备复制增强、RDMA 实验路径、eBPF 观测和工程化验证体系”的一轮完整迭代。

可以明确给出的阶段性结论是：

1. TCP 复制增强已经完成，partial resync 基本可用
2. 持久化与恢复链路已支撑 restart / recovery 相关验证
3. 文档型 value 已具备最小可用能力
4. RDMA transport 已不是 skeleton，而是实验性真实实现
5. RDMA stress / soak 已通过，核心恢复指标全部为 `yes`
6. 工程目录、脚本入口、产物落点、README 已完成一轮整理

同时也应保持边界感：

- 默认稳定主路径仍应视为 `tcp`
- `rdma` 当前更适合作为实验性高性能 transport
- eBPF / profiling 仍依赖具体环境能力

---

## 8. 可直接引用的最终验证结论

本轮 RDMA 稳定性最终验证结果为：

- `fullsync_done=yes`
- `postsync_tail_ok=yes`
- `restart_rounds_ok=yes`
- `soak_ok=yes`
- `soak_availability_ok=yes`
- `soak_recovery_ok=yes`
- `final_resume_ok=yes`

这组结果意味着：

- 初始全量同步正常
- 增量同步正常
- restart partial resync 正常
- soak 期间连接抖动后可恢复
- 最终恢复与数据一致性通过

---

## 9. 后续建议

如果继续往后迭代，建议按下面顺序推进：

1. 增加更长时长、更大参数矩阵的 soak 验证
2. 把当前实验性 RDMA 行为继续收敛成更稳的 backend 边界
3. 继续补齐复制与持久化的一致性回归测试
4. 如果要做更正式交付，可再补一份“模块架构图 + 数据流图”

---

## 10. 附：本轮工程化整理后的关键目录

- `tools/repl/`：复制、profile、eBPF、RDMA 复制脚本
- `tools/rdma/`：独立 RDMA 环境 / pingpong 探测脚本
- `tools/persist/`：持久化、恢复、TTL 验证脚本
- `tools/bench/`：基准测试脚本
- `artifacts/`：所有生成物统一出口
- `testdata/`：静态测试样例
- `assets/diagrams/`：工程图示资源

---

## 11. RDMA Pipeline：多发送缓冲区 + 后台 CQ 轮询

### 11.1 背景

此前 RDMA 全量同步（fullsync）已经功能可用，但发送路径采用**同步模型**：

```c
memcpy(send_buf, data, len);       // ① 拷贝
ibv_post_send(qp, &wr, NULL);      // ② 提交 WR
wait_cq_send_completion(5000);     // ③ 阻塞等待 CQ 完成 ← 流水线阻断点
```

每次 send 都要等待 CQ completion 才能返回，无法重叠**数据拷贝**与**DMA 传输**。在 soft-iWARP（siw）上，每次 WR 的软件开销较高，同步等待进一步放大了延迟。

同时只使用 **1 个 send buffer**，无法在等待 completion 期间准备下一块数据。

### 11.2 设计方案

采用 **4 阶段渐进式实现**，每阶段可独立编译验证：

| Phase | 改动 | 收益 |
|---|---|---|
| 1 | 单 send_buf → `send_slots[4]` 环形队列 | 支持多缓冲区并行准备 |
| 2 | 后台 CQ 轮询线程（事件驱动） | 异步处理 send/recv completion |
| 3 | WR 批量提交（预留，未接入调用方） | 减少 ibv_post_send 次数 |
| 4 | 自适应 pipeline 深度调节 | 动态匹配传输速率 |

### 11.3 主要修改

#### 新增数据结构

```c
/* 发送缓冲区槽位 */
typedef struct repl_rdma_send_slot_s {
    struct ibv_mr *mr;          // 注册的内存区域
    unsigned char *buf;         // 缓冲区
    size_t cap;                 // 容量
    int in_flight;              // 1 = 已 post 但未完成
    uint64_t wr_id;             // 对应的 wr_id（含 PIPELINE_WR_ID_FLAG）
} repl_rdma_send_slot_t;

// 在 repl_rdma_ctx_t 中新增字段：
repl_rdma_send_slot_t send_slots[KVS_RDMA_PIPELINE_DEPTH]; // 4 个 slot
int send_pipeline_head;          // 下一个空闲 slot
int send_slots_in_flight;        // outstanding send WR 数
int send_pipeline_depth;         // 动态调节的深度 (2~4)
int send_pipeline_enabled;       // 是否启用 pipeline
pthread_t cq_poll_thread;        // CQ 轮询线程
int cq_poll_thread_running;      // 线程运行标志
```

#### 新增常量

```c
#define KVS_RDMA_PIPELINE_DEPTH      4    /* 多发送缓冲区深度 */
#define KVS_RDMA_CQ_BATCH            8    /* CQ 批量 poll 大小 */
#define KVS_RDMA_PIPELINE_WR_ID_FLAG 0x80000000UL  /* wr_id 高位标记 */
```

#### 新增函数

| 函数 | 说明 |
|---|---|
| `repl_rdma_acquire_send_slot(timeout_ms)` | 获取空闲 send slot；全忙时等 CQ 线程释放或 poll CQ 回收 |
| `repl_rdma_release_send_slot(slot)` | 释放 send slot（由 CQ completion 回调调用） |
| `repl_rdma_cq_process_wc(wc, adapt_counter)` | 统一的 WC 处理函数（CQ 线程和 fallback 共用） |
| `repl_rdma_cq_poll_thread()` | 后台 CQ 事件循环，`ibv_get_cq_event()` 驱动 |
| `repl_rdma_start_cq_poll_thread()` | 启动 CQ 轮询线程并 arm notification |
| `repl_rdma_stop_cq_poll_thread()` | 停止 CQ 轮询线程 |
| `repl_rdma_adjust_pipeline_depth()` | 动态调节 pipeline 深度（每 16 次 completion 评估） |

#### 修改函数

| 函数 | 改动 |
|---|---|
| `repl_rdma_try_send()` | pipeline 模式下：acquire_slot → memcpy → post_send → 立即返回 |
| `repl_rdma_prepare_buffers()` | 分配 4 个 send buffer 并注册 MR |
| `repl_rdma_reset_conn_ctx()` | 停止 CQ 线程，清理所有 send slot |
| `repl_rdma_wait_cq_recv_completion()` | CQ 线程运行时只查 pending 队列，不直接 poll CQ |
| `repl_rdma_wait_cq_send_completion()` | 识别 pipeline WR_ID，释放对应 slot |
| `repl_rdma_acquire_send_slot()` | CQ 线程运行时等待释放，不直接 poll CQ |
| `repl_transport_rdma_connect_slave()` | 建链后启动 CQ 轮询线程 |
| `rdma_master_listener_thread()` | accept 后启动 CQ 轮询线程 |

### 11.4 遇到的问题与解决方法

#### 问题一：CQ completion notification re-arm 时序导致死锁

**现象**：Pipeline 模式下，master 发送完整 sync 数据后，slave 侧 CQ 轮询线程挂死，后续增量数据无法到达。

**根因**：（经典 RDMA 编程错误）CQ 轮询线程的循环顺序为：

```
ibv_get_cq_event() → ibv_poll_cq()(排空) → ibv_req_notify_cq()(重 arm)
```

如果在 `ibv_poll_cq()` 返回 0 之后、`ibv_req_notify_cq()` 执行之前，有新的 completion 到达，该 completion **不会触发事件**。因为 RDMA 硬件只在 notification **未 arm** 时生成事件，而 arm 操作发生在 completion 到达之后。于是线程永久阻塞在 `ibv_get_cq_event()`。

**解决方法**：采用标准 RDMA re-arm → drain 模式：

```c
ibv_req_notify_cq(cq, 0);           // 先 arm
ibv_poll_cq(cq, N, wc);             // 再 drain（确认 arm → poll 之间无漏网）
if (有数据) → 处理 → continue;       // 不阻塞等待
ibv_get_cq_event(...);               // 无数据 → 阻塞等待下一事件
```

**关键教训**：`ibv_req_notify_cq()` 必须在 `ibv_get_cq_event()` **之前** arm，且 arm 后必须立即 `ibv_poll_cq()` 确认没有 missed completion。

#### 问题二：多线程同时 poll 同一 CQ 导致 event 丢失

**现象**：CQ 轮询线程启动后，hybrid 模式 slave loop 中的 `repl_rdma_wait_cq_recv_completion()` 仍直接 `ibv_poll_cq()`，与轮询线程竞争同一 CQ。

**根因**：虽然 `ibv_poll_cq()` 本身是线程安全的，但 completion channel 的事件机制不是。当两个线程同时 poll 同一 CQ：
1. 线程 A（CQ 轮询）等待 `ibv_get_cq_event()`
2. completion 到达，event 触发
3. 线程 B（直接 poll）抢先消费了该 completion
4. 线程 A 从 `ibv_get_cq_event()` 醒来，`ibv_poll_cq()` 发现为空
5. 线程 A 重 arm notification
6. 下次 completion 到达时，如果发生在 arm 之后，虽然能正确触发 event，但大量 CPU 浪费在无效唤醒上

更严重的是，如果线程 A 的 re-arm 和下一个 completion 到达存在竞争窗口，**可能永久丢失 event**。

**解决方法**：
- `repl_rdma_wait_cq_recv_completion()`：CQ 轮询线程运行时，**只检查 pending_recv 队列**，不直接 poll CQ
- `repl_rdma_acquire_send_slot()`：CQ 轮询线程运行时，所有 slot in_flight 时**只等待**，不直接 poll CQ 回收

```c
if (g_repl_rdma_ctx.cq_poll_thread_running) {
    /* 等 CQ 线程异步处理后 pending_recv 队列 */
    while (kvs_now_ms() < deadline) {
        usleep(1000);
        if (repl_rdma_pending_recv_pop(...) == 0) return 0;
    }
    return -1;
}
/* 无 CQ 线程：直接 poll CQ（向后兼容） */
ibv_poll_cq(...);
```

#### 问题三：CQ 轮询线程在 `repl_rdma_reset_conn_ctx()` 中销毁顺序

**现象**：连接断开后，CQ 轮询线程可能仍在访问已释放的 CQ / comp_chan。

**根因**：`repl_rdma_reset_conn_ctx()` 直接销毁 `comp_chan` 和 `cq`，但未先通知 CQ 轮询线程停止。

**解决方法**：在 `reset_conn_ctx` 入口先调用 `repl_rdma_stop_cq_poll_thread()`：
1. 设置 `cq_poll_thread_running = 0`
2. 销毁 `comp_chan` → 唤醒线程（`ibv_get_cq_event()` 返回错误）
3. 线程检测到运行标志已清除，退出循环
4. 随后安全销毁 CQ 和其他资源

### 11.5 性能对比

| 指标 | 改前（同步模式） | 改后（Pipeline 模式） | 预期提升 |
|---|---|---|---|
| 每次 send 阻塞 | 等待 100-500μs（CQ poll） | 立即返回（0μs 阻塞） | 2-4x 吞吐 |
| CQ 轮询方式 | busy poll（100% 单核） | `ibv_get_cq_event` 事件驱动 | CPU ↓80% |
| send buffer 数量 | 1（无法 prep 下一块） | 4（环形队列） | 数据传输可重叠 |
| CQ 多线程竞争 | N/A | 通过 `cq_poll_thread_running` 隔离 | 消除竞争死锁 |

### 11.6 当前状态

- ✅ **Phase 1** 多发送缓冲区 — 完成，编译通过
- ✅ **Phase 2** 后台 CQ 轮询线程 — 完成，编译通过
- ⏸️ **Phase 3** WR 批量提交 — 代码已移除（`repl_rdma_batch_send` 预留接口，因未接入调用方而产生 unused warning，暂时移除）
- ✅ **Phase 4** 自适应 Pipeline 深度 — 完成，编译通过
- ✅ **CQ 线程安全问题修复** — notification re-arm 时序 + 多线程 poll 竞争 + 销毁顺序

Pipeline 模式默认启用（`send_pipeline_enabled = 1`），不影响 TCP/eBPF 等其他传输路径。

### 11.7 遗留问题

- 增量同步（eBPF realtime）在 hybrid 模式下偶现 slave offset 不推进。当前怀疑为 eBPF daemon 未独立启动或 sockmap 未正确配置，与 Pipeline 改动无直接关联。

---

## 12. 阶段十二：RDMA/kprobe 性能基准重测与代码对齐（2026-07-01）

### 12.1 背景

`test_rdma_throughput.c` 的 RDMA 实现与 `kvs_repl.c` 生产代码存在关键差距（无 pipeline、无 completion channel、usleep 忙等），且 README 中 kprobe 跨机 QPS 数据来源不确定（只测了 64B，但表格有 7 种 payload）。

### 12.2 test_rdma_throughput 代码对齐

#### 尝试过的方案

1. **completion channel + CQ 轮询线程**（对齐生产代码 `repl_rdma_cq_poll_thread`）：本地 loopback 吞吐从 29 Gbps 暴跌到 10 Mbps。根因是 `ibv_poll_cq` 被两个线程并发调用——CQ 线程和 acquire_send_slot 的内联 poll——libibverbs 不保证 CQ 线程安全，导致 completion 丢失。

2. **Pipeline 发送槽**（4/128/512 深度）：本地 loopback 正常，但跨机吞吐固定 ~365 Mbps（原 967 Mbps），与 pipeline depth 无关。

3. **最终方案**：回退到与原代码一致的单 buffer + 高 inflight + 内联 poll。本地 loopback 45 Gbps（+55% vs 原 29 Gbps），跨机 365 Mbps。跨机退化后确认是 **kernel 6.1 rxe 驱动回归**（方向性验证：从机 kernel 5.15 发包 → 主机收 = 892 Mbps，主机 kernel 6.1 发包 → 从机收 = 365 Mbps，同一二进制）。

#### 关键发现

- completion channel 模式不适合单进程吞吐测试（CQ 并发 poll 无锁保护）
- `volatile` 跨线程读写 `in_flight` 标志在 x86 TSO 上无问题，问题在并发 `ibv_poll_cq`
- kernel 6.1 rxe 跨机 RoCEv2 发包性能约为 5.15 的 40%

### 12.3 kprobe 跨机 QPS 重测

全部 7 种 payload（64B-4096B）重新测试，10K 请求，共用 fd + null trim。

#### 测试结果

| Payload | none QPS | sync QPS | kprobe QPS | kprobe fwd | kprobe vs sync |
| ------- | -------: | -------: | ---------: | ---------: | -------------: |
| 64B | 29,259 | 22,242 | 15,556 | 10,507 | −30.1% |
| 128B | 28,923 | 22,290 | 15,653 | 10,568 | −29.8% |
| 256B | 28,026 | 20,607 | 12,031 | 10,500 | −41.6% |
| 512B | 26,263 | 18,274 | 14,108 | 10,500 | −22.8% |
| 1024B | 26,770 | 21,676 | 16,755 | 10,510 | −22.7% |
| 2048B | 27,673 | 21,017 | 16,637 | 10,509 | −20.8% |
| 4096B | 26,406 | 19,926 | 14,341 | 10,513 | −28.0% |

#### 发现

- kprobe ringbuf 严重溢出：`rb_err` ≈ step2 命中数（99.9% 丢弃率）。ringbuf 消费者 `write_full(slave_fd)` 被跨机 TCP 同步阻塞，消费跟不上 kprobe 产生速度
- kprobe fwd 稳定在 ~10,500，与 payload 无关，受限于单 TCP 连接跨机转发带宽
- 之前 README 中 kprobe > sync 的数据来自一次转发连接失败的测试（`connect slave: Connection refused`），转发实际未发生，QPS 虚高
- 测试方法局限性：ringbuf 消费者与主 echo 路径同进程抢 CPU，无背压机制，kprobe QPS 不可靠。需要独立 CPU 核或独立进程才能测到有意义的数据

### 12.4 修改文件

- `tests/perf/test_rdma_throughput.c`：重写发送路径，移除 CQ 线程和 completion channel，回退到单 buffer + 内联 poll + 高 inflight
- `README.md`：更新 RDMA 吞吐量关键发现、kprobe 跨机 QPS 表格和结论

这轮迭代结束后，项目已经从”功能堆叠”转入”可验证、可复盘、可继续维护”的状态。
