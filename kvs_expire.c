#include "kvstore.h"

kvs_expire_table_t global_expire = {0};

static unsigned int expire_hash(const char *key, int engine) {
    unsigned int h = 2166136261u;
    while (*key) { h ^= (unsigned char)(*key++); h *= 16777619u; }
    h ^= (unsigned int)engine;
    return h % KVS_EXPIRE_BUCKETS;
}

long long kvs_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int kvs_expire_create(kvs_expire_table_t *tab) {
    if (!tab) return -1;
    tab->buckets = calloc(KVS_EXPIRE_BUCKETS, sizeof(kvs_expire_item_t*));
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
    kvs_expire_item_t *cur = tab->buckets[idx];
    while (cur) {
        if (cur->engine == engine && strcmp(cur->key, key) == 0) return cur;
        cur = cur->next;
    }
    return NULL;
}

int kvs_expire_set(kvs_expire_table_t *tab, int engine, const char *key, long long ttl_ms) {
    if (!tab || !key || ttl_ms < 0) return -1;
    long long expire_at = kvs_now_ms() + ttl_ms;
    kvs_expire_item_t *item = expire_find(tab, engine, key);
    if (item) {
        item->expire_at_ms = expire_at;
        return 0;
    }
    item = kvs_malloc(sizeof(*item));
    if (!item) return -1;
    memset(item, 0, sizeof(*item));
    item->key = kvs_malloc(strlen(key) + 1);
    if (!item->key) { kvs_free(item); return -1; }
    strcpy(item->key, key);
    item->engine = engine;
    item->expire_at_ms = expire_at;
    unsigned int idx = expire_hash(key, engine);
    item->next = tab->buckets[idx];
    tab->buckets[idx] = item;
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

int kvs_expire_persist(kvs_expire_table_t *tab, int engine, const char *key) {
    return kvs_expire_del(tab, engine, key);
}

int kvs_expire_is_expired(kvs_expire_table_t *tab, int engine, const char *key) {
    kvs_expire_item_t *item = expire_find(tab, engine, key);
    return item ? (kvs_now_ms() >= item->expire_at_ms) : 0;
}

long long kvs_expire_ttl(kvs_expire_table_t *tab, int engine, const char *key) {
    kvs_expire_item_t *item = expire_find(tab, engine, key);
    if (!item) return -1;
    long long now = kvs_now_ms();
    if (now >= item->expire_at_ms) return -2;
    long long ms = item->expire_at_ms - now;
    return (ms + 999) / 1000;
}
