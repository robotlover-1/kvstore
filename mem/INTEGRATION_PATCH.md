# 集成补丁说明

## 一、在 `kvstore.h` 中加入

```c
#include "kvs_memory.h"
```

## 二、在 `main()` 中解析 `--mem`

示例：

```c
kvs_mem_mode_t mem_mode = KVS_MEM_EXISTING;

for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--mem") == 0 && i + 1 < argc) {
        if (kvs_mem_parse_mode(argv[i + 1], &mem_mode) != 0) {
            fprintf(stderr, "invalid memory mode: %s\n", argv[i + 1]);
            return -1;
        }
        ++i;
    }
}

if (kvs_mem_init(mem_mode) != 0) {
    fprintf(stderr, "failed to initialize memory backend: %s\n", kvs_mem_mode_name(mem_mode));
    return -1;
}
```

退出前：

```c
kvs_mem_fini();
```

## 三、Makefile 加入

```make
OBJS += kvs_memory.o

kvs_memory.o: kvs_memory.c kvs_memory.h
	gcc -Wall -Wextra -O2 -c kvs_memory.c -o kvs_memory.o
```

如果你的环境中需要 `dlopen`：

```make
LDFLAGS += -ldl
```

## 四、所有模块只保留 `kvs_malloc/kvs_free`

确认这些文件内不要直接散落 `malloc/free`：

- kvs_array.c
- kvs_rbtree.c
- kvs_hash.c
- kvs_expire.c
- kvs_persist.c
- kvs_repl.c
- reactor.c（如有连接对象或写队列节点动态分配）

## 五、jemalloc 依赖

运行前确认：

```bash
ldconfig -p | grep jemalloc
```

若无输出，`--mem jemalloc` 会初始化失败。
