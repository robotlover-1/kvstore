#ifndef __KV_STORE_H__
#define __KV_STORE_H__

#include <assert.h>
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>

#define NETWORK_REACTOR 0
#define NETWORK_PROACTOR 1
#define NETWORK_NTYCO 2
#define NETWORK_SELECT NETWORK_REACTOR

#define ENABLE_ARRAY 1
#define ENABLE_RBTREE 1
#define ENABLE_HASH 1

#define KVS_ARRAY_SIZE 1024
#define MAX_TABLE_SIZE 1024
#define KVS_MAX_ARGC 16
#define KVS_STREAM_IN_CAP 65536
#define KVS_READ_CHUNK 4096

#define KVS_ENGINE_ARRAY 1
#define KVS_ENGINE_RBTREE 2
#define KVS_ENGINE_HASH 3
#define KVS_EXPIRE_BUCKETS 1024

typedef struct kvs_blob_s {
    unsigned char *data;
    size_t len;
} kvs_blob_t;

typedef struct kvs_blob_view_s {
    const unsigned char *data;
    size_t len;
} kvs_blob_view_t;

typedef struct kvs_resp_request_s {
    int argc;
    kvs_blob_t argv[KVS_MAX_ARGC];
} kvs_resp_request_t;

typedef enum kvs_resp_parse_state_e {
    KVS_RESP_STATE_ARRAY = 0,
    KVS_RESP_STATE_BULK_LEN,
    KVS_RESP_STATE_BULK_DATA
} kvs_resp_parse_state_t;

typedef struct kvs_out_node_s {
    unsigned char *data;
    size_t len;
    size_t sent;
    struct kvs_out_node_s *next;
} kvs_out_node_t;

typedef struct kvs_stream_s {
    unsigned char inbuf[KVS_STREAM_IN_CAP];
    size_t in_len;
    size_t parse_pos;

    kvs_resp_parse_state_t state;
    int argc_expected;
    int argc_read;
    ssize_t bulk_len;
    kvs_resp_request_t req;

    kvs_out_node_t *out_head;
    kvs_out_node_t *out_tail;
    size_t out_queued_bytes;
} kvs_stream_t;

typedef struct kvs_expire_item_s {
    kvs_blob_t key;
    int engine;
    long long expire_at_ms;
    struct kvs_expire_item_s *next;
} kvs_expire_item_t;

typedef struct kvs_expire_table_s {
    kvs_expire_item_t **buckets;
    int size;
} kvs_expire_table_t;

typedef struct kvs_array_item_s {
    kvs_blob_t key;
    kvs_blob_t value;
    int used;
} kvs_array_item_t;

typedef struct kvs_array_s {
    kvs_array_item_t *table;
    int total;
} kvs_array_t;

typedef struct hashnode_s {
    kvs_blob_t key;
    kvs_blob_t value;
    struct hashnode_s *next;
} hashnode_t;

typedef struct hashtable_s {
    hashnode_t **nodes;
    int max_slots;
    int count;
} hashtable_t;
typedef struct hashtable_s kvs_hash_t;

typedef struct _rbtree_node {
    struct _rbtree_node *left;
    struct _rbtree_node *right;
    kvs_blob_t key;
    kvs_blob_t value;
} rbtree_node;

typedef struct _rbtree {
    rbtree_node *root;
    int count;
} rbtree;
typedef struct _rbtree kvs_rbtree_t;

typedef int (*msg_handler)(char *msg, int length, char *response);

extern kvs_expire_table_t global_expire;
extern int reactor_start(unsigned short port, msg_handler handler);
extern int proactor_start(unsigned short port, msg_handler handler);
extern int ntyco_start(unsigned short port, msg_handler handler);

void *kvs_malloc(size_t size);
void kvs_free(void *ptr);

int kvs_array_create(kvs_array_t *inst);
void kvs_array_destory(kvs_array_t *inst);
int kvs_array_set(kvs_array_t *inst, const kvs_blob_view_t *key, const kvs_blob_view_t *value);
kvs_blob_t *kvs_array_get(kvs_array_t *inst, const kvs_blob_view_t *key);
int kvs_array_del(kvs_array_t *inst, const kvs_blob_view_t *key);
int kvs_array_mod(kvs_array_t *inst, const kvs_blob_view_t *key, const kvs_blob_view_t *value);
int kvs_array_exist(kvs_array_t *inst, const kvs_blob_view_t *key);

int kvs_hash_create(kvs_hash_t *hash);
void kvs_hash_destory(kvs_hash_t *hash);
int kvs_hash_set(kvs_hash_t *hash, const kvs_blob_view_t *key, const kvs_blob_view_t *value);
kvs_blob_t *kvs_hash_get(kvs_hash_t *hash, const kvs_blob_view_t *key);
int kvs_hash_mod(kvs_hash_t *hash, const kvs_blob_view_t *key, const kvs_blob_view_t *value);
int kvs_hash_del(kvs_hash_t *hash, const kvs_blob_view_t *key);
int kvs_hash_exist(kvs_hash_t *hash, const kvs_blob_view_t *key);

int kvs_rbtree_create(kvs_rbtree_t *inst);
void kvs_rbtree_destory(kvs_rbtree_t *inst);
int kvs_rbtree_set(kvs_rbtree_t *inst, const kvs_blob_view_t *key, const kvs_blob_view_t *value);
kvs_blob_t *kvs_rbtree_get(kvs_rbtree_t *inst, const kvs_blob_view_t *key);
int kvs_rbtree_del(kvs_rbtree_t *inst, const kvs_blob_view_t *key);
int kvs_rbtree_mod(kvs_rbtree_t *inst, const kvs_blob_view_t *key, const kvs_blob_view_t *value);
int kvs_rbtree_exist(kvs_rbtree_t *inst, const kvs_blob_view_t *key);

int init_kvengine(void);
void dest_kvengine(void);

void kvs_stream_init(kvs_stream_t *s);
void kvs_stream_destroy(kvs_stream_t *s);
void kvs_stream_reset_request(kvs_stream_t *s);
void kvs_stream_compact_input(kvs_stream_t *s);
int kvs_stream_feed(kvs_stream_t *s, const unsigned char *data, size_t len);
int kvs_stream_has_output(const kvs_stream_t *s);
const unsigned char *kvs_stream_output_ptr(const kvs_stream_t *s);
size_t kvs_stream_output_len(const kvs_stream_t *s);
void kvs_stream_consume_output(kvs_stream_t *s, size_t n);

int kvs_blob_equal_view(const kvs_blob_t *a, const kvs_blob_view_t *b);
int kvs_blob_compare_view(const kvs_blob_t *a, const kvs_blob_view_t *b);
int kvs_blob_dup(kvs_blob_t *dst, const unsigned char *data, size_t len);
void kvs_blob_free(kvs_blob_t *b);

long long kvs_now_ms(void);
int kvs_expire_create(kvs_expire_table_t *tab);
void kvs_expire_destroy(kvs_expire_table_t *tab);
int kvs_expire_set(kvs_expire_table_t *tab, int engine, const kvs_blob_view_t *key, long long ttl_ms);
int kvs_expire_del(kvs_expire_table_t *tab, int engine, const kvs_blob_view_t *key);
int kvs_expire_persist(kvs_expire_table_t *tab, int engine, const kvs_blob_view_t *key);
int kvs_expire_is_expired(kvs_expire_table_t *tab, int engine, const kvs_blob_view_t *key);
long long kvs_expire_ttl(kvs_expire_table_t *tab, int engine, const kvs_blob_view_t *key);
int kvs_active_expire_cycle(int budget);

#endif
