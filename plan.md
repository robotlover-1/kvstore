一、我从你的需求里提炼出的目标
你这次更新的核心，不是单纯补几个功能，而是把当前 kvstore 从“基础单机 KV”逐步升级为：

可压测、可验证的数据存储服务

能用 testcase 做功能验证
能统计/观察性能数据
README 要补齐编译、测试、性能说明
具备持久化能力

全量持久化
增量持久化
重启恢复验证
持久化文件和内存数据一致性验证
具备更完整的数据类型能力

key-value 扩展为类似文档型 / JSON 风格对象存储
增量持久化时支持对象级更新
具备主从复制能力

单主单从同步
testcase 可验证主从一致
区分日志模式 / 主从模式
具备更高性能的数据同步方案

部分数据走 rdma
实时同步走 ebpf
支持 key 超时能力

TTL / 过期淘汰
二、建议的总体实施策略
你的需求跨度很大，我建议不要“并行硬上”，而是按下面原则推进：

先做基础工程能力，再做功能增强
先做单机正确性，再做分布式同步
先做全量持久化，再做增量持久化
先做传统可用主从，再考虑 RDMA / eBPF 优化
每一步都配 testcase 和 README 更新
也就是说，路线应该是：

工程整理 → 持久化基础 → 数据模型增强 → 主从复制 → 高性能同步 → TTL 收尾

三、逐步更新实现计划
阶段 0：现状梳理与基线建立
目标
先搞清楚项目当前已经实现了什么、缺什么、哪些模块需要重构，避免后面返工。

任务
梳理现有代码结构

src/main/kvstore.c 当前承担了哪些职责
是否存在：
网络监听
命令解析
内存 KV 存储
文件 IO
testcase 对接接口
确认当前支持的命令

set/get/del
是否已有批量插入 / 保存 / 加载
是否已有 socket 通信协议
建立基线测试

启动 kvstore
用 testcase 跑最基础读写
记录当前通过情况
README 补一个“当前能力说明”小节

当前已支持功能
当前未支持功能
编译与运行方法
输出物
当前模块清单
基础命令列表
基线测试结果
README 初版更新
建议
如果现在 kvstore.c 是单文件 1000+ 行，后面最好逐步拆分，否则新增持久化、复制、TTL 后复杂度会很快失控。

阶段 1：重构核心架构，给后续功能留扩展点
目标
把当前项目整理成可扩展架构，为持久化、主从、TTL 做准备。

任务
抽离核心模块 建议至少拆成这些逻辑层：

store：内存数据管理
protocol：请求解析与响应编码
server：网络服务循环
persistence：持久化读写
replication：主从同步
expire：TTL 管理
统一数据结构 当前如果只是简单字符串 KV，建议至少抽象成：

key
value_type
value_payload
version / seq
expire_at
dirty 标记
引入操作日志抽象 为后续增量持久化和主从复制准备：

每次写操作生成统一 op_record
类型如 SET / DEL / UPDATE_FIELD / EXPIRE
统一错误码和日志输出 后面调试主从、恢复、增量落盘都很需要。

输出物
模块边界清晰的代码结构
统一对象模型
可复用操作日志结构
风险
如果这一层不做，后续“增量持久化”和“主从复制”会分别写一套逻辑，最终很难维护。

阶段 2：实现全量持久化
目标
先实现最稳定、最容易验证的全量快照持久化。

任务
定义全量快照文件格式 建议文件里包含：

文件头 / magic
版本号
记录数
每条记录的 key / type / value / expire_at
实现 save

遍历内存数据
序列化到持久化文件
先写临时文件，再 rename，避免文件损坏
实现启动时 load

服务启动时读取快照
恢复全部 key
对已过期数据直接跳过
testcase 验证 按你需求中的流程：

启动 kvstore
testcase 插入 10w 条
发送 save
停机
重启 kvstore
自动 load
testcase 读取 10w 条校验一致
README 补充

持久化文件位置
save/load 使用方法
数据一致性验证方法
输出物
save/load 功能
全量恢复 testcase
持久化文件格式文档
建议
这个阶段先不要上 mmap，先用普通文件 IO 做通，确保格式和恢复逻辑稳定。

阶段 3：实现增量持久化
目标
在全量快照基础上，增加写操作日志，实现“重放恢复”。

任务
定义增量日志格式 每条日志记录：

op_type
key
value / field
timestamp
seq_id
写操作自动追加日志

set/del/update 等操作后 append log
必要时 flush / fsync 策略可配置
恢复策略 启动时：

先加载全量快照
再按顺序重放增量日志
设计日志截断策略 当全量快照生成后：

老增量日志可归档或清空
避免日志无限增长
按你需求验证

增量持久化 kvstore
testcase 插入 10w 条
停机
重启 kvstore
自动 load 增量持久化文件
testcase 读取 10w 条校验一致
输出物
append-only 增量日志
快照 + 日志恢复机制
增量恢复 testcase
建议
你需求里提到“加载持久化数据用 mmap 实现”，这个可以放在这个阶段后半段做优化：

先保证格式正确
再把读取或映射切换成 mmap
阶段 4：扩展 value 模型，支持“文档型”数据
目标
从简单 key -> string 升级到“key-value + 文档型对象”。

任务
定义 value 类型系统 建议至少支持：

string
integer
document/json-like object
设计文档对象内部结构 可先支持最小能力：

set key field value
get key field
del key field
统一序列化与反序列化 文档对象需要支持：

全量快照序列化
增量日志序列化
协议层扩展 testcase 或客户端命令要能表达：

创建对象
修改字段
读取字段
一致性验证

普通 KV 与文档对象都能持久化
重启后结构不丢失
输出物
多类型 value 数据结构
文档型操作接口
对应持久化支持
建议
“类 redis 的 key-value + 类 mongodb 的 document” 这个目标很大，第一版千万不要追求完整 JSON 能力。建议只支持：

平铺字段
string/integer 字段值 这样后续主从和持久化实现会容易很多。
阶段 5：区分日志模式与主从模式
目标
把“本地增量日志”和“远程复制日志”从架构上区分清楚。

任务
抽象两种模式

日志模式：用于本地恢复
主从模式：用于远程节点同步
操作分发机制 一次写请求到达主节点后：

更新内存
写本地持久化日志
发送复制事件给 slave
复制消息格式 和本地增量日志可以共用大部分结构，但要补充：

source node
replication offset / seq
ack 机制
配置项设计

节点角色：master / slave
主节点地址
是否开启持久化
是否开启复制
输出物
角色配置
本地日志与远程复制分离
可扩展复制协议
建议
这一阶段先不要碰 RDMA / eBPF，先用 TCP 把主从同步链路打通。

阶段 6：实现基础主从复制
目标
完成你需求里“先启动 master，插入数据，再启动 slave，然后 master 再插入，slave 能读到一致数据”的能力。

任务
slave 启动注册到 master

建立连接
报告自身状态
请求初始同步
实现全量同步 slave 第一次接入时：

master 发送当前全量快照
slave 装载快照
实现增量同步 全量同步完成后：

master 将新写入操作持续推送给 slave
slave 顺序回放
一致性验证 testcase 按你的要求设计：

master 启动
testcase 插入 5w 条
slave 启动
slave 完成初始同步
master 再插入 5w 条
testcase 到 slave 读取 10w 条，验证一致
异常恢复机制 最基础先支持：

slave 断连重连后重新全量同步 后面再优化成 offset 续传
输出物
单主单从复制
初始全量同步 + 在线增量同步
主从一致 testcase
建议
这个阶段的成功标准不是“高性能”，而是“slave 数据最终一致”。

阶段 7：引入 mmap 优化持久化加载
目标
实现你需求里提到的 mmap 持久化数据加载优化。

任务
分析当前快照/日志文件是否适合 mmap

是否是定长/半定长结构
是否需要额外索引
实现 mmap 读取层

映射文件
顺序扫描恢复数据
校验文件完整性
对比普通 read 与 mmap 的恢复性能

启动耗时
内存占用
10w / 50w / 100w 数据恢复对比
README 记录性能结果

测试环境
测试规模
对比结果
输出物
mmap 加载实现
恢复性能测试结果
建议
mmap 更适合作为优化项，不要在最开始就把它和功能实现绑死。

阶段 8：实现 TTL / key 超时
目标
补齐 key 超时机制。

任务
数据结构增加过期时间

expire_at 时间戳
协议支持

expire key seconds
ttl key
可选 setex
过期处理策略 建议先做两种：

惰性删除：访问时检查是否过期
定时扫描：后台线程周期清理
与持久化联动

快照要写入过期时间
恢复时跳过已过期数据
增量日志记录 expire 操作
与主从联动

主节点产生 expire 操作时要同步给 slave
避免主从各自独立过期导致不一致
输出物
TTL 命令
过期清理机制
TTL 持久化与复制支持
建议
过期一定要以主节点时间线为准，否则主从会出现细微不一致。

阶段 9：引入 RDMA / eBPF 做高性能同步优化
目标
完成你需求中的高级优化部分，但这应该是后置增强项，不应阻塞主线交付。

9.1：TCP 复制增强与 partial resync（已完成）
完成项
- replication backlog
- replid / offset 复制位点
- FULLRESYNC / CONTINUE 协议分支
- in-process partial resync
- slave 复制位点持久化
- cross-process restart partial resync
验收结果
- `make check-repl` 已覆盖全量同步、进程内重连 partial resync、跨进程重启 partial resync
- 当前验收通过

9.2：高性能优化前的观测与基线（已完成）
目标
- 在不引入 RDMA / eBPF 依赖的前提下，先固化复制链路性能基线
- 为后续 RDMA 传输实验和 eBPF 观测提供可对比数据

完成项
- `check-repl-metrics`：复制链路指标基线采集
- `check-repl-profile`：profiling 外层入口
- `check-repl-ebpf-env`：perf/eBPF 环境能力探测
- 复制 `INFO` 指标补充 `repl_transport`
- 复制 transport 抽象前置层
- `repl_transport_backend` 配置项，默认 `tcp`

验收结果
- `make check-repl-metrics` 通过，已输出 full sync / tail / restart 三阶段复制指标
- `make check-repl-profile` 通过，profiling helper 可生成 artifacts
- `make check-repl-ebpf-env` 通过，当前环境结论为 `profiling_readiness=minimal`

9.3：实验性 RDMA / eBPF（已完成阶段性闭环验证，当前处于工程化收尾）
当前状态
- `rdma` transport backend 已从 skeleton 演进为实验性可运行实现
- 本地环境已具备 Soft-RoCE (`rxe0`) 与 `bpftrace`，可执行真实 RDMA / eBPF 验证
- 已完成 RDMA smoke、RDMA stress、可选 eBPF 联动观测；当前主链路已完成 soak 验证并进入日志/文档收尾

当前完成项
- `rdma` transport backend 真实握手、收发、disconnect / reconnect 基础路径
- `check-repl-rdma-unsupported`：非 RDMA 构建场景下明确 unsupported 且行为可测试
- `run_repl_rdma_smoke.py`：验证 RDMA fullsync 与基础数据正确性
- `run_repl_rdma_stress.py`：验证 fullsync、tail 增量、restart partial resync，并支持 soak / eBPF artifacts
- RDMA stress 已验证：fullsync、tail 增量、跨进程 restart partial resync、soak reconnect 恢复全部通过
- slave 复制写本地持久化已补齐，消除“重启后 offset 前进但数据集回退”的一致性缺口
- eBPF 已验证：syscall/sched tracepoints + kvstore/rdmacm/ibverbs uprobes 可采集，且可输出结构化 summary

当前验证结论
- RDMA stress / soak 已通过：`fullsync_done=yes`、`postsync_tail_ok=yes`、`restart_rounds_ok=yes`、`soak_ok=yes`、`soak_availability_ok=yes`、`soak_recovery_ok=yes`、`final_resume_ok=yes`
- 新一轮 RDMA 稳定性收敛已完成：
  - 通过 defer ACK during fullsync、修复 stale replica 清理、`pending_recv` 队列化、master 持续接收 `REPLACK`、去掉重复 ACK、串行化 CQ 轮询，已将“中等负载复制失败”收敛为“中等负载通过”
  - 小一档回归（`64 / 16 / 1`）通过：`fullsync_done=yes`、`postsync_tail_ok=yes`、`restart_rounds_ok=yes`、`final_resume_ok=yes`
  - 中等负载回归（`128 / 32 / 2`）通过：`fullsync_done=yes`、`postsync_tail_ok=yes`、`restart_rounds_ok=yes`、`final_resume_ok=yes`
  - 剩余现象是 master 侧仍可能出现 `RDMA_CM_EVENT_DISCONNECTED`，但在当前验证中属于 `seen=yes` 且 `impactful=no`，不再影响复制正确性
- 后续如果继续收敛 RDMA 生命周期，优先方向不是再修复数据一致性，而是：
  - 进一步区分良性 disconnect 与真正故障 disconnect
  - 缩小 master async disconnect 的触发窗口
  - 视需要补充更长期 soak 和更大负载验证
- eBPF 已命中真实 RDMA 路径：`rdma_connect`、`rdma_accept`、`rdma_get_cm_event`、`repl_transport_send`、`repl_handle_replica_send_failure`
- 当前剩余工作以工程化收尾为主：压缩调试日志、沉淀最终 README/plan 命令、补充阶段性限制与结论

剩余工作
- 收敛并保留高信号 RDMA 观测点，移除高频成功噪声日志
- 在 README / plan 中沉淀最终验证命令、环境前提与限制条件
- 根据需要补充更长时长 soak 或参数矩阵验证

后续进入“基本完成”的条件
- README / plan / 验证命令完成收口
- RDMA 不可用时保持 TCP 降级不受影响
- eBPF 观测路径在 root / capability 前提下可重复执行

任务
明确范围 你的需求写的是：

部分数据同步用 rdma
实时数据同步用 ebpf
这里建议先落成“实验性增强”，而不是默认主路径。

RDMA 适合的场景

大块数据全量同步
批量日志传输
降低 CPU 拷贝开销
eBPF 适合的场景

网络路径观测
延迟分析
某些事件捕获
更像“监控/加速辅助”，不要一开始承担全部核心同步职责
实施顺序

先把 TCP 主从打通
再对全量同步引入 RDMA 通道
再考虑是否对增量实时复制做 eBPF 辅助观测或优化
增加降级策略

RDMA 不可用时自动退回 TCP
eBPF 不可用时不影响主流程
输出物
实验性 RDMA 同步通道
eBPF 观测/优化模块
性能收益报告
风险
这是整个需求里实现成本最高、环境依赖最重的一部分。非常容易拖慢整体进度，所以建议明确标成：

P0：不含 RDMA/eBPF 的可用版
P1：RDMA/eBPF 优化版
阶段 10：完善 README、测试和性能数据
目标
补齐项目交付质量，满足你对 README 和测试说明的要求。

任务
README 至少补齐以下内容

编译步骤
运行步骤
testcase 使用方法
持久化测试步骤
主从测试步骤
TTL 测试步骤
性能测试方法与结果
建立分层测试

单元测试：序列化、反序列化、TTL 判断
集成测试：save/load、增量恢复、主从复制
压测：10w / 50w / 100w 写入读取
性能指标建议

QPS
平均延迟 / P99
快照耗时
恢复耗时
主从同步延迟
输出物
完整 README
可重复执行测试流程
性能测试记录
四、推荐的优先级排序
如果你希望“先做出一个能交付的版本”，我建议优先级如下：

P0：必须先完成
现状梳理与架构整理
全量持久化
增量持久化
文档型 value 最小支持
单主单从复制
README + testcase 补齐
P1：增强项
mmap 加载优化
TTL / key 超时
P2：高阶优化
RDMA 同步
eBPF 实时同步/观测
五、建议的里程碑版本
v1：单机可恢复版
基础 KV
save/load
增量日志
testcase 10w 恢复通过
v2：文档存储版
支持 document 类型
文档字段增删改查
文档型持久化通过
v3：基础主从版
单主单从
全量 + 增量同步
slave 校验一致通过
v4：增强优化版
mmap
TTL
性能测试报告
v5：高性能实验版
RDMA
eBPF
性能收益分析
六、每阶段的验收标准
为了避免“做了很多但不好验收”，建议每阶段都定义明确标准：

阶段 2 全量持久化验收
插入 10w 条后 save 成功
重启自动恢复成功
testcase 全量读取一致
阶段 3 增量持久化验收
不做全量快照，仅靠增量日志也能恢复
多次写入/删除后重启一致
阶段 6 主从复制验收
slave 首次接入能完成全量同步
master 新增 5w 条后 slave 可读到
主从读取结果一致
阶段 8 TTL 验收
key 到期后不可读
重启后过期 key 不会“复活”
主从过期行为一致
七、对你当前项目最现实的开发建议
结合你现在项目状态，我建议你这样推进最稳：

第一轮
先只做：

README 补充
模块梳理
全量持久化
testcase 恢复验证
第二轮
再做：

增量持久化
mmap 优化
文档型数据结构
第三轮
再做：

主从复制
主从 testcase
第四轮
最后做：

TTL
RDMA / eBPF
这样可以保证你每一轮都有可展示成果，不会一下子陷入“大而全但都没跑通”的状态。

八、如果要落到“实际编码顺序”，建议这样排
读懂 kvstore.c 当前数据结构和命令流
把数据操作抽象成统一 API
增加对象元信息：类型、过期时间、版本号
实现 save/load
写 10w 恢复 testcase
实现增量日志 append/replay
让所有写命令都经过统一 op log
扩展 document 类型
实现 master/slave 基础连接
做全量同步
做增量同步
做 TTL
做 mmap 优化
最后再做 RDMA/eBPF