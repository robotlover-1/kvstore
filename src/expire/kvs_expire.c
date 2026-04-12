#include "kvstore/kvstore.h"

kvs_expire_table_t global_expire = {0};

static unsigned int expire_hash(const char *key, int engine) {
    unsigned int h = 2166136261u;
    while (*key) {
        h ^= (unsigned char)(*key++);
        h *= 16777619u;
    }
    h ^= (unsigned int)engine;
    h *= 16777619u;
    return h % KVS_EXPIRE_BUCKETS;
}

long long kvs_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int kvs_expire_create(kvs_expire_table_t *tab) {
    if (!tab) return -1;
    tab->buckets = (kvs_expire_item_t **)kvs_calloc(KVS_EXPIRE_BUCKETS, sizeof(kvs_expire_item_t *));
    if (!tab->buckets) return -1;
    tab->size = KVS_EXPIRE_BUCKETS;
    return 0;
}

void kvs_expire_destroy(kvs_expire_table_t *tab) {
    if (!tab || !tab->buckets) return;
    for (int i = 0; i < tab->size; ++i) {
        kvs_expire_item_t *cur = tab->buckets[i];
        while (cur) {
            kvs_expire_item_t *next = cur->next;
            kvs_free(cur->key);
            kvs_free(cur);
            cur = next;
        }
    }
    kvs_free(tab->buckets);
    tab->buckets = NULL;
    tab->size = 0;
}

static kvs_expire_item_t *expire_find(kvs_expire_table_t *tab, int engine, const char *key) {
    if (!tab || !tab->buckets || !key) return NULL;
    unsigned int idx = expire_hash(key, engine);
    for (kvs_expire_item_t *cur = tab->buckets[idx]; cur; cur = cur->next) {
        if (cur->engine == engine && strcmp(cur->key, key) == 0) return cur;
    }
    return NULL;
}

int kvs_expire_set(kvs_expire_table_t *tab, int engine, const char *key, long long ttl_ms) {
    if (!tab || !tab->buckets || !key || ttl_ms <= 0) return -1;
    long long expire_at = kvs_now_ms() + ttl_ms;
    kvs_expire_item_t *node = expire_find(tab, engine, key);
    if (node) { node->expire_at_ms = expire_at; return 0; }
    node = (kvs_expire_item_t*)kvs_malloc(sizeof(*node));
    if (!node) return -1;
    memset(node, 0, sizeof(*node));
    node->key = (char *)kvs_malloc(strlen(key) + 1);
    if (!node->key) { kvs_free(node); return -1; }
    strcpy(node->key, key);
    node->engine = engine;
    node->expire_at_ms = expire_at;
    unsigned int idx = expire_hash(key, engine);
    node->next = tab->buckets[idx];
    tab->buckets[idx] = node;
    return 0;
}

int kvs_expire_del(kvs_expire_table_t *tab, int engine, const char *key) {
    if (!tab || !tab->buckets || !key) return -1;
    unsigned int idx = expire_hash(key, engine);
    kvs_expire_item_t *cur = tab->buckets[idx], *prev = NULL;
    while (cur) {
        if (cur->engine == engine && strcmp(cur->key, key) == 0) {
            if (prev) prev->next = cur->next; else tab->buckets[idx] = cur->next;
            kvs_free(cur->key); kvs_free(cur); return 0;
        }
        prev = cur; cur = cur->next;
    }
    return 1;
}

int kvs_expire_persist(kvs_expire_table_t *tab, int engine, const char *key) { return kvs_expire_del(tab, engine, key); }

int kvs_expire_is_expired(kvs_expire_table_t *tab, int engine, const char *key) {
    kvs_expire_item_t *node = expire_find(tab, engine, key);
    if (!node) return 0;
    return kvs_now_ms() >= node->expire_at_ms;
}

long long kvs_expire_ttl(kvs_expire_table_t *tab, int engine, const char *key) {
    kvs_expire_item_t *node = expire_find(tab, engine, key);
    if (!node) return -1;
    long long now = kvs_now_ms();
    if (now >= node->expire_at_ms) return -2;
    long long remain_ms = node->expire_at_ms - now;
    long long s = remain_ms / 1000;
    if (remain_ms % 1000) s += 1;
    return s;
}

static int engine_del(int engine, const char *key);

int kvs_active_expire_cycle(int budget) {
    if (budget <= 0 || !global_expire.buckets) return 0;
    int removed = 0;
    long long now = kvs_now_ms();
    for (int i = 0; i < global_expire.size && removed < budget; ++i) {
        kvs_expire_item_t *cur = global_expire.buckets[i], *prev = NULL;
        while (cur && removed < budget) {
            if (now >= cur->expire_at_ms) {
                engine_del(cur->engine, cur->key);
                if (prev) prev->next = cur->next; else global_expire.buckets[i] = cur->next;
                kvs_expire_item_t *dead = cur; cur = cur->next;
                kvs_free(dead->key); kvs_free(dead); removed++; continue;
            }
            prev = cur; cur = cur->next;
        }
    }
    return removed;
}

static int engine_del(int engine, const char *key) {
    switch (engine) {
        case KVS_ENGINE_ARRAY: return kvs_array_del(&global_array, (char *)key);
        case KVS_ENGINE_RBTREE: return kvs_rbtree_del(&global_rbtree, (char *)key);
        case KVS_ENGINE_HASH: return kvs_hash_del(&global_hash, (char *)key);
        case KVS_ENGINE_SKIPTABLE: return kvs_skiptable_del(&global_skiptable, (char *)key);
        default: return -1;
    }
}
