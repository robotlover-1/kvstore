#include "kvstore.h"

kvs_hash_t global_hash = {0};

static uint32_t hash_bytes(const unsigned char *data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

static hashnode_t *create_node(const kvs_blob_view_t *key, const kvs_blob_view_t *value) {
    hashnode_t *n = (hashnode_t *)calloc(1, sizeof(hashnode_t));
    if (!n) return NULL;
    if (kvs_blob_dup(&n->key, key->data, key->len) != 0 || kvs_blob_dup(&n->value, value->data, value->len) != 0) {
        kvs_blob_free(&n->key);
        kvs_blob_free(&n->value);
        free(n);
        return NULL;
    }
    return n;
}

int kvs_hash_create(kvs_hash_t *hash) {
    if (!hash) return -1;
    hash->nodes = (hashnode_t **)calloc(MAX_TABLE_SIZE, sizeof(hashnode_t *));
    if (!hash->nodes) return -1;
    hash->max_slots = MAX_TABLE_SIZE;
    hash->count = 0;
    return 0;
}

void kvs_hash_destory(kvs_hash_t *hash) {
    if (!hash || !hash->nodes) return;
    for (int i = 0; i < hash->max_slots; ++i) {
        hashnode_t *cur = hash->nodes[i];
        while (cur) {
            hashnode_t *next = cur->next;
            kvs_blob_free(&cur->key);
            kvs_blob_free(&cur->value);
            free(cur);
            cur = next;
        }
    }
    free(hash->nodes);
    memset(hash, 0, sizeof(*hash));
}

int kvs_hash_set(kvs_hash_t *hash, const kvs_blob_view_t *key, const kvs_blob_view_t *value) {
    if (!hash || !hash->nodes) return -1;
    uint32_t idx = hash_bytes(key->data, key->len) % hash->max_slots;
    for (hashnode_t *n = hash->nodes[idx]; n; n = n->next) {
        if (kvs_blob_equal_view(&n->key, key)) return 1;
    }
    hashnode_t *n = create_node(key, value);
    if (!n) return -2;
    n->next = hash->nodes[idx];
    hash->nodes[idx] = n;
    hash->count++;
    return 0;
}

kvs_blob_t *kvs_hash_get(kvs_hash_t *hash, const kvs_blob_view_t *key) {
    if (!hash || !hash->nodes) return NULL;
    uint32_t idx = hash_bytes(key->data, key->len) % hash->max_slots;
    for (hashnode_t *n = hash->nodes[idx]; n; n = n->next) {
        if (kvs_blob_equal_view(&n->key, key)) return &n->value;
    }
    return NULL;
}

int kvs_hash_mod(kvs_hash_t *hash, const kvs_blob_view_t *key, const kvs_blob_view_t *value) {
    kvs_blob_t *cur = kvs_hash_get(hash, key);
    if (!cur) return 1;
    kvs_blob_t newv = {0};
    if (kvs_blob_dup(&newv, value->data, value->len) != 0) return -2;
    kvs_blob_free(cur);
    *cur = newv;
    return 0;
}

int kvs_hash_del(kvs_hash_t *hash, const kvs_blob_view_t *key) {
    if (!hash || !hash->nodes) return -1;
    uint32_t idx = hash_bytes(key->data, key->len) % hash->max_slots;
    hashnode_t *cur = hash->nodes[idx], *prev = NULL;
    while (cur) {
        if (kvs_blob_equal_view(&cur->key, key)) {
            if (prev) prev->next = cur->next; else hash->nodes[idx] = cur->next;
            kvs_blob_free(&cur->key); kvs_blob_free(&cur->value); free(cur); hash->count--; return 0;
        }
        prev = cur; cur = cur->next;
    }
    return 1;
}

int kvs_hash_exist(kvs_hash_t *hash, const kvs_blob_view_t *key) { return kvs_hash_get(hash, key) ? 0 : 1; }
