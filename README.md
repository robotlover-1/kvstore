# kvstore - 高性能键值存储系统

kvstore是一个用C语言实现的高性能、可扩展的键值存储系统，设计用于学习和研究目的。它提供了类似Redis的功能，支持多种存储引擎、内存后端和网络模型。

## 主要特性

> 注意：下面分为“当前代码已实现并可验证的能力”和“后续规划能力”。本轮开始会按阶段推进实现，并在每一步提供对应验证方法。

### 当前代码已实现并可验证的能力

#### 存储引擎

- **数组(Array)**：基础键值存储
- **红黑树(RBTREE)**：前缀 `R*` 命令
- **哈希表(Hash)**：前缀 `H*` 命令
- **跳表(Skiptable)**：前缀 `X*` 命令

#### 内存管理

- **libc**：标准 C 库内存分配器
- **jemalloc**：高性能内存分配器
- **custom**：自定义内存分配器，支持 slab 与大块映射统计

#### 网络模型

- **Reactor**：基于 epoll
- **Proactor**：基于 io_uring
- **NtyCo**：协程模型

#### 核心功能

- **RESP 协议解析与响应**
- **AOF + dump 恢复链路**：启动时可回放 `dump` 和 `aof`，恢复路径优先使用 `mmap` 加载文件内容，大文件或映射失败时回退到普通流式读取
- **SAVE / BGSAVE / BGREWRITEAOF**：持久化落盘优先尝试 `io_uring` 文件写/`fsync`，失败时回退到传统同步文件 IO；AOF 主路径已切到 `fd-first`，不再依赖 `FILE*` 的 `fflush`，只有仍经过 stdio 的路径才需要先 `fflush`
- **主从复制基础能力**：支持 `SLAVEOF`、`ROLE`、`REPLSYNC` 全量同步与增量广播
- **TTL / 过期清理**：支持 `EXPIRE`、`TTL`、`PERSIST`
- **分布式锁基础命令**：`LOCK`、`UNLOCK`、`RENEW`、`OWNER`
- **文档型 value（最小对象模型）**：支持 `DOCSET`、`DOCGET`、`DOCDEL`、`DOCDROP`、`DOCEXIST`、`DOCCOUNT`、`DOCGETALL`
- **INFO / MEMSTAT / 自动快照规则**

### 当前尚未完成或需要继续增强的能力

- 更严格的“10w/100w 数据级”持久化回归测试与性能结果整理
- 更完整的主从异常恢复与断点续传机制
- RDMA / eBPF 同步优化
- README 中部分历史描述与实际实现的进一步对齐

## 快速开始

### 编译安装

```bash
# 清理并编译
make clean && make

# 编译完成后生成可执行文件 kvstore
```

### 启动服务

```bash
# 使用默认配置启动（若当前目录存在 kvstore.conf，会自动加载该配置文件）
./kvstore

# 显式指定配置文件启动
./kvstore --config kvstore.conf

# 指定端口和内存后端（命令行参数会覆盖配置文件同名项）
./kvstore --config kvstore.conf --port 6380 --mem jemalloc

# 指定网络模型
./kvstore --config kvstore.conf --net proactor --port 5000

# 启动从节点
./kvstore --config kvstore.conf --role slave --master-host 127.0.0.1 --master-port 5000

# 启动哨兵模式
./kvstore --config kvstore.conf --sentinel --sentinel-master-name mymaster --sentinel-monitor-host 127.0.0.1 --sentinel-monitor-port 5000
```

### 配置文件

项目根目录已提供示例配置文件：`kvstore.conf`

配置文件格式为 `key=value`，支持空行和 `#` 注释。例如：

```ini
port=5000
role=master
master_host=127.0.0.1
master_port=5000

dump_path=kvstore.dump
aof_path=kvstore.aof
mem_backend=libc
net_backend=reactor
appendfsync=always

autosnap=60:1000,300:10

sentinel=0
sentinel_master_name=mymaster
sentinel_monitor_host=127.0.0.1
sentinel_monitor_port=5000
sentinel_known_slaves=
sentinel_down_after_ms=5000
sentinel_failover_timeout_ms=10000
sentinel_quorum=1
```

说明：

- 如果当前目录存在 `kvstore.conf`，`./kvstore` 会自动加载它
- 也可以通过 `--config <path>` 显式指定配置文件
- 命令行参数优先级高于配置文件
- 当前配置文件已覆盖现有主要启动配置项，包括 `port`、`dump_path`、`aof_path`、`mem_backend`、`net_backend`、`appendfsync`、`autosnap`、主从配置和哨兵配置

### 基本使用

使用netcat或Redis客户端连接：

```bash
# 设置键值
printf '*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n' | nc 127.0.0.1 5000

# 获取键值
printf '*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n' | nc 127.0.0.1 5000

# 删除键
printf '*2\r\n$3\r\nDEL\r\n$3\r\nkey\r\n' | nc 127.0.0.1 5000
```

## 命令行参数


| 参数                          | 说明                                | 默认值       |
| ----------------------------- | ----------------------------------- | ------------ |
| `--config`                    | 配置文件路径                        | `kvstore.conf` |
| `--port`                      | 监听端口                            | 5000         |
| `--role`                      | 角色（master/slave）                | master       |
| `--master-host`               | 主节点地址（从节点使用）            | 127.0.0.1    |
| `--master-port`               | 主节点端口（从节点使用）            | 5000         |
| `--dump`                      | RDB快照文件路径                     | kvstore.dump |
| `--aof`                       | AOF文件路径                         | kvstore.aof  |
| `--mem`                       | 内存后端（libc/jemalloc/custom）    | libc         |
| `--net`                       | 网络模型（reactor/proactor/ntyco）  | reactor      |
| `--appendfsync`               | AOF同步策略（always/everysec）      | always       |
| `--autosnap`                  | 自动快照规则（秒:变化数,秒:变化数） | 无           |
| `--sentinel`                  | 启用哨兵模式                        | 关闭         |
| `--sentinel-master-name`      | 哨兵监控的主节点名称                | mymaster     |
| `--sentinel-monitor-host`     | 哨兵监控的主节点地址                | 127.0.0.1    |
| `--sentinel-monitor-port`     | 哨兵监控的主节点端口                | 5000         |
| `--sentinel-known-slaves`     | 已知从节点列表                      | 空           |
| `--sentinel-down-after`       | 节点下线判定时间（毫秒）            | 5000         |
| `--sentinel-failover-timeout` | 故障转移超时时间（毫秒）            | 10000        |
| `--sentinel-quorum`           | 哨兵投票法定人数                    | 1            |

## 支持的命令

### 基本键值操作

- **SET key value**：设置键值
- **GET key**：获取键值
- **DEL key**：删除键
- **EXIST key**：检查键是否存在
- **MSET key1 value1 key2 value2 ...**：批量设置
- **MGET key1 key2 ...**：批量获取
- **MOD key value**：修改键值（需键已存在）

### 存储引擎前缀

命令支持前缀指定存储引擎：

- **RSET/RGET/RDEL**：红黑树引擎操作
- **HSET/HGET/HDEL**：哈希表引擎操作
- **XSET/XGET/XDEL**：跳表引擎操作
- 无前缀：数组引擎操作

### 过期和TTL

- **EXPIRE key seconds**：设置键过期时间
- **TTL key**：获取键剩余生存时间
- **PERSIST key**：移除键的过期时间

当前 TTL 系统已从“固定桶 + 全表扫描”优化为“哈希索引 + 最小堆调度 + 自适应主动清理预算”：

- 过期项可通过哈希索引按 `engine + key` 快速定位
- 主动过期优先处理最早到期的 key，不再每轮扫描整张过期表
- 当过期 key 数量增大时，事件循环会自动提升每轮清理预算
- 更适合大量 key 同时设置 TTL 的场景

对于海量 TTL 压测，建议优先使用 `HSET/HGET/HEXPIRE/HTTL` 对应的哈希引擎命令，而不是默认数组引擎。默认数组引擎容量较小，更适合基础功能验证。

### 服务器管理

- **INFO**：获取服务器信息
- **MEMSTAT**：获取内存统计信息
- **PING**：测试连接
- **QUIT**：关闭连接
- **SAVE**：同步保存RDB快照
- **BGSAVE**：后台保存RDB快照
- **BGREWRITEAOF**：重写AOF文件
- **APPENDFSYNC policy**：设置AOF同步策略
- **CONFIG APPENDFSYNC policy**：配置AOF同步策略

### 自动快照管理

- **SNAPRULE seconds changes**：添加自动快照规则
- **SNAPRULES**：查看所有快照规则
- **SNAPRULECLEAR**：清除所有快照规则

### 分布式锁

- **LOCK key owner seconds**：获取锁
- **UNLOCK key owner**：释放锁
- **RENEW key owner seconds**：续期锁
- **OWNER key**：查看锁的所有者

### 文档型对象操作

- **DOCSET key field value**：设置文档字段；若文档不存在则自动创建
- **DOCGET key field**：获取文档字段值
- **DOCDEL key field**：删除文档中的单个字段
- **DOCDROP key**：删除整个文档
- **DOCEXIST key**：检查文档是否存在
- **DOCCOUNT key**：返回文档字段数量
- **DOCGETALL key**：返回文档全部字段和值（RESP array）

第一版文档模型是“扁平对象模型”：

- 只支持 `key -> field -> string value`
- 适合作为 plan 第四阶段的最小可用实现
- 已接入 dump/AOF 恢复链路
- 已接入主从复制广播链路

### 复制和集群

- **SLAVEOF host port**：设置为指定主节点的从节点
- **SLAVEOF NO ONE**：停止复制，变为主节点
- **ROLE**：查看服务器角色和复制状态

## 架构设计

### 代码结构

```
kvstore/
├── src/
│   ├── main/           # 主程序入口
│   ├── core/           # 网络模型实现
│   ├── storage/        # 存储引擎实现
│   ├── memory/         # 内存管理
│   ├── expire/         # 键过期处理
│   ├── persistence/    # 持久化实现
│   ├── replication/    # 复制和哨兵
│   └── utils/          # 工具函数
├── include/kvstore/    # 头文件
├── clients/            # 客户端示例
├── benchmarks/         # 性能测试
├── tests/              # 测试代码
└── scripts/            # 辅助脚本
```

### 存储引擎架构

系统支持四种存储引擎，可通过编译宏启用或禁用：

- `ENABLE_ARRAY`：数组存储
- `ENABLE_RBTREE`：红黑树存储
- `ENABLE_HASH`：哈希表存储
- `ENABLE_SKIPTABLE`：跳表存储

### 内存管理架构

自定义内存分配器（custom后端）特点：

- 小内存（≤1024B）：使用size class + slab page + freelist
- 大内存（>1024B）：直接使用mmap
- 详细的统计信息：分配次数、使用内存、碎片率等

### 网络模型对比

- **Reactor**：基于epoll，适合I/O密集型场景
- **Proactor**：基于io_uring，适合高并发异步场景
- **NtyCo**：基于协程，适合连接数多的场景

## 性能基准测试

当前持久化链路说明：

- AOF 主路径已切换为 `fd-first`，优先尝试 `io_uring` 文件写
- dump / rewrite 快照序列化已支持直接写入目标 `fd`，不再必须经过 `FILE*`
- AOF / dump / rewrite 的落盘同步优先尝试 `io_uring fsync`
- 若运行环境、队列初始化或提交失败，则自动回退到 `pwrite/fsync`
- 恢复加载路径优先使用 `mmap`，失败时回退到普通流式读取

项目提供完整的基准测试套件：

### 运行基准测试

```bash
# 进入基准测试目录
cd benchmarks/scripts

# 基础测试
python3 bench_mem_backend.py --ops 50000 --value-size 128

# 带参数测试
python3 bench_mem_backend.py --ops 100000 --value-size 256 --warmup 1000 --csv my_bench.csv

# 使用包装脚本
../run_benchmark.sh bench_mem_backend.py --ops 50000 --value-size 128
```

### 测试指标

- **QPS**：每秒操作数（SET/GET/HSET等）
- **内存使用**：VmRSS（物理内存）、VmSize（虚拟内存）
- **内存碎片**：内存间隙（VmSize - VmRSS）
- **自定义内存统计**：slab使用情况、mmap统计等

### 测试场景建议

```bash
# 小value测试（观察slab效果）
python3 bench_mem_backend.py --ops 50000 --value-size 64

# 大value测试（观察mmap效果）
python3 bench_mem_backend.py --ops 20000 --value-size 4096

# 混合场景测试
python3 bench_mem_backend.py --ops 30000 --value-size 128 --csv bench_small.csv
python3 bench_mem_backend.py --ops 30000 --value-size 4096 --csv bench_large.csv
```

## 客户端支持

### Python客户端示例

```python
import socket

def kvstore_command(host, port, *args):
    conn = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    conn.connect((host, port))
  
    # 构建RESP协议命令
    cmd = f"*{len(args)}\r\n"
    for arg in args:
        cmd += f"${len(arg)}\r\n{arg}\r\n"
  
    conn.send(cmd.encode())
    response = conn.recv(4096)
    conn.close()
    return response.decode()

# 使用示例
response = kvstore_command("127.0.0.1", 5000, "SET", "mykey", "myvalue")
print(response)
```

### 其他语言客户端

项目提供了多种语言的客户端示例：

- `clients/py-kvstore.py`：Python示例
- `clients/go-kvstore.go`：Go示例
- `clients/js-kvstore.js`：JavaScript示例
- `clients/javakvstore.java`：Java示例
- `clients/rust-kvstore.rs`：Rust示例

## 测试方法

### 当前推荐的基线验证顺序

在继续做后续功能改造前，建议先固定使用下面这套基线验证：

```bash
# 1. 编译
make clean && make

# 2. 启动服务
./kvstore

# 3. 运行基线验证
make check

# 4. 单独验证主从复制
make check-repl
```

说明：

- `make check-resp`：验证 RESP 基础读写、多包、精确返回格式
- `make check-ttl`：验证 TTL / EXPIRE / PERSIST
- `make check-persist`：验证基础持久化命令与文件落盘
- `make check-doc`：验证文档型 value 的增删改查与保存
- `make check-mass-ttl`：验证大量 TTL key 的写入、到期与回收行为
- `make check-repl`：验证主从全量同步和增量复制

如果服务端口不是默认 `5000`，可以这样运行：

```bash
make check TEST_PORT=6380
```

如果主从测试端口需要调整：

```bash
make check-repl REPL_MASTER_PORT=7000 REPL_SLAVE_PORT=7001
```

### 单元测试

当前 `tests/test.c` 为空，项目现阶段以集成验证脚本为主。后续在做文档型 value、mmap、TTL 扩展时，再补充真正的单元测试。

### 集成测试

```bash
# RESP基础能力
make check-resp

# TTL能力
make check-ttl

# 持久化基础验证
make check-persist

# 文档型 value 验证
make check-doc

# 海量 TTL key 验证
make check-mass-ttl

# io_uring 持久化正确性/性能 smoke 验证
make check-uring-persist

# 主从复制验证
make check-repl
```

### io_uring 持久化基准/正确性验证

```bash
# 默认 smoke 验证
make check-uring-persist

# 指定写入条数与 fsync 策略
make check-uring-persist URING_PERSIST_COUNT=500 URING_PERSIST_APPEND_FSYNC=always
```

说明：

- 该脚本会启动一个临时 kvstore 实例
- 批量写入 key 后执行 `SAVE`
- 输出写入耗时、SAVE 耗时、重启恢复校验耗时
- 同时校验 `aof`/`dump` 文件存在且恢复后的 key 可读

### mmap 恢复性能增强版验证

```bash
# 默认 mmap 恢复验证（默认使用 hash 引擎，适合较大 key 数）
make check-mmap-recover

# 指定恢复数据规模
make check-mmap-recover MMAP_RECOVER_COUNT=5000 MMAP_RECOVER_APPEND_FSYNC=everysec

# 指定底层引擎（array/hash/rbtree/skiptable）
make check-mmap-recover MMAP_RECOVER_ENGINE=hash MMAP_RECOVER_COUNT=5000
```

说明：

- 启动临时实例写入数据并生成 `dump` / `aof`
- 重启后统计恢复 wall time
- 通过 `INFO` 输出恢复阶段统计信息
- 重点观测 `recover_mmap_attempts`、`recover_mmap_success`、`recover_mmap_fallbacks`、`recover_mmap_bytes`

### 海量 TTL key 验证

```bash
# 先启动服务
./kvstore --port 5100

# 小规模验证
make check-mass-ttl TEST_PORT=5100 MASS_TTL_KEYS=200 MASS_TTL_SECONDS=2 MASS_TTL_BATCH=50 MASS_TTL_SAMPLE=10

# 更大规模验证
make check-mass-ttl TEST_PORT=5100 MASS_TTL_KEYS=5000 MASS_TTL_SECONDS=2 MASS_TTL_BATCH=500 MASS_TTL_SAMPLE=20
```

可调参数：

- `MASS_TTL_KEYS`：写入 key 总数
- `MASS_TTL_SECONDS`：每个 key 的 TTL 秒数
- `MASS_TTL_BATCH`：批次大小
- `MASS_TTL_SAMPLE`：过期前后抽样检查数量

说明：

- `check-mass-ttl` 默认使用哈希引擎命令进行验证，更适合大量 key 场景
- 该脚本会验证“写入成功 / 过期前可读 / 过期后删除 / 服务仍可响应”四类行为
- 如果要逐步扩大规模，建议从 `200 -> 5000 -> 20000` 逐步提升

### 第四阶段（文档型 value）建议验证步骤

```bash
# 1. 编译
make clean && make

# 2. 启动服务
./kvstore --port 5099

# 3. 运行文档对象测试
make check-doc TEST_PORT=5099
```

如果你想手工验证，可以执行：

```bash
# 写入文档字段
printf '*4\r\n$6\r\nDOCSET\r\n$6\r\nuser:1\r\n$4\r\nname\r\n$5\r\nalice\r\n' | nc 127.0.0.1 5099

# 读取文档字段
printf '*3\r\n$6\r\nDOCGET\r\n$6\r\nuser:1\r\n$4\r\nname\r\n' | nc 127.0.0.1 5099

# 查看字段数量
printf '*2\r\n$8\r\nDOCCOUNT\r\n$6\r\nuser:1\r\n' | nc 127.0.0.1 5099

# 获取整个文档
printf '*2\r\n$9\r\nDOCGETALL\r\n$6\r\nuser:1\r\n' | nc 127.0.0.1 5099
```

验证持久化恢复：

```bash
# 保存
printf '*1\r\n$4\r\nSAVE\r\n' | nc 127.0.0.1 5099

# 重启服务后再次读取
printf '*3\r\n$6\r\nDOCGET\r\n$6\r\nuser:1\r\n$4\r\nname\r\n' | nc 127.0.0.1 5099
```

### 性能测试

```bash
# 运行完整性能测试套件
./scripts/run_benchmark.sh

# 运行复制全同步测试
./scripts/run_repl_fullsync_test.sh

# 运行持久化性能测试
./scripts/run_save_bgsave_perf_test.sh
```

## 开发指南

### 添加新命令

1. 在`src/main/kvstore.c`的`handle_parsed_command`函数中添加命令处理逻辑
2. 如果需要持久化，调用`persist_note_write()`和`persist_append_raw()`
3. 如果需要复制，调用`repl_broadcast()`
4. 添加相应的测试用例

### 添加新存储引擎

1. 在`include/kvstore/kvstore.h`中定义引擎ID和数据结构
2. 实现引擎的CRUD操作函数
3. 在`src/storage/`目录下创建引擎实现文件
4. 更新编译配置（Makefile）
5. 在`handle_parsed_command`中集成引擎支持

### 内存后端开发

1. 实现`kvs_malloc`、`kvs_free`等内存操作函数
2. 实现`kvs_mem_get_stats`函数提供统计信息
3. 在`kvs_mem_init`中初始化后端
4. 更新`kvs_mem_prepare_process`处理进程启动

## 故障排除

### jemalloc启动问题

如果遇到jemalloc模式的static TLS block问题，系统会自动通过LD_PRELOAD重启进程：

```bash
# 错误信息示例
jemalloc: ... static TLS block ...

# 自动处理
系统会自动设置 LD_PRELOAD 并重启进程
```

### 内存泄漏检测

使用MEMSTAT命令监控内存使用：

```bash
printf '*1\r\n$7\r\nMEMSTAT\r\n' | nc 127.0.0.1 5000
```

关键指标：

- `current_small_inuse`：当前使用的小内存块数
- `peak_small_inuse`：小内存使用峰值
- `current_large_inuse_bytes`：当前大内存使用字节数
- `internal_fragment_ppm`：内部碎片率（百万分之一）

### 性能问题排查

1. 检查网络模型是否适合场景
2. 检查内存后端选择
3. 使用INFO命令查看服务器状态
4. 运行基准测试对比不同配置

## 贡献指南

### 开发环境设置

1. 确保安装gcc、make、python3等基础工具
2. 克隆仓库：`git clone <repository-url>`
3. 编译：`make clean && make`

### 代码规范

1. 遵循项目已有的代码风格
2. 添加必要的注释和文档
3. 确保新功能有相应的测试
4. 更新README和相关文档

### 提交PR

1. 从最新的main分支创建特性分支
2. 确保所有测试通过
3. 更新CHANGELOG（如果有）
4. 提供详细的PR描述

## 许可证

本项目采用开源许可证。详细信息请查看LICENSE文件。

## 参考资源

- [Redis协议规范](https://redis.io/topics/protocol)
- [io_uring文档](https://unixism.net/loti/)
- [jemalloc文档](http://jemalloc.net/)
- [NtyCo协程库](https://github.com/wangbojing/NtyCo)

## 联系方式

如有问题或建议，请通过以下方式联系：

- 项目GitHub仓库
- 提交Issue
- 参与讨论

---

*最后更新：2026年*
