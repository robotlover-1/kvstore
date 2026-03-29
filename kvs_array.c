#include "kvstore.h"

kvs_array_t global_array = {0};

static int find_slot(kvs_array_t *inst, const kvs_blob_view_t *key) {
    for (int i = 0; i < KVS_ARRAY_SIZE; ++i) {
        if (inst->table[i].used && kvs_blob_equal_view(&inst->table[i].key, key)) return i;
    }
    return -1;
}

int kvs_array_create(kvs_array_t *inst) {
    if (!inst || inst->table) return -1;
    inst->table = (kvs_array_item_t *)calloc(KVS_ARRAY_SIZE, sizeof(kvs_array_item_t));
    return inst->table ? 0 : -1;
}

void kvs_array_destory(kvs_array_t *inst) {
    if (!inst || !inst->table) return;
    for (int i = 0; i < KVS_ARRAY_SIZE; ++i) {
        if (inst->table[i].used) {
            kvs_blob_free(&inst->table[i].key);
            kvs_blob_free(&inst->table[i].value);
        }
    }
    free(inst->table);
    inst->table = NULL;
    inst->total = 0;
}

int kvs_array_set(kvs_array_t *inst, const kvs_blob_view_t *key, const kvs_blob_view_t *value) {
    if (!inst || !inst->table || !key || !value) return -1;
    if (find_slot(inst, key) >= 0) return 1;
    for (int i = 0; i < KVS_ARRAY_SIZE; ++i) {
        if (!inst->table[i].used) {
            if (kvs_blob_dup(&inst->table[i].key, key->data, key->len) != 0) return -2;
            if (kvs_blob_dup(&inst->table[i].value, value->data, value->len) != 0) {
                kvs_blob_free(&inst->table[i].key);
                return -2;
            }
            inst->table[i].used = 1;
            inst->total++;
            return 0;
        }
    }
    return -1;
}

kvs_blob_t *kvs_array_get(kvs_array_t *inst, const kvs_blob_view_t *key) {
    int idx = find_slot(inst, key);
    return idx >= 0 ? &inst->table[idx].value : NULL;
}

int kvs_array_del(kvs_array_t *inst, const kvs_blob_view_t *key) {
    int idx = find_slot(inst, key);
    if (idx < 0) return 1;
    kvs_blob_free(&inst->table[idx].key);
    kvs_blob_free(&inst->table[idx].value);
    memset(&inst->table[idx], 0, sizeof(inst->table[idx]));
    inst->total--;
    return 0;
}

int kvs_array_mod(kvs_array_t *inst, const kvs_blob_view_t *key, const kvs_blob_view_t *value) {
    int idx = find_slot(inst, key);
    if (idx < 0) return 1;
    kvs_blob_t newv = {0};
    if (kvs_blob_dup(&newv, value->data, value->len) != 0) return -2;
    kvs_blob_free(&inst->table[idx].value);
    inst->table[idx].value = newv;
    return 0;
}

int kvs_array_exist(kvs_array_t *inst, const kvs_blob_view_t *key) {
    return find_slot(inst, key) >= 0 ? 0 : 1;
}
