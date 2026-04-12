#ifndef KVSTORE_H
#define KVSTORE_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <pthread.h>

#define BUFFER_CAP 65536
#define MAX_EVENTS 1024
#define LISTEN_BACKLOG 128

#define ROLE_MASTER 1
#define ROLE_SLAVE 2

#define KVS_ENGINE_ARRAY      1
#define KVS_ENGINE_RBTREE     2
#define KVS_ENGINE_HASH       3
#define KVS_ENGINE_SKIPTABLE  4

#define ENABLE_ARRAY 1
#define ENABLE_RBTREE 1
#define ENABLE_HASH 1
#define ENABLE_SKIPTABLE 1

#define KVS_ARRAY_SIZE 1024
#define MAX_TABLE_SIZE 1024
#define ENABLE_KEY_POINTER 1
#define RED 1
#define BLACK 2
#define ENABLE_KEY_CHAR 1

#define BGSAVE_IDLE 0
#define BGSAVE_RUNNING 1
#define BGSAVE_OK 2
#define BGSAVE_ERR 3

#define REPL_STATE_NONE 0
#define REPL_STATE_WAIT_BGSAVE 1
#define REPL_STATE_ONLINE 2

typedef int (*msg_handler)(char *msg, int length, char *response);

#if ENABLE_ARRAY
typedef struct kvs_array_item_s {
    char *key;
    char *value;
} kvs_array_item_t;

typedef struct kvs_array_s {
    kvs_array_item_t *table;
    int idx;
    int total;
} kvs_array_t;

extern kvs_array_t global_array;
int kvs_array_create(kvs_array_t *inst);
void kvs_array_destory(kvs_array_t *inst);
int kvs_array_set(kvs_array_t *inst, char *key, char *value);
char* kvs_array_get(kvs_array_t *inst, char *key);
int kvs_array_del(kvs_array_t *inst, char *key);
int kvs_array_mod(kvs_array_t *inst, char *key, char *value);
int kvs_array_exist(kvs_array_t *inst, char *key);
#endif

#if ENABLE_RBTREE
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

typedef struct _rbtree {
    rbtree_node *root;
    rbtree_node *nil;
} rbtree;

typedef struct _rbtree kvs_rbtree_t;
extern kvs_rbtree_t global_rbtree;
int kvs_rbtree_create(kvs_rbtree_t *inst);
void kvs_rbtree_destory(kvs_rbtree_t *inst);
int kvs_rbtree_set(kvs_rbtree_t *inst, char *key, char *value);
char* kvs_rbtree_get(kvs_rbtree_t *inst, char *key);
int kvs_rbtree_del(kvs_rbtree_t *inst, char *key);
int kvs_rbtree_mod(kvs_rbtree_t *inst, char *key, char *value);
int kvs_rbtree_exist(kvs_rbtree_t *inst, char *key);
#endif

#if ENABLE_HASH
typedef struct hashnode_s {
#if ENABLE_KEY_POINTER
    char *key;
    char *value;
#else
    char key[128];
    char value[512];
#endif
    struct hashnode_s *next;
} hashnode_t;

typedef struct hashtable_s {
    hashnode_t **nodes;
    int max_slots;
    int count;
} hashtable_t;

typedef struct hashtable_s kvs_hash_t;
extern kvs_hash_t global_hash;
int kvs_hash_create(kvs_hash_t *hash);
void kvs_hash_destory(kvs_hash_t *hash);
int kvs_hash_set(hashtable_t *hash, char *key, char *value);
char *kvs_hash_get(kvs_hash_t *hash, char *key);
int kvs_hash_mod(kvs_hash_t *hash, char *key, char *value);
int kvs_hash_del(kvs_hash_t *hash, char *key);
int kvs_hash_exist(kvs_hash_t *hash, char *key);
#endif

#if ENABLE_SKIPTABLE
typedef struct kvs_skiptable_s kvs_skiptable_t;
typedef int (*kvs_skip_visit_cb)(const char *key, const char *value, void *arg);
extern kvs_skiptable_t global_skiptable;
int kvs_skiptable_create(kvs_skiptable_t *inst);
void kvs_skiptable_destory(kvs_skiptable_t *inst);
int kvs_skiptable_set(kvs_skiptable_t *inst, char *key, char *value);
char *kvs_skiptable_get(kvs_skiptable_t *inst, char *key);
int kvs_skiptable_mod(kvs_skiptable_t *inst, char *key, char *value);
int kvs_skiptable_del(kvs_skiptable_t *inst, char *key);
int kvs_skiptable_exist(kvs_skiptable_t *inst, char *key);
int kvs_skiptable_foreach(kvs_skiptable_t *inst, kvs_skip_visit_cb cb, void *arg);
#endif

#define KVS_EXPIRE_BUCKETS 1024
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

typedef struct out_node_s {
    unsigned char *data;
    size_t len;
    size_t sent;
    struct out_node_s *next;
} out_node_t;

typedef struct conn_s {
    int fd;
    int is_listener;
    int is_replica;
    int replica_state;
    unsigned long long wait_bgsave_seq;
    unsigned char inbuf[BUFFER_CAP];
    size_t in_len;
    out_node_t *out_head;
    out_node_t *out_tail;
    out_node_t *repl_backlog_head;
    out_node_t *repl_backlog_tail;
    struct conn_s *next_replica;
} conn_t;

typedef struct {
    int role;
    int port;
    char master_host[128];
    int master_port;
    char dump_path[256];
    char aof_path[256];
    char mem_backend[32];
} kv_config_t;

typedef struct {
    const char *backend_name;
    int backend_id;
    int initialized;
    size_t small_max_size;
    size_t class_count;
    unsigned long long alloc_calls;
    unsigned long long free_calls;
    unsigned long long calloc_calls;
    unsigned long long realloc_calls;
    unsigned long long small_alloc_calls;
    unsigned long long small_free_calls;
    unsigned long long large_alloc_calls;
    unsigned long long large_free_calls;
    unsigned long long fallback_alloc_calls;
    unsigned long long fallback_free_calls;
    unsigned long long current_small_inuse;
    unsigned long long peak_small_inuse;
    unsigned long long current_large_inuse_bytes;
    unsigned long long peak_large_inuse_bytes;
    unsigned long long current_fallback_inuse_bytes;
    unsigned long long peak_fallback_inuse_bytes;
    unsigned long long total_small_page_bytes;
    unsigned long long total_large_map_bytes;
    unsigned long long active_large_map_bytes;
    unsigned long long peak_active_large_map_bytes;
    unsigned long long current_requested_bytes;
    unsigned long long current_allocated_bytes;
    unsigned long long internal_fragment_bytes;
    unsigned long long small_page_used_bytes;
    unsigned int internal_fragment_ppm;
    unsigned int page_utilization_ppm;
    size_t class_sizes[16];
    size_t class_total_chunks[16];
    size_t class_free_chunks[16];
    size_t class_page_count[16];
    size_t class_bytes_in_pages[16];
} kvs_mem_stats_t;

extern kv_config_t g_cfg;
extern int g_epfd;
extern conn_t *g_replicas;
extern pthread_mutex_t g_repl_lock;
extern FILE *g_aof_fp;
extern pid_t g_bgsave_pid;
extern int g_bgsave_state;
extern long long g_bgsave_last_start_ms;
extern long long g_bgsave_last_end_ms;
extern unsigned long long g_bgsave_seq;
extern unsigned long long g_bgsave_done_seq;

void *kvs_malloc(size_t size);
void *kvs_calloc(size_t n, size_t size);
void *kvs_realloc(void *ptr, size_t size);
void kvs_free(void *ptr);
long long kvs_now_ms(void);
int kvs_mem_prepare_process(const char *backend_name, char *argv0, char **argv);
int kvs_mem_init(const char *backend_name);
const char *kvs_mem_backend_name(void);
int kvs_mem_get_stats(kvs_mem_stats_t *stats);

int kvs_expire_create(kvs_expire_table_t *tab);
void kvs_expire_destroy(kvs_expire_table_t *tab);
int kvs_expire_set(kvs_expire_table_t *tab, int engine, const char *key, long long ttl_ms);
int kvs_expire_del(kvs_expire_table_t *tab, int engine, const char *key);
int kvs_expire_persist(kvs_expire_table_t *tab, int engine, const char *key);
int kvs_expire_is_expired(kvs_expire_table_t *tab, int engine, const char *key);
long long kvs_expire_ttl(kvs_expire_table_t *tab, int engine, const char *key);
int kvs_active_expire_cycle(int budget);

int reactor_start(void);
int queue_bytes(conn_t *c, const unsigned char *buf, size_t len);
void close_conn(conn_t *c);
int parse_resp_stream(conn_t *c, unsigned char *buf, size_t *len, int from_replication);
int handle_parsed_command(conn_t *c, int argc, char **argv, size_t *argl, const unsigned char *raw, size_t rawlen, int from_replication);

void repl_add_slave(conn_t *c);
void repl_remove_slave(conn_t *c);
void repl_broadcast(const unsigned char *raw, size_t rawlen);
void repl_fullsync_cron(void);
int start_slave_thread(void);

int persist_init(void);
void persist_close(void);
int persist_append_raw(const unsigned char *buf, size_t len);
int persist_save_dump(void);
int persist_recover(void);
int kvs_snapshot_to_fp(FILE *fp);
int persist_bgsave_start(void);
void persist_bgsave_poll(void);
int persist_bgsave_in_progress(void);
const char *persist_bgsave_state_name(void);

int resp_simple_string(char *out, size_t cap, const char *s);
int resp_error(char *out, size_t cap, const char *s);
int resp_integer(char *out, size_t cap, long long v);
int resp_bulk(char *out, size_t cap, const char *s, size_t len);
int resp_null_bulk(char *out, size_t cap);
size_t resp_build_cmd3(unsigned char *out, size_t cap, const char *cmd, const char *a1, const char *a2);
size_t resp_build_cmd2(unsigned char *out, size_t cap, const char *cmd, const char *a1);
size_t resp_build_cmd1(unsigned char *out, size_t cap, const char *cmd);

#endif
