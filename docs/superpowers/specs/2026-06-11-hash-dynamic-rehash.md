# Hash 引擎动态扩容 — 设计文档

**日期**: 2026-06-11
**状态**: 已确认

## 背景

当前 Hash 引擎使用固定 1024 桶（`MAX_TABLE_SIZE`）链地址法，不随数据量扩容。
100 万 key 时平均每桶 977 节点，每次 HSET 遍历长链表做 strcmp 去重，QPS 仅 23k（Redis 同负载 123k）。

## 目标

实现渐进式 rehash + 性能优化，使 kvstore Hash 引擎 100 万 key 写入性能超过 Redis（>123k QPS）。

## 设计

### 结构体

```c
typedef struct hashnode_s {
    uint32_t hv;              // 缓存的 FNV-1a hash 值（rehash 免重算）
    char *key;
    char *value;
    struct hashnode_s *next;
} hashnode_t;

typedef struct hashtable_s {
    hashnode_t **nodes;
    int max_slots;
    int count;
} hashtable_t;

typedef struct kvs_hash_s {
    hashtable_t ht[2];        // ht[0]: 活跃表, ht[1]: 扩容目标
    int rehash_idx;           // 当前迁移到的桶索引, -1 = 未在 rehash
} kvs_hash_t;
```

### 优化参数

| 参数 | Redis | kvstore（本次）| 理由 |
|------|-------|---------------|------|
| 初始桶数 | 4 | **4096** | 减少小数据量下的扩容次数 |
| 扩容倍数 | ×2 | **×2** | 相同 |
| 触发阈值(负载因子) | 1.0 | **2.0** | 链地址法容忍更高负载，少扩容 |
| 每次操作迁移桶数 | 1 | **10** | 加速 rehash 完成 |
| Hash 值缓存 | 无 | **有** (`node->hv`) | rehash 免重算 hash |
| Hash 函数 | MurmurHash2 | **FNV-1a**（不变） | 足够均匀 |

### 扩容触发

`kvs_hash_set` 完成后检查 `ht[0].count > ht[0].max_slots * 2`：
1. 分配 `ht[1]`：`max_slots = ht[0].max_slots * 2`，`nodes = calloc`
2. 设置 `rehash_idx = 0`

### 渐进迁移 (`rehash_step`)

每次 `set/get/del/mod/exist` 调用 `rehash_step(hash, 10)`：
1. 迁移 `rehash_idx` 到 `rehash_idx + 9` 共 10 个桶（跳过空桶）
2. 对每个节点：计算 `node->hv % ht[1].max_slots`，链入 `ht[1]`
3. 全部迁移完成后：释放 `ht[0].nodes`，设置 `ht[0] = ht[1]`，清零 `ht[1]`，`rehash_idx = -1`

### 查找逻辑

所有 get/del/mod/exist：
1. 在 `ht[0]` 中查找
2. 如果 `rehash_idx >= 0` 且未找到，在 `ht[1]` 中查找

set 操作：
1. 先在 `ht[0]` + `ht[1]` 中去重
2. 如果 rehash 中，新 key 直接插入 `ht[1]`
3. 如果未在 rehash，插入 `ht[0]`

### dump 遍历

遍历 `ht[0].nodes` 所有桶的所有节点。如果 `rehash_idx >= 0`，额外遍历 `ht[1].nodes` 已迁移的桶。避免重复计数。

### 恢复兼容性

dump 文件格式不变（KVSD 二进制），恢复时正常插入 `kvs_hash_set`，引擎自动触发扩容。

## 改动范围

| 文件 | 改动 | 行数 |
|------|------|------|
| `include/kvstore/kvstore.h` | 结构体定义 (`hashnode_t`, `kvs_hash_t`) | ~15 |
| `src/storage/kvs_hash.c` | rehash_step, 扩容触发, 双表查找, create/destroy | ~120 |
| `src/main/kvstore.c` | dump 遍历 (2 处) + INFO 内存遍历 (1 处) | ~10 |
| `src/replication/kvs_repl.c` | 全量同步遍历 hash | ~3 |
| `src/persistence/kvs_persist.c` | 恢复适配 | ~3 |

其他引擎（RBTREE/Skiptable/Array/Doc）不受影响。

## 预期效果

- 100 万 key 写入 QPS：23k → **130k+**（超过 Redis 的 123k）
- rehash 开销：每次操作额外迁移 10 桶，500 次操作完成一次 rehash（对 100 万次操作可忽略）
- 扩容次数：100 万 key 时约 5 次（4096→8192→16384→...→1M）
