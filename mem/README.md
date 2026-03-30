# RESP + TTL + 主从同步 分支内存后端集成包

这是一个**内存后端集成包**，用于把三种内存分配策略合并进你之前的 `RESP + TTL + 主从同步 + 三引擎并存` 分支：

- `existing`：保持现有行为
- `jemalloc`：运行时 `dlopen` jemalloc
- `pool`：自定义内存池，小内存和大内存分开策略

## 说明

我能在当前环境里**真实提供并验证**的是这套内存后端实现本身，以及一个可运行的演示程序。

当前工作区里并没有你之前那套完整的 `RESP + TTL + 主从同步 + 三引擎并存` 源码树，所以我不能诚实地声称这里已经把它完整合并并编译通过了。

因此，这个包提供的是：

1. `kvs_memory.c` / `kvs_memory.h`
2. 一个可运行的 `mempool_demo` 演示程序
3. 一份**精确的集成说明**，告诉你把这套内存后端并入你当前分支时要改哪些位置

## 你需要改的地方

### 1. 在你的 `kvstore.h` 里加入

```c
#include "kvs_memory.h"
```

并确保全工程统一使用：

```c
void *kvs_malloc(size_t size);
void kvs_free(void *ptr);
```

### 2. 在你的主程序初始化时加内存模式解析

推荐支持：

```bash
./kvstore --port 5000 --role master --mem existing
./kvstore --port 5000 --role master --mem jemalloc
./kvstore --port 5000 --role master --mem pool
./kvstore --port 5001 --role slave --master-host 127.0.0.1 --master-port 5000 --mem pool
```

启动前：

```c
kvs_mem_mode_t mem_mode = KVS_MEM_EXISTING;
/* 解析 --mem */
if (kvs_mem_init(mem_mode) != 0) {
    fprintf(stderr, "failed to init memory backend: %s\n", kvs_mem_mode_name(mem_mode));
    return -1;
}
```

退出前：

```c
kvs_mem_fini();
```

### 3. 三引擎代码保持不变，只需要继续使用 `kvs_malloc/kvs_free`

- `kvs_array.c`
- `kvs_rbtree.c`
- `kvs_hash.c`
- `kvs_expire.c`
- `kvs_persist.c`
- `kvs_repl.c`

凡是内存分配，统一改成：

```c
kvs_malloc(...)
kvs_free(...)
```

### 4. 主从同步 / AOF / dump 都不需要特殊改造

因为它们只要继续通过 `kvs_malloc/kvs_free` 分配节点、buffer、连接对象、复制缓存即可，内存后端会在底层切换。

## 模式说明

### existing
直接走系统默认 `malloc/free`，语义最接近你当前分支。

### jemalloc
通过 `dlopen("libjemalloc.so.2")` 或 `dlopen("libjemalloc.so")` 动态装载。
如果运行环境没有 jemalloc 共享库，初始化会失败。

### pool
- 小于等于 256 字节的分配：走 16 / 32 / 64 / 128 / 256 五档 size class
- 大于 256 字节：走大块链表管理

## 本包附带的演示程序

`mempool_demo` 可以验证三种模式都能正常分配、释放并打印统计：

```bash
./mempool_demo existing
./mempool_demo jemalloc
./mempool_demo pool
```

## 为什么给的是“集成包”而不是完整 merged server

因为这次对话里此前提到的某些“已生成分支”并不在当前实际文件系统中。我不想再给你一个看似完整但实际上没有被真实构建过的包。

这个包里给你的内容都是真实存在、可下载、可编译、可运行验证的。
