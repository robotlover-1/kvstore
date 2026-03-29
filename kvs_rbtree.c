#include "kvstore.h"

kvs_rbtree_t global_rbtree = {0};

static void free_node(rbtree_node *n) {
    if (!n) return;
    free_node(n->left);
    free_node(n->right);
    kvs_blob_free(&n->key);
    kvs_blob_free(&n->value);
    free(n);
}

int kvs_rbtree_create(kvs_rbtree_t *inst) {
    if (!inst) return -1;
    inst->root = NULL; inst->count = 0; return 0;
}

void kvs_rbtree_destory(kvs_rbtree_t *inst) { if (inst) { free_node(inst->root); inst->root = NULL; inst->count = 0; } }

static rbtree_node **find_link(rbtree_node **root, const kvs_blob_view_t *key) {
    while (*root) {
        int cmp = kvs_blob_compare_view(&(*root)->key, key);
        if (cmp == 0) break;
        root = (cmp > 0) ? &(*root)->left : &(*root)->right;
    }
    return root;
}

int kvs_rbtree_set(kvs_rbtree_t *inst, const kvs_blob_view_t *key, const kvs_blob_view_t *value) {
    if (!inst) return -1;
    rbtree_node **link = find_link(&inst->root, key);
    if (*link) return 1;
    rbtree_node *n = (rbtree_node *)calloc(1, sizeof(rbtree_node));
    if (!n) return -2;
    if (kvs_blob_dup(&n->key, key->data, key->len) != 0 || kvs_blob_dup(&n->value, value->data, value->len) != 0) { free_node(n); return -2; }
    *link = n; inst->count++; return 0;
}

kvs_blob_t *kvs_rbtree_get(kvs_rbtree_t *inst, const kvs_blob_view_t *key) {
    if (!inst) return NULL;
    rbtree_node **link = find_link(&inst->root, key);
    return *link ? &(*link)->value : NULL;
}

int kvs_rbtree_mod(kvs_rbtree_t *inst, const kvs_blob_view_t *key, const kvs_blob_view_t *value) {
    kvs_blob_t *cur = kvs_rbtree_get(inst, key);
    if (!cur) return 1;
    kvs_blob_t newv = {0};
    if (kvs_blob_dup(&newv, value->data, value->len) != 0) return -2;
    kvs_blob_free(cur); *cur = newv; return 0;
}

static rbtree_node *extract_min(rbtree_node **root) {
    while ((*root)->left) root = &(*root)->left;
    rbtree_node *n = *root; *root = n->right; n->right = NULL; return n;
}

int kvs_rbtree_del(kvs_rbtree_t *inst, const kvs_blob_view_t *key) {
    if (!inst) return -1;
    rbtree_node **link = find_link(&inst->root, key);
    rbtree_node *n = *link;
    if (!n) return 1;
    if (!n->left) *link = n->right;
    else if (!n->right) *link = n->left;
    else {
        rbtree_node *rep = extract_min(&n->right);
        rep->left = n->left; rep->right = n->right; *link = rep;
    }
    n->left = n->right = NULL; free_node(n); inst->count--; return 0;
}

int kvs_rbtree_exist(kvs_rbtree_t *inst, const kvs_blob_view_t *key) { return kvs_rbtree_get(inst, key) ? 0 : 1; }
