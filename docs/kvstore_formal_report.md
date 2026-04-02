# KVStore 技术文档（正式报告版）

## 第一章 总体架构设计

### 1.1 项目定位

KVStore 是一个面向教学与工程实践的小型键值存储服务，整体目标是实现一个具备以下能力的类 Redis 系统：

- 基于 TCP 的网络服务
- RESP 协议解析与响应
- 多种底层存储引擎
- key 过期控制
- 全量/增量持久化
- 主从复制
- 可切换的内存分配策略
- 批量命令处理

系统主程序在初始化阶段完成三类核心资源准备：存储引擎、过期表、持久化系统；之后恢复历史数据，最后进入事件循环对外提供服务。

### 1.2 分层设计

系统可以抽象为以下 5 层：

#### 1）网络事件层

负责 socket 创建、连接接入、epoll 事件监听、非阻塞读写和输出队列发送。

对应模块：

- `reactor.c`

#### 2）协议解析层

负责把 TCP 字节流按 RESP 协议解析成命令与参数。

对应模块：

- `kvstore.c` 中的 `parse_resp_stream()`

#### 3）命令调度层

负责把解析结果映射为具体动作，并协调 TTL、持久化、复制等附加能力。

对应模块：

- `kvstore.c` 中的 `handle_parsed_command()`

#### 4）存储执行层

负责实际读写键值数据，支持三种存储引擎。

对应模块：

- `kvs_array.c`
- `kvs_hash.c`
- `kvs_rbtree.c`

#### 5）横切能力层

负责为整个系统提供辅助能力，包括：

- 过期管理：`kvs_expire.c`
- 持久化：`kvs_persist.c`
- 主从复制：`kvs_repl.c`
- 内存管理：`kvs_mem.c`

### 1.3 总体数据流

系统运行时的主数据流如下：

```text
Client
  -> TCP 连接
  -> reactor 读事件
  -> 输入缓冲
  -> RESP 解析
  -> 命令执行
  -> 底层引擎操作
  -> TTL / 持久化 / 复制联动
  -> 输出队列
  -> reactor 写事件
  -> Client
```

### 1.4 多引擎抽象设计

系统没有把所有数据统一塞到一个结构中，而是支持三种并行引擎：

- array
- hash
- rbtree

命令通过前缀区分引擎：

- `SET/GET/DEL` -> array
- `RSET/RGET/RDEL` -> rbtree
- `HSET/HGET/HDEL` -> hash

这种设计允许系统在统一协议与命令入口下复用不同底层结构。

### 1.5 配置与启动参数

系统通过命令行解析配置，支持如下参数：

- `--port`
- `--role master|slave`
- `--master-host`
- `--master-port`
- `--dump`
- `--aof`
- `--mem libc|jemalloc|custom`

配置统一保存在全局结构 `g_cfg` 中，由各模块共享使用。

---

## 第二章 请求处理时序

本章描述系统在运行时处理一个客户端请求的完整时序。

### 2.1 连接建立时序

1. 主线程在 `reactor_start()` 中创建监听 socket
2. 设置 `SO_REUSEADDR`
3. 将监听 fd 设置为非阻塞
4. 注册到 epoll
5. 等待客户端连接到来
6. `on_accept()` 中调用 `accept()` 接入新连接
7. 为新连接分配 `conn_t`
8. 新连接注册 `EPOLLIN`

### 2.2 普通写请求时序（以 SET 为例）

```text
客户端发送 SET key value
  -> reactor 触发 EPOLLIN
  -> on_read()
  -> recv() 读取数据进入 inbuf
  -> parse_resp_stream()
  -> handle_parsed_command()
  -> engine_set()
  -> 存储引擎写入
  -> persist_append_raw() 追加 AOF
  -> repl_broadcast() 广播给从节点（主节点场景）
  -> queue_bytes() 放入响应
  -> reactor 触发 EPOLLOUT
  -> on_write()
  -> send() 返回 +OK
```

### 2.3 普通读请求时序（以 GET 为例）

```text
客户端发送 GET key
  -> on_read()
  -> parse_resp_stream()
  -> handle_parsed_command()
  -> try_expire() 先检查 TTL
  -> engine_get()
  -> resp_bulk()/resp_null_bulk()
  -> queue_bytes()
  -> on_write()
  -> 返回 value 或 nil
```

### 2.4 批量命令处理时序

当客户端一次发送多条 RESP 命令时，`parse_resp_stream()` 会在一个循环中逐条解析并逐条执行。
输入缓冲中的多条命令按照字节顺序被依次拆出、执行、排队响应。

因此系统天然支持：

- pipeline
- 批量回放 AOF
- 批量接收主从复制流

### 2.5 TTL 时序

#### 被动过期

在 GET、MOD、DEL、EXPIRE、TTL、PERSIST 等访问路径上，系统会先调用 `try_expire()`：

1. 检查 key 是否已过期
2. 若已过期，删除底层 key
3. 同时删除过期表记录
4. 再继续本次命令逻辑

#### 主动过期

主事件循环中定时触发：

1. `epoll_wait(..., 100)` 返回
2. 检查当前时间与 `g_last_expire`
3. 若超过 100ms，则调用 `kvs_active_expire_cycle(32)`
4. 最多删除 32 个过期项
5. 更新时间基准

### 2.6 主从复制时序

#### 从节点启动

1. 如果 `role=slave`，在 `reactor_start()` 中调用 `start_slave_thread()`
2. 复制线程不断尝试连接主节点
3. 连接成功后发送 `REPLSYNC`

#### 主节点处理同步

1. 主节点收到 `REPLSYNC`
2. 调用 `repl_add_slave(c)` 注册副本连接
3. 调用 `queue_snapshot(c)` 发送全量快照
4. 后续主节点写命令成功后调用 `repl_broadcast(raw, rawlen)` 发送增量命令

#### 从节点应用数据

1. 从节点线程 `recv()`
2. 收到的字节流进入 `parse_resp_stream(NULL, ..., 1)`
3. 复制流中的命令复用统一命令执行路径
4. 从库状态逐步追平主库

### 2.7 启动恢复时序

服务启动时执行：

1. `persist_init()`
2. `persist_recover()`
3. 先 `replay_file(dump)`
4. 再 `replay_file(aof)`
5. 两类文件内容都重新送给 `parse_resp_stream(NULL, ..., 1)`

这样即可完成状态恢复。

---

## 第三章 各模块源码级实现

## 3.1 `kvstore.c`：主控制模块

### 3.1.1 主要职责

- 管理全局配置 `g_cfg`
- RESP 编码函数
- 命令参数解析
- 命令执行调度
- 存储引擎统一分发
- 快照导出
- 程序入口 `main()`

### 3.1.2 RESP 编码工具

包括：

- `resp_simple_string`
- `resp_error`
- `resp_integer`
- `resp_bulk`
- `resp_null_bulk`
- `resp_build_cmd1/2/3`

这些函数的作用是：

- 给客户端返回 RESP 响应
- 为复制与持久化构造标准命令流

### 3.1.3 引擎映射逻辑

通过 `cmd_engine()` 与 `strip_prefix()` 完成命令到引擎与操作名的拆分。

### 3.1.4 命令执行逻辑

`handle_parsed_command()` 是核心。主要处理以下命令类型：

- 复制控制：`REPLSYNC`、`REPLDONE`
- 信息查询：`INFO`、`MEMSTAT`
- 持久化触发：`SAVE`
- 数据操作：`SET/MOD/DEL/GET/EXIST`
- TTL 操作：`EXPIRE/TTL/PERSIST`
- 三种引擎对应的 `R*` / `H*` 命令

### 3.1.5 复制与 AOF 挂载点

写命令成功后：

```text
命令成功
  -> AOF 追加
  -> 若为 master，则广播给 replicas
```

这是系统最重要的联动点之一。

### 3.1.6 快照导出

`kvs_snapshot_to_fp()` 负责把当前三种引擎中的数据全部导出到文件流中。
快照格式不是私有二进制，而是 RESP 命令流。

导出函数分为：

- `snapshot_array()`
- `snapshot_hash()`
- `snapshot_rbtree_node()`

如果 key 存在剩余 TTL，则额外追加过期命令。

### 3.1.7 程序入口

`main()` 完成：

1. 参数解析
2. 内存后端准备
3. 内存模块初始化
4. 三种引擎创建
5. 过期表创建
6. 持久化初始化
7. 恢复历史数据
8. 启动 reactor

---

## 3.2 `reactor.c`：网络与事件循环模块

### 3.2.1 主要职责

- 创建监听 socket
- 管理 epoll
- 新连接接入
- 非阻塞读写
- 输出缓冲发送
- 复制连接清理
- 周期性触发过期扫描

### 3.2.2 fd 到连接对象映射

```c
static conn_t *fdmap[65536];
```

通过 fd 数组映射快速定位连接上下文。

### 3.2.3 输出队列设计

`queue_bytes()` 负责把待发送内容封装到 `out_node_t` 链表中，并打开 `EPOLLOUT`。
这是实现非阻塞响应和复制流发送的关键机制。

### 3.2.4 读路径

`on_read()` 中持续读取到 `conn_t.inbuf`，并调用 `parse_resp_stream()`。

如果对端关闭连接，但本端还有待发数据，则先切换到写事件而不是立刻关闭，这个细节对复制和短连接响应都很重要。

### 3.2.5 写路径

`on_write()` 从输出链表头部开始发送：

- 发送成功则更新 `sent`
- 节点全部发送完成则释放
- 队列为空则恢复成只监听 `EPOLLIN`

### 3.2.6 事件优先级处理

如果一个连接既有 `EPOLLIN` 又有 `EPOLLOUT`，且当前已有待发队列，则优先写。
这样可以避免先读到 EOF 而过早关闭连接。

### 3.2.7 过期扫描挂载点

主循环中按时间间隔触发 `kvs_active_expire_cycle(32)`，把后台清理逻辑与网络事件整合在一个线程中。

---

## 3.3 `kvs_array.c`：顺序表引擎

### 3.3.1 数据结构

固定长度数组，每个元素持有：

- `key`
- `value`

### 3.3.2 实现特点

- 查找：线性扫描
- 插入：找空槽位
- 删除：清空槽位
- 修改：重新分配新 value

### 3.3.3 适用性

优点是简单、直观、便于调试。
缺点是查找效率低，更适合小规模数据或作为最基础的演示引擎。

---

## 3.4 `kvs_hash.c`：哈希表引擎

### 3.4.1 数据结构

- 桶数组 `hash->nodes`
- 单链表解决冲突

### 3.4.2 哈希函数

采用 FNV 风格混合算法，避免简单字符求和导致分布不均。

### 3.4.3 操作逻辑

- `set`：查重后头插
- `get`：链表遍历匹配 key
- `mod`：新分配 value 并替换
- `del`：维护前驱指针删除节点

### 3.4.4 工程意义

这是系统中最适合通用 KV 读写的引擎，实现效率和复杂度平衡较好。

---

## 3.5 `kvs_rbtree.c`：红黑树引擎

### 3.5.1 数据结构

标准红黑树节点包含：

- key
- value
- color
- left/right/parent

使用 `nil` 哨兵表示空叶子。

### 3.5.2 关键实现

- `rbtree_left_rotate`
- `rbtree_right_rotate`
- `rbtree_insert_fixup`
- `rbtree_delete_fixup`
- `rbtree_search`

### 3.5.3 封装层

红黑树底层算法完成后，又被封装成 KV 接口：

- `kvs_rbtree_set`
- `kvs_rbtree_get`
- `kvs_rbtree_mod`
- `kvs_rbtree_del`
- `kvs_rbtree_exist`

### 3.5.4 作用

为系统提供有序存储能力，是未来扩展范围查询、顺序遍历的重要基础。

---

## 3.6 `kvs_expire.c`：过期管理模块

### 3.6.1 数据结构

过期项单独存放于哈希桶中，每项包括：

- key
- engine
- expire_at_ms
- next

### 3.6.2 设计要点

过期表通过 `(engine, key)` 联合索引，避免三种引擎的同名 key 冲突。

### 3.6.3 主要接口

- `kvs_expire_set`
- `kvs_expire_del`
- `kvs_expire_persist`
- `kvs_expire_is_expired`
- `kvs_expire_ttl`
- `kvs_active_expire_cycle`

### 3.6.4 与引擎的解耦

过期模块自身不持有 value，也不直接依赖具体结构，只在删除时调用统一的 `engine_del()`。

---

## 3.7 `kvs_persist.c`：持久化模块

### 3.7.1 AOF

`persist_append_raw()` 直接把原始 RESP 命令追加到 AOF 文件。
因此 AOF 文件本质上就是命令日志。

### 3.7.2 dump

`persist_save_dump()` 调用 `kvs_snapshot_to_fp()` 导出全量状态。
dump 的格式同样是 RESP 命令流。

### 3.7.3 恢复

`persist_recover()`：

1. 回放 dump
2. 回放 aof

恢复依赖的是统一 RESP 解析器，而不是专门写一套文件恢复协议。

---

## 3.8 `kvs_repl.c`：复制模块

### 3.8.1 从节点线程模型

slave 启动一个独立线程持续重连主库。同步建立后：

- 发送 `REPLSYNC`
- 接收主库发来的全量/增量数据
- 直接按 RESP 流执行

### 3.8.2 主节点副本管理

主节点在 `kvstore.c` 中维护 `g_replicas` 链表，并提供：

- `repl_add_slave`
- `repl_remove_slave`
- `repl_broadcast`

### 3.8.3 全量同步

由 `queue_snapshot()` 完成：

- `FULLRESYNC`
- dump 数据
- `REPLDONE`

### 3.8.4 增量同步

通过 `repl_broadcast(raw, rawlen)` 广播原始写命令。

---

## 3.9 `kvs_mem.c`：内存管理模块

### 3.9.1 目标

提供统一的内存分配接口，让整个项目可以切换不同 allocator。

### 3.9.2 支持的后端

- `libc`
- `jemalloc`
- `custom`

### 3.9.3 custom allocator 结构

#### 小对象

使用 slab 分类管理，每个 size class 维护：

- chunk 大小
- free_list
- pages
- 统计信息

#### 大对象

使用独立 `mmap`

#### fallback

无法走前两种路径时退回普通 malloc

### 3.9.4 统计能力

支持记录：

- 调用次数
- inuse 大小
- 页面占用
- 内部碎片
- 每个 class 的 page/chunk 状态

并通过 `MEMSTAT` 命令导出。

---

## 3.10 其他网络模型文件

项目中还存在：

- `proactor.c`
- `ntyco.c`

它们体现了作者对其它网络模型的尝试，但主线运行逻辑最终由 `reactor.c` 承担。
因此从当前主程序入口来看，正式生效的是 reactor 模型。

---

## 第四章 模块依赖图

### 4.1 逻辑依赖图

```text
                 +------------------+
                 |    kvstore.c     |
                 | 主控/命令调度层  |
                 +---------+--------+
                           |
     +---------------------+----------------------+
     |                     |                      |
     v                     v                      v
+-----------+       +-------------+       +--------------+
| reactor.c |       | kvs_persist |       |  kvs_repl.c  |
| 网络事件层 |       |  持久化模块  |       |  复制模块     |
+-----+-----+       +------+------|       +------+-------+
      |                    |                     |
      |                    |                     |
      |                    v                     |
      |            +---------------+             |
      |            | snapshot 导出 |<------------+
      |            +-------+-------+
      |                    |
      v                    v
+-----+----------------------------------------------+
|                存储引擎统一接口层                  |
|    engine_set/get/mod/del/exist in kvstore.c      |
+-----+----------------+----------------------+------+
      |                |                      |
      v                v                      v
+-----------+    +-----------+         +-------------+
| kvs_array |    | kvs_hash  |         | kvs_rbtree  |
+-----------+    +-----------+         +-------------+
      ^                ^                      ^
      |                |                      |
      +----------------+----------+-----------+
                                 |
                                 v
                           +------------+
                           | kvs_expire |
                           | TTL 模块    |
                           +------------+

所有模块共同依赖：
    +------------+
    | kvs_mem.c  |
    | 内存管理    |
    +------------+
```

### 4.2 依赖关系说明

#### 1）`kvstore.c` 是核心调度者

它依赖所有底层模块，并协调它们之间的调用。

#### 2）`reactor.c` 依赖 `kvstore.c`

因为读取到协议字节流后最终要调用 RESP 解析和命令执行逻辑。

#### 3）`kvs_expire.c` 依赖统一引擎接口的删除语义

它自己不管理 value，只负责到期后调用底层删除。

#### 4）`kvs_persist.c` 依赖 `kvs_snapshot_to_fp()`

快照导出逻辑定义在 `kvstore.c` 中。

#### 5）`kvs_repl.c` 依赖 RESP 解析器

从节点接收到主节点同步流后，直接复用命令执行链。

#### 6）`kvs_mem.c` 是横切依赖

所有模块都通过 `kvs_malloc/free` 使用统一内存后端。

### 4.3 功能复用关系

#### RESP 命令流复用

同一套 RESP 格式被用于：

- 网络请求
- AOF 文件
- dump 恢复
- 主从复制

#### 快照逻辑复用

同一套 `kvs_snapshot_to_fp()` 被用于：

- `SAVE` 全量持久化
- `FULLRESYNC` 全量复制

#### 输出队列复用

`queue_bytes()` 同时服务于：

- 客户端响应
- 主从复制增量广播
- 全量复制数据发送

---

## 第五章 启动与恢复流程图

### 5.1 启动流程图

```text
main(argc, argv)
  |
  +--> parse_args()
  |       |
  |       +--> 解析 port / role / dump / aof / mem 等参数
  |
  +--> 若 mem=jemalloc
  |       |
  |       +--> kvs_mem_prepare_process()
  |               |
  |               +--> 设置 LD_PRELOAD 并 exec 重新拉起进程
  |
  +--> kvs_mem_init()
  |
  +--> kvs_array_create()
  |
  +--> kvs_rbtree_create()
  |
  +--> kvs_hash_create()
  |
  +--> kvs_expire_create()
  |
  +--> persist_init()
  |
  +--> persist_recover()
  |       |
  |       +--> replay dump
  |       |
  |       +--> replay aof
  |
  +--> reactor_start()
          |
          +--> 创建监听 socket
          +--> epoll 注册监听 fd
          +--> 若 role=slave，则启动复制线程
          +--> 进入主事件循环
```

### 5.2 恢复流程图

```text
persist_recover()
  |
  +--> replay_file(dump_path)
  |       |
  |       +--> fread() 读取文件块
  |       +--> parse_resp_stream(NULL, buf, &len, 1)
  |       +--> 执行 dump 中的 SET/HSET/RSET/EXPIRE 命令
  |
  +--> replay_file(aof_path)
          |
          +--> fread() 读取文件块
          +--> parse_resp_stream(NULL, buf, &len, 1)
          +--> 执行后续增量写命令
```

### 5.3 主从启动流程图

#### 主节点

```text
main()
  -> role=master
  -> 正常初始化
  -> reactor_start()
  -> 等待客户端与从节点连接
```

#### 从节点

```text
main()
  -> role=slave
  -> 正常初始化
  -> persist_recover()
  -> reactor_start()
       |
       +--> start_slave_thread()
               |
               +--> connect(master)
               +--> send(REPLSYNC)
               +--> recv(sync stream)
               +--> parse_resp_stream(..., from_replication=1)
```

### 5.4 一次完整写请求的系统级流转

```text
Client: SET key value
   |
   v
TCP socket
   |
   v
reactor.on_read()
   |
   v
parse_resp_stream()
   |
   v
handle_parsed_command()
   |
   +--> engine_set()
   |
   +--> persist_append_raw()
   |
   +--> repl_broadcast()   [仅 master]
   |
   +--> queue_bytes(+OK)
   |
   v
reactor.on_write()
   |
   v
Client 收到响应
```

### 5.5 一次 TTL 到期的流转

#### 被动过期

```text
Client: GET key
   |
   v
handle_parsed_command()
   |
   +--> try_expire()
           |
           +--> kvs_expire_is_expired()
           +--> engine_del()
           +--> kvs_expire_del()
   |
   +--> engine_get() -> 返回 null
```

#### 主动过期

```text
reactor main loop
   |
   +--> 时间片到达
   |
   +--> kvs_active_expire_cycle(32)
           |
           +--> 扫描过期桶
           +--> engine_del()
           +--> 删除过期记录
```

### 5.6 结论

启动流程、恢复流程、复制流程、请求处理流程最终都汇聚到同一个核心设计：

1. 所有状态变化都尽量表示成 RESP 命令流
2. 所有命令最终都尽量复用统一执行入口
3. 复杂功能不是各写各的，而是建立在统一协议、统一调度和统一引擎抽象之上

因此，这个项目虽然规模不大，但架构上已经具备较强的一致性和可扩展性。




---

# 附录：简要总结

本系统最重要的设计特点可以概括为以下几点：

1. **统一命令执行入口**
   网络请求、AOF 回放、复制同步统一走 RESP 解析 + 命令执行链。
2. **多引擎统一抽象**
   array/hash/rbtree 被统一成标准接口，方便 TTL、持久化和复制复用。
3. **TTL 与存储解耦**
   过期信息独立维护，不侵入三种存储结构定义。
4. **全量与增量并存**
   dump 负责全量，AOF 负责增量，兼顾恢复效率和数据完整性。
5. **主从复制高度复用既有能力**
   全量复制复用快照，增量复制复用原始命令流。
6. **内存策略可插拔**
   通过统一 `kvs_malloc/free` 支持 libc / jemalloc / custom 三种后端。
7. **批量处理天然成立**
   不是额外拼接出来的功能，而是 RESP 流式解析设计的自然结果。
