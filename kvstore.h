#ifndef __KV_STORE_H__
#define __KV_STORE_H__

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>
#include <errno.h>

#define NETWORK_REACTOR  0
#define NETWORK_PROACTOR 1
#define NETWORK_NTYCO    2
#define NETWORK_SELECT   NETWORK_REACTOR

#define ENABLE_ARRAY   1
#define ENABLE_RBTREE  1
#define ENABLE_HASH    1

#define BUFFER_LENGTH 16384
#define KVS_MAX_ARGS  32
#define KVS_STREAM_IN_CAP   65536
#define KVS_STREAM_OUT_CAP  65536
#define KVS_EXPIRE_BUCKETS  1024
#define KVS_PERSIST_BUCKETS 1024

#define KVS_ENGINE_ARRAY  1
#define KVS_ENGINE_RBTREE 2
#define KVS_ENGINE_HASH   3

typedef int (*msg_handler)(char *msg, int length, char *response);
extern int reactor_start(unsigned short port, msg_handler handler);
extern int proactor_start(unsigned short port, msg_handler handler);
extern int ntyco_start(unsigned short port, msg_handler handler);

#if ENABLE_ARRAY
typedef struct kvs_array_item_s { char *key; char *value; } kvs_array_item_t;
#define KVS_ARRAY_SIZE 1024
typedef struct kvs_array_s { kvs_array_item_t *table; int idx; int total; } kvs_array_t;
int kvs_array_create(kvs_array_t *inst);
void kvs_array_destory(kvs_array_t *inst);
int kvs_array_set(kvs_array_t *inst, char *key, char *value);
char* kvs_array_get(kvs_array_t *inst, char *key);
int kvs_array_del(kvs_array_t *inst, char *key);
int kvs_array_mod(kvs_array_t *inst, char *key, char *value);
int kvs_array_exist(kvs_array_t *inst, char *key);
#endif

#if ENABLE_RBTREE
#define RED 1
#define BLACK 2
#define ENABLE_KEY_CHAR 1
#if ENABLE_KEY_CHAR
typedef char* KEY_TYPE;
#else
typedef int KEY_TYPE;
#endif
typedef struct _rbtree_node {
    unsigned char color;
    struct _rbtree_node *right;
    struct _rbtree_node *left;
    struct _rbtree_node *parent;
    KEY_TYPE key;
    void *value;
} rbtree_node;
typedef struct _rbtree { rbtree_node *root; rbtree_node *nil; } rbtree;
typedef struct _rbtree kvs_rbtree_t;
int kvs_rbtree_create(kvs_rbtree_t *inst);
void kvs_rbtree_destory(kvs_rbtree_t *inst);
int kvs_rbtree_set(kvs_rbtree_t *inst, char *key, char *value);
char* kvs_rbtree_get(kvs_rbtree_t *inst, char *key);
int kvs_rbtree_del(kvs_rbtree_t *inst, char *key);
int kvs_rbtree_mod(kvs_rbtree_t *inst, char *key, char *value);
int kvs_rbtree_exist(kvs_rbtree_t *inst, char *key);
#endif

#if ENABLE_HASH
#define MAX_KEY_LEN 128
#define MAX_VALUE_LEN 512
#define MAX_TABLE_SIZE 1024
#define ENABLE_KEY_POINTER 1
typedef struct hashnode_s {
#if ENABLE_KEY_POINTER
    char *key; char *value;
#else
    char key[MAX_KEY_LEN]; char value[MAX_VALUE_LEN];
#endif
    struct hashnode_s *next;
} hashnode_t;
typedef struct hashtable_s { hashnode_t **nodes; int max_slots; int count; } hashtable_t;
typedef struct hashtable_s kvs_hash_t;
int kvs_hash_create(kvs_hash_t *hash);
void kvs_hash_destory(kvs_hash_t *hash);
int kvs_hash_set(hashtable_t *hash, char *key, char *value);
char * kvs_hash_get(kvs_hash_t *hash, char *key);
int kvs_hash_mod(kvs_hash_t *hash, char *key, char *value);
int kvs_hash_del(kvs_hash_t *hash, char *key);
int kvs_hash_exist(kvs_hash_t *hash, char *key);
#endif

void *kvs_malloc(size_t size);
void kvs_free(void *ptr);
long long kvs_now_ms(void);

/* RESP request */
typedef struct kvs_resp_arg_s {
    size_t len;
    char *data;
} kvs_resp_arg_t;

typedef struct kvs_resp_request_s {
    int argc;
    kvs_resp_arg_t argv[KVS_MAX_ARGS];
} kvs_resp_request_t;

void kvs_resp_request_reset(kvs_resp_request_t *req);
int kvs_resp_parse_one(const unsigned char *buf, size_t len, size_t *consumed, kvs_resp_request_t *req, char *errbuf, size_t errcap);
int kvs_dispatch_request(const kvs_resp_request_t *req, unsigned char *out, size_t outcap, size_t *outlen, int internal_replay);

/* write queue */
typedef struct kvs_out_node_s {
    unsigned char *data;
    size_t len;
    size_t sent;
    struct kvs_out_node_s *next;
} kvs_out_node_t;

/* TTL */
typedef struct kvs_expire_item_s {
    char *key;
    int engine;
    long long expire_at_ms;
    struct kvs_expire_item_s *next;
} kvs_expire_item_t;

typedef struct kvs_expire_table_s {
    kvs_expire_item_t **buckets;
    int size;
} kvs_expire_table_t;
extern kvs_expire_table_t global_expire;
int kvs_expire_create(kvs_expire_table_t *tab);
void kvs_expire_destroy(kvs_expire_table_t *tab);
int kvs_expire_set(kvs_expire_table_t *tab, int engine, const char *key, long long ttl_ms);
int kvs_expire_del(kvs_expire_table_t *tab, int engine, const char *key);
int kvs_expire_persist(kvs_expire_table_t *tab, int engine, const char *key);
int kvs_expire_is_expired(kvs_expire_table_t *tab, int engine, const char *key);
long long kvs_expire_ttl(kvs_expire_table_t *tab, int engine, const char *key);
int kvs_active_expire_cycle(int budget);

/* persistence */
typedef struct kvs_dataset_item_s {
    int engine;
    char *key;
    char *value;
    long long expire_at_ms;
    struct kvs_dataset_item_s *next;
} kvs_dataset_item_t;

typedef struct kvs_dataset_table_s {
    kvs_dataset_item_t **buckets;
    int size;
} kvs_dataset_table_t;

int kvs_persist_init(const char *dump_path, const char *aof_path);
void kvs_persist_shutdown(void);
int kvs_persist_append_command(const kvs_resp_request_t *req);
int kvs_persist_save_snapshot(void);
int kvs_persist_load_all(void);
void kvs_dataset_set(int engine, const char *key, const char *value);
void kvs_dataset_del(int engine, const char *key);
void kvs_dataset_expire(int engine, const char *key, long long expire_at_ms);
void kvs_dataset_persist(int engine, const char *key);

/* stream */
typedef struct kvs_stream_s {
    unsigned char inbuf[KVS_STREAM_IN_CAP];
    size_t in_len;
    kvs_out_node_t *out_head;
    kvs_out_node_t *out_tail;
    size_t out_queued_bytes;
} kvs_stream_t;

void kvs_stream_init(kvs_stream_t *s);
void kvs_stream_free(kvs_stream_t *s);
int kvs_stream_enqueue(kvs_stream_t *s, const unsigned char *data, size_t len);
int kvs_stream_feed(kvs_stream_t *s, const unsigned char *data, size_t len);

#endif
