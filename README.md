# kvstore - 高性能键值存储系统

kvstore是一个用C语言实现的高性能、可扩展的键值存储系统，设计用于学习和研究目的。它提供了类似Redis的功能，支持多种存储引擎、内存后端和网络模型。

## 主要特性

### 存储引擎

- **数组(Array)**：简单数组存储，适合小规模数据
- **红黑树(RBTREE)**：自平衡二叉搜索树，支持有序数据
- **哈希表(Hash)**：链式哈希表，快速查找
- **跳表(Skiptable)**：多级索引结构，支持范围查询

### 内存管理

- **libc**：标准C库内存分配器
- **jemalloc**：高性能内存分配器，减少内存碎片
- **custom**：自定义内存分配器，支持slab分配和mmap大块内存

### 网络模型

- **Reactor**：基于epoll的事件驱动模型
- **Proactor**：基于io_uring的异步I/O模型
- **NtyCo**：协程模型，支持高并发连接

### 核心功能

- **RESP协议**：兼容Redis序列化协议
- **数据持久化**：支持AOF和RDB两种持久化方式
- **主从复制**：支持一主多从架构
- **哨兵模式**：支持高可用和自动故障转移
- **键过期**：支持TTL和过期自动清理
- **分布式锁**：支持锁获取、释放和续期

## 快速开始

### 编译安装

```bash
# 清理并编译
make clean && make

# 编译完成后生成可执行文件 kvstore
```

### 启动服务

```bash
# 使用默认配置文件启动（kvstore.conf）
./kvstore

# 指定配置文件启动（推荐）
./kvstore --config kvstore.conf

# 仍支持命令行参数覆盖（兼容模式，建议迁移到配置文件）
./kvstore --config kvstore.conf --port 6380 --mem jemalloc
```

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

## 配置文件（推荐）

默认读取仓库根目录 `kvstore.conf`，也可通过 `--config <path>` 指定。

配置示例：

```ini
port = 5000
bind_ip = 0.0.0.0
role = master
master_host = 127.0.0.1
master_port = 5000
dump_path = kvstore.dump
aof_path = kvstore.aof
appendfsync = always
autosnap =
mem_backend = libc
net_backend = reactor
log_mode = console
persist_mode = aof
sentinel = false
sentinel_master_name = mymaster
sentinel_monitor_host = 127.0.0.1
sentinel_monitor_port = 5000
sentinel_known_slaves =
sentinel_down_after_ms = 5000
sentinel_failover_timeout_ms = 10000
sentinel_quorum = 1
```

## 命令行参数（兼容模式）

| 参数                          | 说明                                | 默认值       |
| ----------------------------- | ----------------------------------- | ------------ |
| `--config`                    | 配置文件路径                        | kvstore.conf |
| `--bind`                      | 监听IP地址                          | 0.0.0.0      |
| `--port`                      | 监听端口                            | 5000         |
| `--role`                      | 角色（master/slave）                | master       |
| `--master-host`               | 主节点地址（从节点使用）            | 127.0.0.1    |
| `--master-port`               | 主节点端口（从节点使用）            | 5000         |
| `--dump`                      | RDB快照文件路径                     | kvstore.dump |
| `--aof`                       | AOF文件路径                         | kvstore.aof  |
| `--mem`                       | 内存后端（libc/jemalloc/custom）    | libc         |
| `--net`                       | 网络模型（reactor/proactor/ntyco）  | reactor      |
| `--log-mode`                  | 日志模式（预留配置）                | console      |
| `--persist-mode`              | 持久化模式（预留配置）              | aof          |
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

### 单元测试

```bash
# 编译测试程序
cd tests
make

# 运行测试
./test
```

### 集成测试

```bash
# 运行Redis协议兼容性测试
./scripts/test_resp_nc_strict.sh

# 运行队列恢复测试
./scripts/test_resp_queue_recovery.sh

# 运行TTL测试
./scripts/test_resp_ttl_nc.sh
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
