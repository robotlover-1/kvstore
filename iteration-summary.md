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

这一部分是本项目后期最复杂、迭代次数最多的工作。

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

这轮迭代结束后，项目已经从“功能堆叠”转入“可验证、可复盘、可继续维护”的状态。
