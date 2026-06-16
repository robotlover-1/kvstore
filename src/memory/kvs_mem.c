#include "kvstore/kvstore.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <limits.h>

#define KVS_MEM_LIBC 0
#define KVS_MEM_JEMALLOC 1
#define KVS_MEM_CUSTOM 2

#define SMALL_CLASS_COUNT 17
#define SMALL_MAX_SIZE 1024
#define CHUNK_MAGIC 0xC0DEC0DEu
#define LARGE_MAGIC 0xC0DEC0DFu
#define FALLBACK_MAGIC 0xC0DEC0E0u
#define KVS_JEMALLOC_ACTIVE_ENV "KVS_MEM_JEMALLOC_ACTIVE"

typedef struct small_chunk_s {
    uint32_t magic;
    uint16_t class_idx;
    uint16_t reserved;
    uint32_t request_size;
    struct small_chunk_s *next;
} small_chunk_t;

typedef struct slab_page_s {
    void *mem;
    size_t size;
    size_t chunks_total;
    size_t chunks_in_use;
    struct slab_page_s *next;
} slab_page_t;

typedef struct {
    size_t size;
    small_chunk_t *free_list;
    slab_page_t *pages;
    size_t total_chunks;
    size_t page_count;
    size_t page_bytes;
} small_class_t;

typedef struct large_hdr_s {
    uint32_t magic;
    uint32_t reserved;
    size_t request_size;
    size_t mapping_size;
} large_hdr_t;

typedef struct fallback_hdr_s {
    uint32_t magic;
    uint32_t reserved;
    size_t request_size;
    size_t alloc_size;
} fallback_hdr_t;

typedef struct {
    int backend;
    int initialized;
    pthread_mutex_t lock;
    unsigned long long alloc_calls;
    unsigned long long free_calls;
    unsigned long long calloc_calls;
    unsigned long long realloc_calls;
    unsigned long long small_alloc_calls;
    unsigned long long small_free_calls;
    unsigned long long large_alloc_calls;
    unsigned long long large_free_calls;
    unsigned long long fallback_alloc_calls;
    unsigned long long fallback_free_calls;
    unsigned long long current_small_inuse;
    unsigned long long peak_small_inuse;
    unsigned long long current_large_inuse_bytes;
    unsigned long long peak_large_inuse_bytes;
    unsigned long long current_fallback_inuse_bytes;
    unsigned long long peak_fallback_inuse_bytes;
    unsigned long long total_small_page_bytes;
    unsigned long long total_large_map_bytes;
    unsigned long long active_large_map_bytes;
    unsigned long long peak_active_large_map_bytes;
    unsigned long long current_requested_bytes;
    unsigned long long current_allocated_bytes;
    small_class_t classes[SMALL_CLASS_COUNT];
} kvs_mem_state_t;

static kvs_mem_state_t g_mem = {
    .backend = KVS_MEM_LIBC,
    .initialized = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .classes = {
        {16,   NULL, NULL, 0, 0, 0},
        {24,   NULL, NULL, 0, 0, 0},
        {32,   NULL, NULL, 0, 0, 0},
        {40,   NULL, NULL, 0, 0, 0},
        {56,   NULL, NULL, 0, 0, 0},
        {72,   NULL, NULL, 0, 0, 0},
        {96,   NULL, NULL, 0, 0, 0},
        {128,  NULL, NULL, 0, 0, 0},
        {160,  NULL, NULL, 0, 0, 0},
        {200,  NULL, NULL, 0, 0, 0},
        {256,  NULL, NULL, 0, 0, 0},
        {320,  NULL, NULL, 0, 0, 0},
        {400,  NULL, NULL, 0, 0, 0},
        {512,  NULL, NULL, 0, 0, 0},
        {640,  NULL, NULL, 0, 0, 0},
        {800,  NULL, NULL, 0, 0, 0},
        {1024, NULL, NULL, 0, 0, 0},
    }
};

static int class_index_for(size_t sz) {
    for (int i = 0; i < SMALL_CLASS_COUNT; ++i) {
        if (sz <= g_mem.classes[i].size) return i;
    }
    return -1;
}

static int backend_from_name(const char *name) {
    if (!name || !*name) return KVS_MEM_LIBC;
    if (!strcmp(name, "libc") || !strcmp(name, "system") || !strcmp(name, "default")) return KVS_MEM_LIBC;
    if (!strcmp(name, "jemalloc")) return KVS_MEM_JEMALLOC;
    if (!strcmp(name, "custom") || !strcmp(name, "pool")) return KVS_MEM_CUSTOM;
    return -1;
}

static const char *find_jemalloc_path(void) {
    static const char *cands[] = {
        "/lib/x86_64-linux-gnu/libjemalloc.so.2",
        "/usr/lib/x86_64-linux-gnu/libjemalloc.so.2",
        "/usr/local/lib/libjemalloc.so.2",
        "/lib64/libjemalloc.so.2",
        "/usr/lib64/libjemalloc.so.2",
        "libjemalloc.so.2",
        "libjemalloc.so.1",
        "libjemalloc.so",
        NULL
    };
    struct stat st;
    for (int i = 0; cands[i]; ++i) {
        if (cands[i][0] != '/') return cands[i];
        if (stat(cands[i], &st) == 0 && S_ISREG(st.st_mode)) return cands[i];
    }
    return NULL;
}

static int ensure_jemalloc_process_image(const char *argv0, char **argv) {
    const char *active = getenv(KVS_JEMALLOC_ACTIVE_ENV);
    if (active && !strcmp(active, "1")) return 0;

    const char *jemalloc_path = find_jemalloc_path();
    if (!jemalloc_path) return -1;

    const char *old_preload = getenv("LD_PRELOAD");
    char preload[4096];
    if (old_preload && *old_preload) {
        if (strstr(old_preload, jemalloc_path)) {
            snprintf(preload, sizeof(preload), "%s", old_preload);
        } else {
            snprintf(preload, sizeof(preload), "%s:%s", jemalloc_path, old_preload);
        }
    } else {
        snprintf(preload, sizeof(preload), "%s", jemalloc_path);
    }

    if (setenv("LD_PRELOAD", preload, 1) != 0) return -1;
    if (setenv(KVS_JEMALLOC_ACTIVE_ENV, "1", 1) != 0) return -1;

    execvp(argv0, argv);
    return -1;
}

static void update_peak_ull(unsigned long long *peak, unsigned long long value) {
    if (value > *peak) *peak = value;
}

static unsigned long long payload_from_mapping_size(size_t mapping_size) {
    return mapping_size > sizeof(large_hdr_t) ? (unsigned long long)(mapping_size - sizeof(large_hdr_t)) : 0ULL;
}

static void account_live_alloc_locked(size_t requested, size_t allocated) {
    g_mem.current_requested_bytes += (unsigned long long)requested;
    g_mem.current_allocated_bytes += (unsigned long long)allocated;
}

static void account_live_free_locked(size_t requested, size_t allocated) {
    if (g_mem.current_requested_bytes >= (unsigned long long)requested) g_mem.current_requested_bytes -= (unsigned long long)requested;
    else g_mem.current_requested_bytes = 0;
    if (g_mem.current_allocated_bytes >= (unsigned long long)allocated) g_mem.current_allocated_bytes -= (unsigned long long)allocated;
    else g_mem.current_allocated_bytes = 0;
}

static size_t count_free_chunks(small_chunk_t *head) {
    size_t n = 0;
    for (; head; head = head->next) n++;
    return n;
}

static void custom_account_fallback_alloc(size_t requested, size_t allocated) {
    pthread_mutex_lock(&g_mem.lock);
    g_mem.fallback_alloc_calls++;
    g_mem.current_fallback_inuse_bytes += requested;
    update_peak_ull(&g_mem.peak_fallback_inuse_bytes, g_mem.current_fallback_inuse_bytes);
    account_live_alloc_locked(requested, allocated);
    pthread_mutex_unlock(&g_mem.lock);
}

static void custom_account_fallback_free(size_t requested, size_t allocated) {
    pthread_mutex_lock(&g_mem.lock);
    g_mem.fallback_free_calls++;
    if (g_mem.current_fallback_inuse_bytes >= requested) g_mem.current_fallback_inuse_bytes -= requested;
    else g_mem.current_fallback_inuse_bytes = 0;
    account_live_free_locked(requested, allocated);
    pthread_mutex_unlock(&g_mem.lock);
}

static void *fallback_malloc(size_t size) {
    if (size > SIZE_MAX - sizeof(fallback_hdr_t)) return NULL;
    fallback_hdr_t *hdr = (fallback_hdr_t *)malloc(sizeof(*hdr) + size);
    if (!hdr) return NULL;
    hdr->magic = FALLBACK_MAGIC;
    hdr->request_size = size;
    hdr->alloc_size = sizeof(*hdr) + size;
    custom_account_fallback_alloc(size, size);
    return (void *)(hdr + 1);
}

static int slab_grow_locked(int class_idx) {
    size_t chunk_total = sizeof(small_chunk_t) + g_mem.classes[class_idx].size;
    size_t page_size = 65536;
    if (chunk_total > page_size / 2) page_size = 262144;
    int count = (int)(page_size / chunk_total);
    if (count < 16) count = 16;
    size_t alloc_size = chunk_total * (size_t)count;

    void *mem = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return -1;

    slab_page_t *page = (slab_page_t *)malloc(sizeof(*page));
    if (!page) {
        munmap(mem, alloc_size);
        return -1;
    }
    page->mem = mem;
    page->size = alloc_size;
    page->chunks_total = (size_t)count;
    page->chunks_in_use = 0;
    page->next = g_mem.classes[class_idx].pages;
    g_mem.classes[class_idx].pages = page;
    g_mem.classes[class_idx].page_count++;
    g_mem.classes[class_idx].page_bytes += alloc_size;
    g_mem.classes[class_idx].total_chunks += (size_t)count;
    g_mem.total_small_page_bytes += alloc_size;

    unsigned char *p = (unsigned char *)mem;
    for (int i = 0; i < count; ++i) {
        small_chunk_t *chunk = (small_chunk_t *)(p + i * chunk_total);
        chunk->magic = CHUNK_MAGIC;
        chunk->class_idx = (uint16_t)class_idx;
        chunk->next = g_mem.classes[class_idx].free_list;
        g_mem.classes[class_idx].free_list = chunk;
    }
    return 0;
}

static slab_page_t *find_page_for_chunk(small_class_t *cls, small_chunk_t *chunk);

static void *custom_malloc(size_t size) {
    if (size == 0) size = 1;
    if (size <= SMALL_MAX_SIZE) {
        int idx = class_index_for(size);
        if (idx < 0) return fallback_malloc(size);
        pthread_mutex_lock(&g_mem.lock);
        if (!g_mem.classes[idx].free_list && slab_grow_locked(idx) != 0) {
            pthread_mutex_unlock(&g_mem.lock);
            return fallback_malloc(size);
        }
        small_chunk_t *chunk = g_mem.classes[idx].free_list;
        if (!chunk) {
            pthread_mutex_unlock(&g_mem.lock);
            return fallback_malloc(size);
        }
        g_mem.classes[idx].free_list = chunk->next;
        chunk->next = NULL;
        /* 追踪所属 page 的 chunks_in_use */
        slab_page_t *pg = find_page_for_chunk(&g_mem.classes[idx], chunk);
        if (pg) pg->chunks_in_use++;
        chunk->request_size = (uint32_t)size;
        g_mem.small_alloc_calls++;
        g_mem.current_small_inuse += g_mem.classes[idx].size;
        update_peak_ull(&g_mem.peak_small_inuse, g_mem.current_small_inuse);
        account_live_alloc_locked(size, g_mem.classes[idx].size);
        pthread_mutex_unlock(&g_mem.lock);
        return (void *)(chunk + 1);
    }

    size_t total = sizeof(large_hdr_t) + size;
    size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
    if (pagesz == 0) pagesz = 4096;
    if (total < size) return fallback_malloc(size);
    size_t rounded = (total + pagesz - 1) / pagesz * pagesz;
    large_hdr_t *hdr = (large_hdr_t *)mmap(NULL, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (hdr == MAP_FAILED) return fallback_malloc(size);
    hdr->magic = LARGE_MAGIC;
    hdr->request_size = size;
    hdr->mapping_size = rounded;

    pthread_mutex_lock(&g_mem.lock);
    g_mem.large_alloc_calls++;
    g_mem.current_large_inuse_bytes += size;
    g_mem.active_large_map_bytes += rounded;
    g_mem.total_large_map_bytes += rounded;
    update_peak_ull(&g_mem.peak_large_inuse_bytes, g_mem.current_large_inuse_bytes);
    update_peak_ull(&g_mem.peak_active_large_map_bytes, g_mem.active_large_map_bytes);
    account_live_alloc_locked(size, payload_from_mapping_size(rounded));
    pthread_mutex_unlock(&g_mem.lock);

    return (void *)(hdr + 1);
}

static size_t custom_ptr_size(const void *ptr, int *kind_out) {
    if (kind_out) *kind_out = 0;
    if (!ptr) return 0;

    const small_chunk_t *chunk = ((const small_chunk_t *)ptr) - 1;
    if (chunk->magic == CHUNK_MAGIC && chunk->class_idx < SMALL_CLASS_COUNT) {
        if (kind_out) *kind_out = 1;
        return chunk->request_size;
    }

    const large_hdr_t *lhdr = ((const large_hdr_t *)ptr) - 1;
    if (lhdr->magic == LARGE_MAGIC) {
        if (kind_out) *kind_out = 2;
        return lhdr->request_size;
    }

    const fallback_hdr_t *fhdr = ((const fallback_hdr_t *)ptr) - 1;
    if (fhdr->magic == FALLBACK_MAGIC) {
        if (kind_out) *kind_out = 3;
        return fhdr->request_size;
    }

    return 0;
}

static slab_page_t *find_page_for_chunk(small_class_t *cls, small_chunk_t *chunk) {
    unsigned char *c = (unsigned char *)chunk;
    for (slab_page_t *pg = cls->pages; pg; pg = pg->next) {
        if (c >= (unsigned char *)pg->mem &&
            c <  (unsigned char *)pg->mem + pg->size)
            return pg;
    }
    return NULL;
}

static void try_reclaim_page_locked(small_class_t *cls, slab_page_t *target) {
    /* 阈值保护：至少保留 2 页的 free chunk 缓冲，防止颠簸 */
    size_t free_chunks = count_free_chunks(cls->free_list);
    if (free_chunks < target->chunks_total * 2) return;

    /* 从 free_list 中移除属于 target 页的所有 chunk */
    unsigned char *page_start = (unsigned char *)target->mem;
    unsigned char *page_end   = page_start + target->size;
    small_chunk_t **prev = &cls->free_list;
    while (*prev) {
        unsigned char *c = (unsigned char *)(*prev);
        if (c >= page_start && c < page_end) {
            *prev = (*prev)->next;
            cls->total_chunks--;
        } else {
            prev = &(*prev)->next;
        }
    }

    /* 从 page 链表中移除 */
    slab_page_t **pp = &cls->pages;
    while (*pp) {
        if (*pp == target) { *pp = target->next; break; }
        pp = &(*pp)->next;
    }
    cls->page_count--;
    cls->page_bytes -= target->size;

    g_mem.total_small_page_bytes -= target->size;
    munmap(target->mem, target->size);
    free(target);
}

static void custom_free(void *ptr) {
    if (!ptr) return;
    small_chunk_t *chunk = ((small_chunk_t *)ptr) - 1;
    if (chunk->magic == CHUNK_MAGIC && chunk->class_idx < SMALL_CLASS_COUNT) {
        int idx = chunk->class_idx;
        size_t requested = chunk->request_size;
        pthread_mutex_lock(&g_mem.lock);
        chunk->next = g_mem.classes[idx].free_list;
        g_mem.classes[idx].free_list = chunk;
        g_mem.small_free_calls++;
        if (g_mem.current_small_inuse >= g_mem.classes[idx].size) g_mem.current_small_inuse -= g_mem.classes[idx].size;
        else g_mem.current_small_inuse = 0;
        account_live_free_locked(requested, g_mem.classes[idx].size);
        /* 追踪 page 的 chunks_in_use，完全空闲时尝试回收 */
        slab_page_t *pg = find_page_for_chunk(&g_mem.classes[idx], chunk);
        if (pg && pg->chunks_in_use > 0) {
            pg->chunks_in_use--;
            if (pg->chunks_in_use == 0) {
                try_reclaim_page_locked(&g_mem.classes[idx], pg);
            }
        }
        pthread_mutex_unlock(&g_mem.lock);
        return;
    }
    large_hdr_t *hdr = ((large_hdr_t *)ptr) - 1;
    if (hdr->magic == LARGE_MAGIC) {
        pthread_mutex_lock(&g_mem.lock);
        g_mem.large_free_calls++;
        if (g_mem.current_large_inuse_bytes >= hdr->request_size) g_mem.current_large_inuse_bytes -= hdr->request_size;
        else g_mem.current_large_inuse_bytes = 0;
        if (g_mem.active_large_map_bytes >= hdr->mapping_size) g_mem.active_large_map_bytes -= hdr->mapping_size;
        else g_mem.active_large_map_bytes = 0;
        account_live_free_locked(hdr->request_size, payload_from_mapping_size(hdr->mapping_size));
        pthread_mutex_unlock(&g_mem.lock);
        munmap(hdr, hdr->mapping_size);
        return;
    }
    fallback_hdr_t *fhdr = ((fallback_hdr_t *)ptr) - 1;
    if (fhdr->magic == FALLBACK_MAGIC) {
        size_t req = fhdr->request_size;
        custom_account_fallback_free(req, req);
        free(fhdr);
        return;
    }
    free(ptr);
}

static void *backend_malloc(size_t size) {
    switch (g_mem.backend) {
        case KVS_MEM_CUSTOM:
            return custom_malloc(size);
        case KVS_MEM_JEMALLOC:
        case KVS_MEM_LIBC:
        default:
            return malloc(size);
    }
}

static void backend_free(void *ptr) {
    if (!ptr) return;
    switch (g_mem.backend) {
        case KVS_MEM_CUSTOM:
            custom_free(ptr);
            return;
        case KVS_MEM_JEMALLOC:
        case KVS_MEM_LIBC:
        default:
            free(ptr);
            return;
    }
}

static void *backend_calloc(size_t n, size_t size) {
    if (n != 0 && size > SIZE_MAX / n) return NULL;
    size_t total = n * size;
    void *p = backend_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

static void *backend_realloc(void *ptr, size_t size) {
    if (!ptr) return backend_malloc(size);
    if (size == 0) {
        backend_free(ptr);
        return NULL;
    }
    switch (g_mem.backend) {
        case KVS_MEM_CUSTOM: {
            int kind = 0;
            size_t oldsz = custom_ptr_size(ptr, &kind);
            if (kind == 0) return realloc(ptr, size);
            if (size <= oldsz && kind == 1) return ptr;
            if (size <= oldsz && kind == 2) return ptr;
            if (size <= oldsz && kind == 3) return ptr;
            void *np = backend_malloc(size);
            if (!np) return NULL;
            memcpy(np, ptr, oldsz < size ? oldsz : size);
            backend_free(ptr);
            return np;
        }
        case KVS_MEM_JEMALLOC:
        case KVS_MEM_LIBC:
        default:
            return realloc(ptr, size);
    }
}

int kvs_mem_prepare_process(const char *backend_name, char *argv0, char **argv) {
    int backend = backend_from_name(backend_name ? backend_name : getenv("KVS_MEM_BACKEND"));
    if (backend < 0) return -1;
    if (backend != KVS_MEM_JEMALLOC) return 0;
    return ensure_jemalloc_process_image(argv0, argv);
}

int kvs_mem_init(const char *backend_name) {
    int backend = backend_from_name(backend_name ? backend_name : getenv("KVS_MEM_BACKEND"));
    if (backend < 0) return -1;
    g_mem.backend = backend;
    g_mem.initialized = 1;
    return 0;
}

const char *kvs_mem_backend_name(void) {
    switch (g_mem.backend) {
        case KVS_MEM_JEMALLOC: return "jemalloc";
        case KVS_MEM_CUSTOM: return "custom";
        default: return "libc";
    }
}

int kvs_mem_get_stats(kvs_mem_stats_t *stats) {
    if (!stats) return -1;
    memset(stats, 0, sizeof(*stats));

    pthread_mutex_lock(&g_mem.lock);
    stats->backend_name = kvs_mem_backend_name();
    stats->backend_id = g_mem.backend;
    stats->initialized = g_mem.initialized;
    stats->small_max_size = SMALL_MAX_SIZE;
    stats->class_count = SMALL_CLASS_COUNT;
    stats->alloc_calls = g_mem.alloc_calls;
    stats->free_calls = g_mem.free_calls;
    stats->calloc_calls = g_mem.calloc_calls;
    stats->realloc_calls = g_mem.realloc_calls;
    stats->small_alloc_calls = g_mem.small_alloc_calls;
    stats->small_free_calls = g_mem.small_free_calls;
    stats->large_alloc_calls = g_mem.large_alloc_calls;
    stats->large_free_calls = g_mem.large_free_calls;
    stats->fallback_alloc_calls = g_mem.fallback_alloc_calls;
    stats->fallback_free_calls = g_mem.fallback_free_calls;
    stats->current_small_inuse = g_mem.current_small_inuse;
    stats->peak_small_inuse = g_mem.peak_small_inuse;
    stats->current_large_inuse_bytes = g_mem.current_large_inuse_bytes;
    stats->peak_large_inuse_bytes = g_mem.peak_large_inuse_bytes;
    stats->current_fallback_inuse_bytes = g_mem.current_fallback_inuse_bytes;
    stats->peak_fallback_inuse_bytes = g_mem.peak_fallback_inuse_bytes;
    stats->total_small_page_bytes = g_mem.total_small_page_bytes;
    stats->total_large_map_bytes = g_mem.total_large_map_bytes;
    stats->active_large_map_bytes = g_mem.active_large_map_bytes;
    stats->peak_active_large_map_bytes = g_mem.peak_active_large_map_bytes;
    stats->current_requested_bytes = g_mem.current_requested_bytes;
    stats->current_allocated_bytes = g_mem.current_allocated_bytes;
    stats->internal_fragment_bytes = g_mem.current_allocated_bytes >= g_mem.current_requested_bytes ? g_mem.current_allocated_bytes - g_mem.current_requested_bytes : 0ULL;
    stats->small_page_used_bytes = g_mem.current_small_inuse;
    stats->internal_fragment_ppm = g_mem.current_allocated_bytes ? (unsigned int)((stats->internal_fragment_bytes * 1000000ULL) / g_mem.current_allocated_bytes) : 0U;
    stats->page_utilization_ppm = g_mem.total_small_page_bytes ? (unsigned int)((g_mem.current_small_inuse * 1000000ULL) / g_mem.total_small_page_bytes) : 0U;
    for (size_t i = 0; i < SMALL_CLASS_COUNT; ++i) {
        stats->class_sizes[i] = g_mem.classes[i].size;
        stats->class_total_chunks[i] = g_mem.classes[i].total_chunks;
        stats->class_free_chunks[i] = count_free_chunks(g_mem.classes[i].free_list);
        stats->class_page_count[i] = g_mem.classes[i].page_count;
        stats->class_bytes_in_pages[i] = g_mem.classes[i].page_bytes;
    }
    pthread_mutex_unlock(&g_mem.lock);
    return 0;
}

void *kvs_malloc(size_t size) {
    if (!g_mem.initialized) kvs_mem_init(NULL);
    pthread_mutex_lock(&g_mem.lock);
    g_mem.alloc_calls++;
    pthread_mutex_unlock(&g_mem.lock);
    return backend_malloc(size);
}

void *kvs_calloc(size_t n, size_t size) {
    if (!g_mem.initialized) kvs_mem_init(NULL);
    pthread_mutex_lock(&g_mem.lock);
    g_mem.calloc_calls++;
    pthread_mutex_unlock(&g_mem.lock);
    return backend_calloc(n, size);
}

void *kvs_realloc(void *ptr, size_t size) {
    if (!g_mem.initialized) kvs_mem_init(NULL);
    pthread_mutex_lock(&g_mem.lock);
    g_mem.realloc_calls++;
    pthread_mutex_unlock(&g_mem.lock);
    return backend_realloc(ptr, size);
}

void kvs_free(void *ptr) {
    if (!g_mem.initialized) kvs_mem_init(NULL);
    pthread_mutex_lock(&g_mem.lock);
    g_mem.free_calls++;
    pthread_mutex_unlock(&g_mem.lock);
    backend_free(ptr);
}
