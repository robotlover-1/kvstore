#include "kvstore.h"
#include <sys/mman.h>
#include <dlfcn.h>

#define KVS_MEM_LIBC 0
#define KVS_MEM_JEMALLOC 1
#define KVS_MEM_CUSTOM 2

#define SMALL_CLASS_COUNT 8
#define SMALL_MAX_SIZE 1024
#define CHUNK_MAGIC 0xC0DEC0DEu
#define LARGE_MAGIC 0xC0DEC0DFu

typedef struct small_chunk_s {
    uint32_t magic;
    uint16_t class_idx;
    uint16_t reserved;
    struct small_chunk_s *next;
} small_chunk_t;

typedef struct slab_page_s {
    void *mem;
    struct slab_page_s *next;
} slab_page_t;

typedef struct {
    size_t size;
    small_chunk_t *free_list;
    slab_page_t *pages;
} small_class_t;

typedef struct large_hdr_s {
    uint32_t magic;
    uint32_t reserved;
    size_t mapping_size;
} large_hdr_t;

typedef struct {
    int backend;
    int initialized;
    int jemalloc_ready;
    void *(*je_malloc)(size_t);
    void (*je_free)(void *);
    void *(*je_calloc)(size_t, size_t);
    void *(*je_realloc)(void *, size_t);
    pthread_mutex_t lock;
    small_class_t classes[SMALL_CLASS_COUNT];
} kvs_mem_state_t;

static kvs_mem_state_t g_mem = {
    .backend = KVS_MEM_LIBC,
    .initialized = 0,
    .jemalloc_ready = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .classes = {
        {32, NULL, NULL},
        {64, NULL, NULL},
        {128, NULL, NULL},
        {256, NULL, NULL},
        {384, NULL, NULL},
        {512, NULL, NULL},
        {768, NULL, NULL},
        {1024, NULL, NULL},
    }
};

static int class_index_for(size_t sz) {
    for (int i = 0; i < SMALL_CLASS_COUNT; ++i) {
        if (sz <= g_mem.classes[i].size) return i;
    }
    return -1;
}

static int try_load_jemalloc(void) {
    if (g_mem.jemalloc_ready) return 0;
    const char *cands[] = {
        "libjemalloc.so.2",
        "libjemalloc.so.1",
        "libjemalloc.so",
        NULL
    };
    for (int i = 0; cands[i]; ++i) {
        void *h = dlopen(cands[i], RTLD_NOW | RTLD_LOCAL);
        if (!h) continue;
        g_mem.je_malloc = (void *(*)(size_t))dlsym(h, "malloc");
        g_mem.je_free = (void (*)(void *))dlsym(h, "free");
        g_mem.je_calloc = (void *(*)(size_t, size_t))dlsym(h, "calloc");
        g_mem.je_realloc = (void *(*)(void *, size_t))dlsym(h, "realloc");
        if (g_mem.je_malloc && g_mem.je_free && g_mem.je_calloc && g_mem.je_realloc) {
            g_mem.jemalloc_ready = 1;
            return 0;
        }
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
    page->next = g_mem.classes[class_idx].pages;
    g_mem.classes[class_idx].pages = page;

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

static void *custom_malloc(size_t size) {
    if (size == 0) size = 1;
    if (size <= SMALL_MAX_SIZE) {
        int idx = class_index_for(size);
        if (idx < 0) return NULL;
        pthread_mutex_lock(&g_mem.lock);
        if (!g_mem.classes[idx].free_list && slab_grow_locked(idx) != 0) {
            pthread_mutex_unlock(&g_mem.lock);
            return NULL;
        }
        small_chunk_t *chunk = g_mem.classes[idx].free_list;
        g_mem.classes[idx].free_list = chunk->next;
        pthread_mutex_unlock(&g_mem.lock);
        chunk->next = NULL;
        return (void *)(chunk + 1);
    }

    size_t total = sizeof(large_hdr_t) + size;
    size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
    size_t rounded = (total + pagesz - 1) / pagesz * pagesz;
    large_hdr_t *hdr = (large_hdr_t *)mmap(NULL, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (hdr == MAP_FAILED) return NULL;
    hdr->magic = LARGE_MAGIC;
    hdr->mapping_size = rounded;
    return (void *)(hdr + 1);
}

static void custom_free(void *ptr) {
    if (!ptr) return;
    small_chunk_t *chunk = ((small_chunk_t *)ptr) - 1;
    if (chunk->magic == CHUNK_MAGIC) {
        int idx = chunk->class_idx;
        pthread_mutex_lock(&g_mem.lock);
        chunk->next = g_mem.classes[idx].free_list;
        g_mem.classes[idx].free_list = chunk;
        pthread_mutex_unlock(&g_mem.lock);
        return;
    }
    large_hdr_t *hdr = ((large_hdr_t *)ptr) - 1;
    if (hdr->magic == LARGE_MAGIC) {
        munmap(hdr, hdr->mapping_size);
        return;
    }
    free(ptr);
}

static void *backend_malloc(size_t size) {
    switch (g_mem.backend) {
        case KVS_MEM_JEMALLOC:
            if (g_mem.jemalloc_ready) return g_mem.je_malloc(size);
            return malloc(size);
        case KVS_MEM_CUSTOM:
            return custom_malloc(size);
        default:
            return malloc(size);
    }
}

static void backend_free(void *ptr) {
    if (!ptr) return;
    switch (g_mem.backend) {
        case KVS_MEM_JEMALLOC:
            if (g_mem.jemalloc_ready) g_mem.je_free(ptr); else free(ptr);
            return;
        case KVS_MEM_CUSTOM:
            custom_free(ptr);
            return;
        default:
            free(ptr);
            return;
    }
}

static void *backend_calloc(size_t n, size_t size) {
    size_t total = n * size;
    void *p = backend_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

static void *backend_realloc(void *ptr, size_t size) {
    if (!ptr) return backend_malloc(size);
    if (size == 0) { backend_free(ptr); return NULL; }
    switch (g_mem.backend) {
        case KVS_MEM_JEMALLOC:
            if (g_mem.jemalloc_ready) return g_mem.je_realloc(ptr, size);
            return realloc(ptr, size);
        case KVS_MEM_CUSTOM: {
            small_chunk_t *chunk = ((small_chunk_t *)ptr) - 1;
            size_t oldsz = 0;
            if (chunk->magic == CHUNK_MAGIC) oldsz = g_mem.classes[chunk->class_idx].size;
            else {
                large_hdr_t *hdr = ((large_hdr_t *)ptr) - 1;
                if (hdr->magic == LARGE_MAGIC) oldsz = hdr->mapping_size - sizeof(large_hdr_t);
            }
            void *np = backend_malloc(size);
            if (!np) return NULL;
            memcpy(np, ptr, oldsz < size ? oldsz : size);
            backend_free(ptr);
            return np;
        }
        default:
            return realloc(ptr, size);
    }
}

int kvs_mem_init(const char *backend_name) {
    int backend = backend_from_name(backend_name ? backend_name : getenv("KVS_MEM_BACKEND"));
    if (backend < 0) return -1;
    if (backend == KVS_MEM_JEMALLOC && try_load_jemalloc() != 0) return -1;
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

void *kvs_malloc(size_t size) {
    if (!g_mem.initialized) kvs_mem_init(NULL);
    return backend_malloc(size);
}

void *kvs_calloc(size_t n, size_t size) {
    if (!g_mem.initialized) kvs_mem_init(NULL);
    return backend_calloc(n, size);
}

void *kvs_realloc(void *ptr, size_t size) {
    if (!g_mem.initialized) kvs_mem_init(NULL);
    return backend_realloc(ptr, size);
}

void kvs_free(void *ptr) {
    if (!g_mem.initialized) kvs_mem_init(NULL);
    backend_free(ptr);
}
