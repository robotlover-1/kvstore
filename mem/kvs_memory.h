#ifndef KVS_MEMORY_H
#define KVS_MEMORY_H

#include <stdint.h>
#include <stddef.h>

typedef enum kvs_mem_mode_e {
    KVS_MEM_EXISTING = 0,
    KVS_MEM_JEMALLOC = 1,
    KVS_MEM_POOL     = 2
} kvs_mem_mode_t;

typedef struct kvs_mem_stats_s {
    uint64_t alloc_calls;
    uint64_t free_calls;
    uint64_t bytes_in_use;
    uint64_t bytes_peak;
    uint64_t bytes_total;
    uint64_t small_pool_allocs;
    uint64_t large_pool_allocs;
    uint64_t jemalloc_allocs;
    uint64_t jemalloc_frees;
} kvs_mem_stats_t;

int kvs_mem_init(kvs_mem_mode_t mode);
void kvs_mem_fini(void);
int kvs_mem_set_mode(kvs_mem_mode_t mode);
kvs_mem_mode_t kvs_mem_get_mode(void);
const char *kvs_mem_mode_name(kvs_mem_mode_t mode);
int kvs_mem_parse_mode(const char *name, kvs_mem_mode_t *mode_out);
void kvs_mem_get_stats(kvs_mem_stats_t *out);

void *kvs_malloc(size_t size);
void kvs_free(void *ptr);

#endif
