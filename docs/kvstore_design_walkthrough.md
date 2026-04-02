# KVStore 技术实现说明（按程序设计思路展开）

## 1. 项目目标与整体设计思路

这个项目的目标，不只是实现一个简单的 `set/get` 字典，而是实现一个具备以下能力的小型 Redis 风格 KV 服务：

- 网络收发与并发连接处理
- RESP 协议解析
- 多种底层 KV 存储结构
- key 过期删除
- 全量/增量持久化
- 主从复制
- 可切换内存分配策略
- 批量命令处理

从 `main()` 的初始化流程可以直接看出系统骨架：

```c
kvs_array_create(&global_array);
kvs_rbtree_create(&global_rbtree);
kvs_hash_create(&global_hash);
kvs_expire_create(&global_expire);
if (persist_init() != 0) { ... }
persist_recover();
return reactor_start();
```

这说明系统启动后会依次完成：

1. 初始化三种存储引擎
2. 初始化过期管理表
3. 初始化持久化文件
4. 从 dump + aof 恢复数据
5. 启动 reactor 事件循环对外服务

所以整套系统的主链路是：

```text
客户端请求
  -> 网络层接收
  -> RESP 协议解析
  -> 命令执行分发
  -> 存储引擎操作
  -> TTL / 持久化 / 复制联动
  -> 响应返回
```

---

## 2. 先搭命令处理中心，而不是先陷入数据结构细节

### 2.1 RESP 解析器是系统入口之一

项目把协议处理集中在 `parse_resp_stream()` 中。它不是一次只解析一条命令，而是支持从输入缓冲中持续解析多条 RESP 命令：

```c
int parse_resp_stream(conn_t *c, unsigned char *buf, size_t *len, int from_replication)
{
    size_t pos = 0;
    while (pos < *len) {
        ...
        handle_parsed_command(c, argc, argv, argl, buf + start, p - start, from_replication);
        ...
        pos = p;
    }
    ...
}
```

这个设计带来的几个重要能力：

- 支持 TCP 分包、粘包
- 支持 pipeline
- 支持恢复时回放文件中的多条命令
- 支持复制流中连续命令的执行

也就是说，**批量处理能力不是额外写出来的，而是协议解析层天然具备的能力**。

### 2.2 `handle_parsed_command()` 是整个系统的逻辑中枢

RESP 解析出来后，统一交给 `handle_parsed_command()` 执行。这里集中处理：

- 普通 KV 操作
- TTL 操作
- 保存快照
- 主从同步命令
- 内存统计命令
- 成功写入后的 AOF 追加
- 成功写入后的复制广播

它相当于一个“命令调度中心”。

其中最关键的一段是：

```c
if (rc == 0) {
    n = resp_simple_string(resp, sizeof(resp), "OK");
    if (!from_replication && is_write_cmd(cmd)) {
        persist_append_raw(raw, rawlen);
        if (g_cfg.role == ROLE_MASTER) repl_broadcast(raw, rawlen);
    }
}
```

这段代码说明：

- 命令执行成功后，先返回 OK
- 对写命令追加 AOF
- 主节点再把原始命令广播给从节点

因此，**持久化和复制都统一挂在命令执行成功之后**，逻辑非常集中。

---

## 3. 复杂 KV 存储的核心不是单一结构，而是统一抽象 + 三种引擎

项目支持三种存储引擎：

- Array
- Hash
- RBTree

### 3.1 通过命令前缀选择引擎

```c
static int cmd_engine(const char *cmd) {
    if (cmd[0] == 'R') return KVS_ENGINE_RBTREE;
    if (cmd[0] == 'H') return KVS_ENGINE_HASH;
    return KVS_ENGINE_ARRAY;
}
```

因此：

- `SET / GET / DEL` 默认走 array
- `RSET / RGET / RDEL` 走红黑树
- `HSET / HGET / HDEL` 走哈希表

然后再去掉前缀：

```c
static const char *strip_prefix(const char *cmd) {
    if (cmd[0] == 'R' || cmd[0] == 'H') return cmd + 1;
    return cmd;
}
```

这样就把“命令类型”和“底层引擎”拆开了。

### 3.2 用统一接口屏蔽底层差异

项目没有做复杂的对象系统，而是用几组统一分发函数：

- `engine_set`
- `engine_get`
- `engine_mod`
- `engine_del`
- `engine_exist`

例如：

```c
static int engine_set(int engine, char *key, char *value) {
    switch (engine) {
        case KVS_ENGINE_ARRAY: return kvs_array_set(&global_array, key, value);
        case KVS_ENGINE_RBTREE: return kvs_rbtree_set(&global_rbtree, key, value);
        case KVS_ENGINE_HASH: return kvs_hash_set(&global_hash, key, value);
        default: return -1;
    }
}
```

这样上层命令处理根本不关心下面到底是数组、树还是哈希。

---

## 4. 三种存储引擎分别如何实现

### 4.1 Array：最简单的顺序表实现

`kvs_array.c` 中的 array 本质上是固定大小的顺序表：

```c
inst->table = (kvs_array_item_t *)kvs_malloc(KVS_ARRAY_SIZE * sizeof(kvs_array_item_t));
```

查找使用线性扫描：

```c
static int find_slot(kvs_array_t *inst, char *key) {
    for (int i = 0; i < KVS_ARRAY_SIZE; ++i) {
        if (inst->table[i].key && strcmp(inst->table[i].key, key) == 0) return i;
    }
    return -1;
}
```

特点：

- 实现简单
- 易于验证功能链路
- 查找复杂度 O(n)
- 适合作为最基础的存储模型

插入时找空槽位，删除时释放 key/value 并清空槽位。

### 4.2 Hash：链地址法哈希表

`kvs_hash.c` 采用哈希桶 + 单链表的结构。

哈希函数使用 FNV 风格混合：

```c
unsigned int sum = 2166136261u;
for (int i = 0; key[i] != 0; ++i) {
    sum ^= (unsigned char)key[i];
    sum *= 16777619u;
}
```

冲突通过链表解决：

```c
new_node->next = hash->nodes[idx];
hash->nodes[idx] = new_node;
```

这意味着：

- 平均查找 O(1)
- 冲突处理简单
- 比 array 更适合作为通用 KV 结构

修改 value 时会重新申请新内存，然后替换旧 value，避免原地修改长度变化带来的风险。

### 4.3 RBTree：有序 KV 存储

`kvs_rbtree.c` 实现了完整的红黑树，包括：

- 左旋
- 右旋
- 插入修复
- 删除修复
- 查找
- 最小值 / 后继查找

创建时使用 `nil` 哨兵节点：

```c
inst->nil = (rbtree_node*)kvs_malloc(sizeof(rbtree_node));
inst->nil->color = BLACK;
inst->root = inst->nil;
```

这是一种经典实现方式，可以大幅减少空指针判断复杂度。

插入时先做普通 BST 插入，再通过 `rbtree_insert_fixup()` 恢复红黑树性质。删除时先找替代节点，再通过 `rbtree_delete_fixup()` 修复平衡。

它的意义在于：

- 维持 key 有序
- 支持中序遍历输出
- 为以后扩展范围查询、排序查询保留基础

---

## 5. 有了 KV 引擎之后，如何为 key 加上 TTL

如果只是普通 KV，到这里已经能用了。但一旦支持过期时间，就必须额外解决：

> 过期信息存在哪里？

这个项目没有把过期时间硬塞进三种存储结构，而是专门做了一张过期表。

### 5.1 过期表独立设计

`kvs_expire.c` 维护了全局过期表：

```c
kvs_expire_table_t global_expire = {0};
```

过期表不是只按 key 索引，而是按 `(engine, key)` 共同索引：

```c
static unsigned int expire_hash(const char *key, int engine) {
    ...
    h ^= (unsigned int)engine;
    ...
}
```

这样做是为了区分：

- array 中的 key
- hash 中的同名 key
- rbtree 中的同名 key

否则会冲突。

### 5.2 设置 TTL

命令层中：

```c
rc = kvs_expire_set(&global_expire, engine, argv[1], atoll(argv[2]) * 1000);
```

内部把秒转换成毫秒：

```c
long long expire_at = kvs_now_ms() + ttl_ms;
```

### 5.3 查询 TTL

```c
long long ttl = kvs_expire_ttl(&global_expire, engine, argv[1]);
```

语义大致是：

- `-1`：没有设置过期
- `-2`：已过期
- 正整数：剩余秒数

并且剩余秒数向上取整，更符合使用习惯。

### 5.4 过期删除为什么要分成被动和主动两种

#### 被动删除

每次访问 key 时，先检测是否过期：

```c
static int try_expire(int engine, char *key) {
    if (kvs_expire_is_expired(&global_expire, engine, key)) {
        engine_del(engine, key);
        kvs_expire_del(&global_expire, engine, key);
        return 1;
    }
    return 0;
}
```

这保证客户端访问到的数据一定不会是过期值。

#### 主动删除

如果某个 key 一直没人访问，它即使过期了，也不会自动消失。  
所以项目在事件循环中周期性触发：

```c
if (now - g_last_expire >= 100) {
    kvs_active_expire_cycle(32);
    g_last_expire = now;
}
```

也就是每隔大约 100ms 扫描一次，最多删 32 个。

两套机制配合后：

- 被动删除保证读到的数据一定有效
- 主动删除保证冷数据最终也能释放内存

---

## 6. 内存管理为什么单独抽象，以及三种策略如何切换

如果整个项目直接到处使用 `malloc/free`，后期很难比较不同分配策略的效果。  
所以作者统一抽象了：

- `kvs_malloc`
- `kvs_calloc`
- `kvs_realloc`
- `kvs_free`

这样 array/hash/rbtree/expire/reactor 等模块全部复用一套内存入口。

### 6.1 三种策略

`kvs_mem.c` 支持：

- libc
- jemalloc
- custom

启动参数解析：

```c
--mem libc
--mem jemalloc
--mem custom
```

### 6.2 libc 模式

最直接，就是普通系统分配器：

```c
return malloc(size);
```

### 6.3 jemalloc 模式

项目没有直接改成 `je_malloc()`，而是通过 `LD_PRELOAD` 重启进程，把整个进程的 malloc/free 切换到 jemalloc：

```c
setenv("LD_PRELOAD", preload, 1);
execvp(argv0, argv);
```

这种方式侵入性很小。

### 6.4 custom 模式

custom allocator 由三部分组成：

1. 小对象 slab
2. 大对象 mmap
3. 兜底 fallback malloc

#### 小对象 slab

预设多个大小等级：

- 32
- 64
- 128
- 256
- 384
- 512
- 768
- 1024

如果申请大小不超过 1024 字节，就找到最接近的 class，从 free list 中取 chunk。

如果 free list 不够，就通过 `mmap` 扩一整页 slab，再切成多个 chunk。

#### 大对象 mmap

超过 1024 的对象直接页对齐后 `mmap`：

```c
large_hdr_t *hdr = (large_hdr_t *)mmap(NULL, rounded, ...);
```

释放时 `munmap`。

#### fallback

如果 slab 或 mmap 路径失败，就退回普通 malloc，保证内存模块尽量不把服务拖垮。

### 6.5 内存统计

内存模块不只是分配和释放，还记录了大量统计信息，例如：

- alloc/free 次数
- 当前 small inuse
- current/peak large 映射量
- 内部碎片率
- slab page 利用率
- 每个 class 的 page/chunk 数量

这些信息通过 `MEMSTAT` 命令输出，方便比较三种策略的效果。

---

## 7. 持久化为什么拆成全量和增量

只做一种持久化通常不够。

- 只做快照：恢复快，但可能丢快照之后的数据
- 只做 AOF：完整，但文件可能越来越大

所以项目同时实现了全量和增量。

### 7.1 增量持久化：AOF

`persist_init()` 打开 AOF 文件：

```c
g_aof_fp = fopen(g_cfg.aof_path, "ab+");
```

命令执行成功后，把原始 RESP 命令直接写进去：

```c
persist_append_raw(raw, rawlen);
```

内部实现：

```c
fwrite(buf, 1, len, g_aof_fp);
fflush(g_aof_fp);
```

这是很重要的设计：  
**AOF 直接保存原始 RESP 命令流。**

### 7.2 全量持久化：dump 快照

`persist_save_dump()` 调用：

```c
kvs_snapshot_to_fp(fp);
```

而快照逻辑会遍历三种引擎，把当前数据全部编码成 RESP 命令：

- array -> `SET`
- rbtree -> `RSET`
- hash -> `HSET`

如果 key 还有 TTL，会额外补一条 `EXPIRE / REXPIRE / HEXPIRE`。

也就是说，dump 文件本质上也是一串 RESP 命令。

### 7.3 恢复流程

恢复顺序是：

```c
replay_file(g_cfg.dump_path);
replay_file(g_cfg.aof_path);
```

先恢复全量，再回放增量，这样状态才能正确。

更妙的是，恢复时不是单独写一套解析器，而是把文件内容重新喂回：

```c
parse_resp_stream(NULL, buf, &len, 1);
```

所以：

- 网络请求
- AOF 回放
- dump 恢复

都复用了同一套 RESP 执行链。

---

## 8. 主从复制为什么能建立在持久化能力之上

当你已经拥有：

- 完整快照导出能力
- 原始 RESP 命令流
- 通用命令执行入口

那么主从复制就很好做。

### 8.1 从节点如何同步

从节点启动后，如果角色是 slave，会在 reactor 启动时额外拉起复制线程：

```c
if (g_cfg.role == ROLE_SLAVE) {
    start_slave_thread();
}
```

线程逻辑：

1. 连接主节点
2. 发送 `REPLSYNC`
3. 持续读取主节点发送的数据
4. 把收到的 RESP 数据流喂给 `parse_resp_stream(NULL, buf, &blen, 1)`

也就是说，**从节点把主节点发来的同步流直接当作命令流执行**。

### 8.2 主节点如何识别从节点

在命令执行层：

```c
if (!strcmp(cmd, "REPLSYNC")) { 
    repl_add_slave(c); 
    queue_snapshot(c); 
    return 0; 
}
```

收到 `REPLSYNC` 的连接会被挂到副本链表里。

### 8.3 全量复制

`queue_snapshot(c)` 的流程是：

1. 发送 `FULLRESYNC`
2. 生成 dump 快照文件
3. 把 dump 文件内容分块排入发送队列
4. 最后发送 `REPLDONE`

注意，这里直接调用了原来的快照能力：

```c
kvs_snapshot_to_fp(fp)
```

所以主从全量同步完全复用了持久化模块。

### 8.4 增量复制

每次主节点写命令成功后：

```c
if (!from_replication && is_write_cmd(cmd)) {
    persist_append_raw(raw, rawlen);
    if (g_cfg.role == ROLE_MASTER) repl_broadcast(raw, rawlen);
}
```

主节点把原始 RESP 命令广播给所有从节点。  
从节点收到后又重新进入统一的 RESP 执行流程。

因此：

- AOF 增量持久化
- replication 增量同步

其实都基于同一份“原始写命令流”。

### 8.5 从库只读

从节点不允许客户端直接写：

```c
if (g_cfg.role == ROLE_SLAVE && !from_replication && is_readonly_slave_blocked(cmd)) {
    ...
}
```

但允许来自主节点复制流的写命令执行。  
这样可以保证主从数据来源单一。

---

## 9. reactor 网络层如何承接全部能力

整个服务最终还是要靠网络层托起来。项目采用的是：

- 非阻塞 socket
- epoll
- reactor

### 9.1 连接对象设计

通过 `fdmap[65536]` 按 fd 保存连接对象：

```c
static conn_t *fdmap[65536];
```

每个连接维护：

- 输入缓冲
- 输出队列
- 是否是监听 socket
- 是否是 replica

### 9.2 输出队列是关键

由于 socket 是非阻塞的，不能假设一次 `send()` 就能发完。  
所以所有响应、快照数据、复制命令都会先走：

```c
queue_bytes(c, buf, len);
```

这里会：

1. 复制一份数据到 `out_node_t`
2. 挂到连接的输出链表
3. 打开 `EPOLLOUT`

这样：

- 普通客户端响应
- 全量复制快照发送
- 增量复制命令广播

都走统一的异步发送机制。

### 9.3 读路径

`on_read()` 中不断 `recv()`，读到数据后累积到输入缓冲，再调用：

```c
parse_resp_stream(c, c->inbuf, &c->in_len, 0);
```

如果解析过程中产生了响应，就补开写事件。

### 9.4 写路径

`on_write()` 从输出队列头部开始发送：

- 如果发送一部分，就记录 `sent`
- 如果一个节点发完，就释放它
- 如果队列空了，就恢复成只监听 `EPOLLIN`

### 9.5 事件循环同时调度 TTL

事件循环每次 `epoll_wait` 返回后，还顺便检查时间并执行主动过期扫描：

```c
if (now - g_last_expire >= 100) {
    kvs_active_expire_cycle(32);
    g_last_expire = now;
}
```

所以 TTL 的后台清理并不需要额外线程，而是直接挂在主循环里。

---

## 10. 这些功能之间到底是怎么相互依赖的

### 10.1 复杂 KV 存储是地基

没有三种引擎和统一接口，就没有后面的：

- TTL 删除
- 快照遍历
- 命令恢复
- 复制同步

### 10.2 TTL 依赖统一删除接口

过期表只负责存 `(engine, key, expire_at)`，到期时通过：

```c
engine_del(cur->engine, cur->key);
```

删除实际数据。

### 10.3 全量/增量持久化依赖统一命令模型

无论 dump 还是 AOF，最后都变成 RESP 命令流。  
恢复时再走同一个协议执行入口。

### 10.4 主从复制依赖持久化能力

- 全量复制复用快照导出
- 增量复制复用原始 RESP 命令流

### 10.5 内存策略是横切能力

内存管理不属于某个业务点，而是被所有模块共同使用，因此影响整个系统。

### 10.6 批量处理依赖网络层 + 协议层

不是单独做了“批处理模块”，而是：

- reactor 读入整块数据
- RESP 解析器循环处理多条命令
- 输出队列顺序发送多条响应

---

## 11. 按程序演化路径总结

如果把这个项目看作逐步生长出来的系统，它大致是这样形成的：

1. **先做网络层和命令入口**
   - reactor
   - RESP 解析
   - 命令分发

2. **再做基础 KV**
   - array
   - hash
   - rbtree
   - 统一引擎接口

3. **再做 TTL**
   - 独立 expire 表
   - lazy expire
   - active expire

4. **再做持久化**
   - AOF 增量
   - dump 全量
   - 启动恢复

5. **再做主从复制**
   - REPLSYNC
   - FULLRESYNC
   - 增量广播

6. **最后做内存策略抽象**
   - libc / jemalloc / custom
   - MEMSTAT 可观测性

7. **批量处理则是协议层天然提供的能力**
   - pipeline 不是外加，而是顺带获得

---

## 12. 最值得抓住的几个设计亮点

### 12.1 RESP 命令流被三次复用

同一种 RESP 结构同时用于：

- 客户端请求
- AOF 文件
- 主从复制同步流

### 12.2 TTL 没侵入底层存储结构

过期信息独立维护，通过统一 `engine_del()` 删除，解耦非常好。

### 12.3 全量持久化与全量复制共用同一套快照逻辑

`kvs_snapshot_to_fp()` 同时服务于 dump 和 FULLRESYNC。

### 12.4 内存策略抽象是全系统级别的

不是只优化某个模块，而是让整个服务都能切换 allocator。

### 12.5 批量处理能力来自协议解析器设计，而不是额外堆功能

说明这个项目不是简单功能堆叠，而是有一定架构意识。

---

## 13. 结论

这个 KV 项目真正实现的，不只是三种数据结构，而是一条完整的服务链路：

```text
网络事件 -> 协议流解析 -> 统一命令调度 -> 多引擎 KV 操作
       -> TTL 管理 -> AOF/快照持久化 -> 主从同步 -> 响应返回
```

它的代码规模不算大，但已经具备一个小型 KV 数据库服务的主要骨架。  
如果继续往下扩展，可以继续增加：

- 范围查询
- 后台重写 AOF
- 多线程/分片
- 连接鉴权
- 更精细的内存回收与统计
- 更规范的复制偏移量管理

从教学和练手角度，这是一个已经相当完整的 mini-Redis 风格项目。
