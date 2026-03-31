#include "kvstore/kvstore.h"

kvs_array_t global_array = {0};

int kvs_array_create(kvs_array_t *inst) {
    if (!inst) return -1;
    inst->table = (kvs_array_item_t *)kvs_malloc(KVS_ARRAY_SIZE * sizeof(kvs_array_item_t));
    if (!inst->table) return -1;
    memset(inst->table, 0, KVS_ARRAY_SIZE * sizeof(kvs_array_item_t));
    inst->idx = 0;
    inst->total = 0;
    return 0;
}

void kvs_array_destory(kvs_array_t *inst) {
    if (!inst || !inst->table) return;
    for (int i = 0; i < KVS_ARRAY_SIZE; ++i) {
        kvs_free(inst->table[i].key);
        kvs_free(inst->table[i].value);
    }
    kvs_free(inst->table);
    inst->table = NULL;
    inst->total = 0;
}

static int find_slot(kvs_array_t *inst, char *key) {
    if (!inst || !inst->table || !key) return -1;
    for (int i = 0; i < KVS_ARRAY_SIZE; ++i) {
        if (inst->table[i].key && strcmp(inst->table[i].key, key) == 0) return i;
    }
    return -1;
}

int kvs_array_set(kvs_array_t *inst, char *key, char *value) {
    if (!inst || !inst->table || !key || !value) return -1;
    if (find_slot(inst, key) >= 0) return 1;
    for (int i = 0; i < KVS_ARRAY_SIZE; ++i) {
        if (!inst->table[i].key) {
            size_t klen = strlen(key), vlen = strlen(value);
            inst->table[i].key = (char *)kvs_malloc(klen + 1);
            inst->table[i].value = (char *)kvs_malloc(vlen + 1);
            if (!inst->table[i].key || !inst->table[i].value) return -2;
            memcpy(inst->table[i].key, key, klen + 1);
            memcpy(inst->table[i].value, value, vlen + 1);
            inst->total++;
            return 0;
        }
    }
    return -1;
}

char *kvs_array_get(kvs_array_t *inst, char *key) {
    int idx = find_slot(inst, key);
    return idx >= 0 ? inst->table[idx].value : NULL;
}

int kvs_array_del(kvs_array_t *inst, char *key) {
    int idx = find_slot(inst, key);
    if (idx < 0) return 1;
    kvs_free(inst->table[idx].key);
    kvs_free(inst->table[idx].value);
    inst->table[idx].key = NULL;
    inst->table[idx].value = NULL;
    if (inst->total > 0) inst->total--;
    return 0;
}

int kvs_array_mod(kvs_array_t *inst, char *key, char *value) {
    int idx = find_slot(inst, key);
    if (idx < 0) return 1;
    size_t vlen = strlen(value);
    char *nv = (char *)kvs_malloc(vlen + 1);
    if (!nv) return -2;
    memcpy(nv, value, vlen + 1);
    kvs_free(inst->table[idx].value);
    inst->table[idx].value = nv;
    return 0;
}

int kvs_array_exist(kvs_array_t *inst, char *key) {
    return find_slot(inst, key) >= 0 ? 0 : 1;
}
