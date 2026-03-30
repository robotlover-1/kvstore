#include "kvs_memory.h"
#include <string.h>
#include <stdlib.h>

#include <dlfcn.h>
#include <errno.h>

#define KVS_POOL_PAGE_SIZE      (64 * 1024)
#define KVS_POOL_SMALL_CLASSES  5

static const size_t g_small_class_sizes[KVS_POOL_SMALL_CLASSES] = {16, 32, 64, 128, 256};

typedef struct kvs_small_page_s {
    struct kvs_small_page_s *next;
    void *memory;
    void *free_list;
    size_t block_size;
    size_t capacity;
    size_t in_use;
} kvs_small_page_t;

typedef struct kvs_large_node_s {
    struct kvs_large_node_s *next;
    void *user_ptr;
    size_t size;
} kvs_large_node_t;

typedef struct kvs_pool_state_s {
    kvs_small_page_t *small_pages[KVS_POOL_SMALL_CLASSES];
    kvs_large_node_t *large_list;
} kvs_pool_state_t;

typedef struct kvs_alloc_hdr_s {
    uint32_t magic;
    uint16_t mode;
    uint16_t class_idx;
    uint64_t size;
    void *owner;
} kvs_alloc_hdr_t;

#define KVS_HDR_MAGIC 0x4B56534DU /* KVSM */

static kvs_mem_mode_t g_mode = KVS_MEM_EXISTING;
static kvs_mem_stats_t g_stats;
static kvs_pool_state_t g_pool;

static void *g_jemalloc_handle = NULL;
static void *(*g_je_malloc_fn)(size_t) = NULL;
static void (*g_je_free_fn)(void *) = NULL;
static void *(*g_je_calloc_fn)(size_t, size_t) = NULL;
static void *(*g_je_realloc_fn)(void *, size_t) = NULL;

static int kvs_jemalloc_open(void) {
    if (g_jemalloc_handle) return 0;
    g_jemalloc_handle = dlopen("libjemalloc.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!g_jemalloc_handle) {
        g_jemalloc_handle = dlopen("libjemalloc.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!g_jemalloc_handle) return -1;

    g_je_malloc_fn = (void *(*)(size_t))dlsym(g_jemalloc_handle, "malloc");
    g_je_free_fn = (void (*)(void *))dlsym(g_jemalloc_handle, "free");
    g_je_calloc_fn = (void *(*)(size_t, size_t))dlsym(g_jemalloc_handle, "calloc");
    g_je_realloc_fn = (void *(*)(void *, size_t))dlsym(g_jemalloc_handle, "realloc");

    if (!g_je_malloc_fn || !g_je_free_fn) {
        dlclose(g_jemalloc_handle);
        g_jemalloc_handle = NULL;
        g_je_malloc_fn = NULL;
        g_je_free_fn = NULL;
        g_je_calloc_fn = NULL;
        g_je_realloc_fn = NULL;
        return -1;
    }
    return 0;
}

static void kvs_jemalloc_close(void) {
    if (g_jemalloc_handle) dlclose(g_jemalloc_handle);
    g_jemalloc_handle = NULL;
    g_je_malloc_fn = NULL;
    g_je_free_fn = NULL;
    g_je_calloc_fn = NULL;
    g_je_realloc_fn = NULL;
}

static int kvs_small_class_index(size_t size) {
    for (int i = 0; i < KVS_POOL_SMALL_CLASSES; ++i) {
        if (size <= g_small_class_sizes[i]) return i;
    }
    return -1;
}

static void kvs_stats_alloc(size_t size, int small_pool, int large_pool, int jemalloc_backend) {
    g_stats.alloc_calls++;
    g_stats.bytes_in_use += size;
    g_stats.bytes_total += size;
    if (g_stats.bytes_in_use > g_stats.bytes_peak) g_stats.bytes_peak = g_stats.bytes_in_use;
    if (small_pool) g_stats.small_pool_allocs++;
    if (large_pool) g_stats.large_pool_allocs++;
    if (jemalloc_backend) g_stats.jemalloc_allocs++;
}

static void kvs_stats_free(size_t size, int jemalloc_backend) {
    g_stats.free_calls++;
    if (g_stats.bytes_in_use >= size) g_stats.bytes_in_use -= size;
    else g_stats.bytes_in_use = 0;
    if (jemalloc_backend) g_stats.jemalloc_frees++;
}

static kvs_small_page_t *kvs_new_small_page(size_t block_size) {
    kvs_small_page_t *page = (kvs_small_page_t *)malloc(sizeof(*page));
    if (!page) return NULL;

    memset(page, 0, sizeof(*page));
    page->memory = malloc(KVS_POOL_PAGE_SIZE);
    if (!page->memory) {
        free(page);
        return NULL;
    }

    page->block_size = block_size;
    page->capacity = KVS_POOL_PAGE_SIZE / block_size;
    page->free_list = NULL;

    unsigned char *p = (unsigned char *)page->memory;
    for (size_t i = 0; i < page->capacity; ++i) {
        void *block = p + i * block_size;
        *(void **)block = page->free_list;
        page->free_list = block;
    }
    return page;
}

static void *kvs_pool_small_alloc(size_t size) {
    int idx = kvs_small_class_index(size + sizeof(kvs_alloc_hdr_t));
    if (idx < 0) return NULL;

    size_t block_size = g_small_class_sizes[idx];
    kvs_small_page_t *page = g_pool.small_pages[idx];
    while (page && page->free_list == NULL) page = page->next;

    if (!page) {
        page = kvs_new_small_page(block_size);
        if (!page) return NULL;
        page->next = g_pool.small_pages[idx];
        g_pool.small_pages[idx] = page;
    }

    void *block = page->free_list;
    page->free_list = *(void **)block;
    page->in_use++;

    kvs_alloc_hdr_t *hdr = (kvs_alloc_hdr_t *)block;
    hdr->magic = KVS_HDR_MAGIC;
    hdr->mode = (uint16_t)KVS_MEM_POOL;
    hdr->class_idx = (uint16_t)idx;
    hdr->size = (uint64_t)size;
    hdr->owner = page;

    kvs_stats_alloc(size, 1, 0, 0);
    return (void *)(hdr + 1);
}

static void *kvs_pool_large_alloc(size_t size) {
    size_t total = sizeof(kvs_alloc_hdr_t) + size;
    kvs_alloc_hdr_t *hdr = (kvs_alloc_hdr_t *)malloc(total);
    if (!hdr) return NULL;

    hdr->magic = KVS_HDR_MAGIC;
    hdr->mode = (uint16_t)KVS_MEM_POOL;
    hdr->class_idx = 0xFFFFu;
    hdr->size = (uint64_t)size;
    hdr->owner = NULL;

    kvs_large_node_t *node = (kvs_large_node_t *)malloc(sizeof(*node));
    if (!node) {
        free(hdr);
        return NULL;
    }

    node->user_ptr = (void *)(hdr + 1);
    node->size = size;
    node->next = g_pool.large_list;
    g_pool.large_list = node;

    kvs_stats_alloc(size, 0, 1, 0);
    return (void *)(hdr + 1);
}

static void kvs_pool_small_free(kvs_alloc_hdr_t *hdr) {
    kvs_small_page_t *page = (kvs_small_page_t *)hdr->owner;
    if (!page) return;

    *(void **)hdr = page->free_list;
    page->free_list = (void *)hdr;
    if (page->in_use > 0) page->in_use--;
    kvs_stats_free((size_t)hdr->size, 0);
}

static void kvs_pool_large_free(kvs_alloc_hdr_t *hdr) {
    kvs_large_node_t *prev = NULL;
    kvs_large_node_t *cur = g_pool.large_list;
    void *user_ptr = (void *)(hdr + 1);

    while (cur) {
        if (cur->user_ptr == user_ptr) {
            if (prev) prev->next = cur->next;
            else g_pool.large_list = cur->next;
            free(cur);
            break;
        }
        prev = cur;
        cur = cur->next;
    }

    kvs_stats_free((size_t)hdr->size, 0);
    free(hdr);
}

static void *kvs_existing_alloc(size_t size) {
    return malloc(size);
}

static void kvs_existing_free(void *ptr) {
    free(ptr);
}

static void *kvs_jemalloc_alloc(size_t size) {
    if (kvs_jemalloc_open() != 0) return NULL;
    kvs_alloc_hdr_t *hdr = (kvs_alloc_hdr_t *)g_je_malloc_fn(sizeof(kvs_alloc_hdr_t) + size);
    if (!hdr) return NULL;
    hdr->magic = KVS_HDR_MAGIC;
    hdr->mode = (uint16_t)KVS_MEM_JEMALLOC;
    hdr->class_idx = 0xFFFFu;
    hdr->size = (uint64_t)size;
    hdr->owner = NULL;
    kvs_stats_alloc(size, 0, 0, 1);
    return (void *)(hdr + 1);
}

static void kvs_jemalloc_free(void *ptr) {
    if (!ptr) return;
    kvs_alloc_hdr_t *hdr = ((kvs_alloc_hdr_t *)ptr) - 1;
    if (hdr->magic == KVS_HDR_MAGIC && hdr->mode == KVS_MEM_JEMALLOC && g_je_free_fn) {
        kvs_stats_free((size_t)hdr->size, 1);
        g_je_free_fn(hdr);
        return;
    }
    free(ptr);
}

int kvs_mem_init(kvs_mem_mode_t mode) {
    memset(&g_stats, 0, sizeof(g_stats));
    memset(&g_pool, 0, sizeof(g_pool));
    g_mode = mode;
    if (g_mode == KVS_MEM_JEMALLOC && kvs_jemalloc_open() != 0) {
        return -1;
    }
    return 0;
}

void kvs_mem_fini(void) {
    for (int i = 0; i < KVS_POOL_SMALL_CLASSES; ++i) {
        kvs_small_page_t *page = g_pool.small_pages[i];
        while (page) {
            kvs_small_page_t *next = page->next;
            free(page->memory);
            free(page);
            page = next;
        }
        g_pool.small_pages[i] = NULL;
    }

    kvs_large_node_t *ln = g_pool.large_list;
    while (ln) {
        kvs_large_node_t *next = ln->next;
        kvs_alloc_hdr_t *hdr = ((kvs_alloc_hdr_t *)ln->user_ptr) - 1;
        free(hdr);
        free(ln);
        ln = next;
    }
    g_pool.large_list = NULL;
    kvs_jemalloc_close();
}

int kvs_mem_set_mode(kvs_mem_mode_t mode) {
    if (g_stats.bytes_in_use != 0) return -1;
    kvs_mem_fini();
    return kvs_mem_init(mode);
}

kvs_mem_mode_t kvs_mem_get_mode(void) {
    return g_mode;
}

const char *kvs_mem_mode_name(kvs_mem_mode_t mode) {
    switch (mode) {
        case KVS_MEM_EXISTING: return "existing";
        case KVS_MEM_JEMALLOC: return "jemalloc";
        case KVS_MEM_POOL:     return "pool";
        default: return "unknown";
    }
}

int kvs_mem_parse_mode(const char *name, kvs_mem_mode_t *mode_out) {
    if (!name || !mode_out) return -1;
    if (strcmp(name, "existing") == 0) { *mode_out = KVS_MEM_EXISTING; return 0; }
    if (strcmp(name, "jemalloc") == 0) { *mode_out = KVS_MEM_JEMALLOC; return 0; }
    if (strcmp(name, "pool") == 0)     { *mode_out = KVS_MEM_POOL; return 0; }
    return -1;
}

void kvs_mem_get_stats(kvs_mem_stats_t *out) {
    if (out) *out = g_stats;
}

void *kvs_malloc(size_t size) {
    if (size == 0) size = 1;

    switch (g_mode) {
        case KVS_MEM_EXISTING:
            return kvs_existing_alloc(size);
        case KVS_MEM_JEMALLOC:
            return kvs_jemalloc_alloc(size);
        case KVS_MEM_POOL: {
            int idx = kvs_small_class_index(size + sizeof(kvs_alloc_hdr_t));
            if (idx >= 0) return kvs_pool_small_alloc(size);
            return kvs_pool_large_alloc(size);
        }
        default:
            return malloc(size);
    }
}

void kvs_free(void *ptr) {
    if (!ptr) return;

    if (g_mode == KVS_MEM_EXISTING) {
        kvs_existing_free(ptr);
        return;
    }

    kvs_alloc_hdr_t *hdr = ((kvs_alloc_hdr_t *)ptr) - 1;
    if (hdr->magic != KVS_HDR_MAGIC) {
        free(ptr);
        return;
    }

    if (hdr->mode == KVS_MEM_JEMALLOC) {
        kvs_jemalloc_free(ptr);
        return;
    }

    if (hdr->mode == KVS_MEM_POOL) {
        if (hdr->class_idx == 0xFFFFu) kvs_pool_large_free(hdr);
        else kvs_pool_small_free(hdr);
        return;
    }

    free(ptr);
}
