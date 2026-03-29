#include "kvstore.h"

kvs_expire_table_t global_expire = {0};

static uint32_t expire_hash_view(const kvs_blob_view_t *key, int engine) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < key->len; ++i) {
        h ^= key->data[i];
        h *= 16777619u;
    }
    h ^= (uint32_t)engine;
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
    tab->buckets = (kvs_expire_item_t **)calloc(KVS_EXPIRE_BUCKETS, sizeof(kvs_expire_item_t *));
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
            kvs_blob_free(&cur->key);
            free(cur);
            cur = next;
        }
    }
    free(tab->buckets);
    memset(tab, 0, sizeof(*tab));
}

static kvs_expire_item_t *expire_find(kvs_expire_table_t *tab, int engine, const kvs_blob_view_t *key) {
    if (!tab || !tab->buckets || !key) return NULL;
    uint32_t idx = expire_hash_view(key, engine);
    kvs_expire_item_t *cur = tab->buckets[idx];
    while (cur) {
        if (cur->engine == engine && kvs_blob_equal_view(&cur->key, key)) return cur;
        cur = cur->next;
    }
    return NULL;
}

int kvs_expire_set(kvs_expire_table_t *tab, int engine, const kvs_blob_view_t *key, long long ttl_ms) {
    kvs_expire_item_t *node;
    uint32_t idx;
    if (!tab || !tab->buckets || !key || ttl_ms <= 0) return -1;
    node = expire_find(tab, engine, key);
    if (node) {
        node->expire_at_ms = kvs_now_ms() + ttl_ms;
        return 0;
    }
    node = (kvs_expire_item_t *)calloc(1, sizeof(*node));
    if (!node) return -1;
    if (kvs_blob_dup(&node->key, key->data, key->len) != 0) {
        free(node);
        return -1;
    }
    node->engine = engine;
    node->expire_at_ms = kvs_now_ms() + ttl_ms;
    idx = expire_hash_view(key, engine);
    node->next = tab->buckets[idx];
    tab->buckets[idx] = node;
    return 0;
}

int kvs_expire_del(kvs_expire_table_t *tab, int engine, const kvs_blob_view_t *key) {
    kvs_expire_item_t *cur, *prev = NULL;
    uint32_t idx;
    if (!tab || !tab->buckets || !key) return -1;
    idx = expire_hash_view(key, engine);
    cur = tab->buckets[idx];
    while (cur) {
        if (cur->engine == engine && kvs_blob_equal_view(&cur->key, key)) {
            if (prev) prev->next = cur->next; else tab->buckets[idx] = cur->next;
            kvs_blob_free(&cur->key);
            free(cur);
            return 0;
        }
        prev = cur;
        cur = cur->next;
    }
    return 1;
}

int kvs_expire_persist(kvs_expire_table_t *tab, int engine, const kvs_blob_view_t *key) {
    return kvs_expire_del(tab, engine, key);
}

int kvs_expire_is_expired(kvs_expire_table_t *tab, int engine, const kvs_blob_view_t *key) {
    kvs_expire_item_t *node = expire_find(tab, engine, key);
    if (!node) return 0;
    return kvs_now_ms() >= node->expire_at_ms;
}

long long kvs_expire_ttl(kvs_expire_table_t *tab, int engine, const kvs_blob_view_t *key) {
    kvs_expire_item_t *node = expire_find(tab, engine, key);
    long long now, remain_ms;
    if (!node) return -1;
    now = kvs_now_ms();
    if (now >= node->expire_at_ms) return -2;
    remain_ms = node->expire_at_ms - now;
    return (remain_ms + 999) / 1000;
}
