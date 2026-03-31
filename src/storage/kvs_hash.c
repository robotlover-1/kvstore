#include "kvstore/kvstore.h"

kvs_hash_t global_hash = {0};

static int _hash(char *key, int size) {
    if (!key) return -1;
    unsigned int sum = 2166136261u;
    for (int i = 0; key[i] != 0; ++i) {
        sum ^= (unsigned char)key[i];
        sum *= 16777619u;
    }
    return (int)(sum % (unsigned int)size);
}

static hashnode_t *_create_node(char *key, char *value) {
    hashnode_t *node = (hashnode_t*)kvs_malloc(sizeof(hashnode_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(*node));
    size_t klen = strlen(key), vlen = strlen(value);
    node->key = (char *)kvs_malloc(klen + 1);
    node->value = (char *)kvs_malloc(vlen + 1);
    if (!node->key || !node->value) {
        kvs_free(node->key); kvs_free(node->value); kvs_free(node); return NULL;
    }
    memcpy(node->key, key, klen + 1);
    memcpy(node->value, value, vlen + 1);
    return node;
}

int kvs_hash_create(kvs_hash_t *hash) {
    if (!hash) return -1;
    hash->nodes = (hashnode_t**)kvs_malloc(sizeof(hashnode_t*) * MAX_TABLE_SIZE);
    if (!hash->nodes) return -1;
    memset(hash->nodes, 0, sizeof(hashnode_t*) * MAX_TABLE_SIZE);
    hash->max_slots = MAX_TABLE_SIZE;
    hash->count = 0;
    return 0;
}

void kvs_hash_destory(kvs_hash_t *hash) {
    if (!hash || !hash->nodes) return;
    for (int i = 0; i < hash->max_slots; ++i) {
        hashnode_t *node = hash->nodes[i];
        while (node) {
            hashnode_t *tmp = node;
            node = node->next;
            kvs_free(tmp->key);
            kvs_free(tmp->value);
            kvs_free(tmp);
        }
    }
    kvs_free(hash->nodes);
    hash->nodes = NULL;
}

int kvs_hash_set(kvs_hash_t *hash, char *key, char *value) {
    if (!hash || !key || !value) return -1;
    int idx = _hash(key, MAX_TABLE_SIZE);
    for (hashnode_t *node = hash->nodes[idx]; node; node = node->next) {
        if (strcmp(node->key, key) == 0) return 1;
    }
    hashnode_t *new_node = _create_node(key, value);
    if (!new_node) return -2;
    new_node->next = hash->nodes[idx];
    hash->nodes[idx] = new_node;
    hash->count++;
    return 0;
}

char *kvs_hash_get(kvs_hash_t *hash, char *key) {
    if (!hash || !key) return NULL;
    int idx = _hash(key, MAX_TABLE_SIZE);
    for (hashnode_t *node = hash->nodes[idx]; node; node = node->next) {
        if (strcmp(node->key, key) == 0) return node->value;
    }
    return NULL;
}

int kvs_hash_mod(kvs_hash_t *hash, char *key, char *value) {
    if (!hash || !key || !value) return -1;
    int idx = _hash(key, MAX_TABLE_SIZE);
    for (hashnode_t *node = hash->nodes[idx]; node; node = node->next) {
        if (strcmp(node->key, key) == 0) {
            size_t vlen = strlen(value);
            char *nv = (char *)kvs_malloc(vlen + 1);
            if (!nv) return -2;
            memcpy(nv, value, vlen + 1);
            kvs_free(node->value);
            node->value = nv;
            return 0;
        }
    }
    return 1;
}

int kvs_hash_del(kvs_hash_t *hash, char *key) {
    if (!hash || !key) return -1;
    int idx = _hash(key, MAX_TABLE_SIZE);
    hashnode_t *cur = hash->nodes[idx], *prev = NULL;
    while (cur) {
        if (strcmp(cur->key, key) == 0) {
            if (prev) prev->next = cur->next; else hash->nodes[idx] = cur->next;
            kvs_free(cur->key); kvs_free(cur->value); kvs_free(cur);
            if (hash->count > 0) hash->count--;
            return 0;
        }
        prev = cur; cur = cur->next;
    }
    return 1;
}

int kvs_hash_exist(kvs_hash_t *hash, char *key) {
    return kvs_hash_get(hash, key) ? 0 : 1;
}
