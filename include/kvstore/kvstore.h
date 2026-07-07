#ifndef KVSTORE_H
#define KVSTORE_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <pthread.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/resource.h>

#ifndef KVS_ENABLE_RDMA
#define KVS_ENABLE_RDMA 0
#endif

#ifndef KVS_ENABLE_EBPF
#define KVS_ENABLE_EBPF 0
#endif

#ifndef KVS_ENABLE_KPROBE_RDMA
#define KVS_ENABLE_KPROBE_RDMA 0
#endif

#define BUFFER_CAP 65536
#define MAX_EVENTS 1024
#define LISTEN_BACKLOG 128

#define ROLE_MASTER 1
#define ROLE_SLAVE 2

#define KVS_REPL_TRANSPORT_TCP 1
#define KVS_REPL_TRANSPORT_RDMA 2
#define KVS_REPL_TRANSPORT_EBPF 3
#define KVS_REPL_TRANSPORT_KPROBE_RDMA 4
#define KVS_REPL_TRANSPORT_EBPF_TCP 5

/* KVSD format flags */
#define KVSD_FLAG_HAS_EXPIRE  0x01   /* record has 8-byte expire_ms after value */

/* Replication send context: which transport to use */
#define KVS_REPL_SEND_FULLSYNC  1   /* bulk existing data: RDMA */
#define KVS_REPL_SEND_REALTIME  2   /* incremental real-time: eBPF */

#define KVS_ENGINE_ARRAY      1
#define KVS_ENGINE_RBTREE     2
#define KVS_ENGINE_HASH       3
#define KVS_ENGINE_SKIPTABLE  4
#define KVS_ENGINE_DOC        5

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

typedef int (*msg_handler)(char *msg, int length, char *response);

typedef enum {
    KVS_AOF_FSYNC_OFF    = 0,
    KVS_AOF_FSYNC_ALWAYS = 1,
} kvs_aof_fsync_policy_t;

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
    uint32_t hv;              // cached FNV-1a hash (32-bit, not modulo-reduced)
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

typedef struct kvs_hash_s {
    hashtable_t ht[2];        // ht[0]: active, ht[1]: expansion target
    int rehash_idx;           // next bucket to migrate, -1 = no rehash in progress
} kvs_hash_t;

extern kvs_hash_t global_hash;
int kvs_hash_create(kvs_hash_t *hash);
void kvs_hash_destory(kvs_hash_t *hash);
int kvs_hash_set(kvs_hash_t *hash, char *key, char *value);
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

#define ENABLE_DOC 1

#if ENABLE_DOC
#define KVS_DOC_BUCKETS 1024
#define KVS_DOC_FIELD_BUCKETS 16

typedef struct kvs_doc_field_s {
    char *name;
    char *value;
    struct kvs_doc_field_s *next;
} kvs_doc_field_t;

typedef struct kvs_doc_s {
    char *key;
    kvs_doc_field_t **fields;
    int field_count;
    int bucket_count;
    struct kvs_doc_s *next;
} kvs_doc_t;

typedef struct kvs_doc_table_s {
    kvs_doc_t **buckets;
    int size;
    int count;
} kvs_doc_table_t;

extern kvs_doc_table_t global_doc;
int kvs_doc_create(kvs_doc_table_t *tab);
void kvs_doc_destroy(kvs_doc_table_t *tab);
int kvs_doc_set(kvs_doc_table_t *tab, const char *key, const char *field, const char *value);
char *kvs_doc_get(kvs_doc_table_t *tab, const char *key, const char *field);
int kvs_doc_del_field(kvs_doc_table_t *tab, const char *key, const char *field);
int kvs_doc_del(kvs_doc_table_t *tab, const char *key);
int kvs_doc_exist(kvs_doc_table_t *tab, const char *key);
int kvs_doc_field_exist(kvs_doc_table_t *tab, const char *key, const char *field);
int kvs_doc_field_count(kvs_doc_table_t *tab, const char *key);

typedef int (*kvs_doc_field_visit_cb)(const char *name, const char *value, void *arg);
int kvs_doc_foreach_field(kvs_doc_table_t *tab, const char *key, kvs_doc_field_visit_cb cb, void *arg);

typedef int (*kvs_doc_visit_cb)(const char *key, kvs_doc_t *doc, void *arg);
int kvs_doc_foreach(kvs_doc_table_t *tab, kvs_doc_visit_cb cb, void *arg);
#endif

#define KVS_EXPIRE_BUCKETS 8192
#define KVS_EXPIRE_HEAP_INIT_CAP 1024
typedef struct kvs_expire_item_s {
    char *key;
    int engine;
    long long expire_at_ms;
    size_t heap_index;
    struct kvs_expire_item_s *next;
} kvs_expire_item_t;

typedef struct kvs_expire_table_s {
    kvs_expire_item_t **buckets;
    size_t size;
    size_t count;
    kvs_expire_item_t **heap;
    size_t heap_size;
    size_t heap_cap;
} kvs_expire_table_t;
extern kvs_expire_table_t global_expire;

#define OUT_RING_SIZE (4 * 1024 * 1024)

typedef struct conn_s {
    int fd;
    int is_listener;
    int is_replica;
    int repl_draining;
    int repl_fullsync_pending;
    int repl_transport_kind;
    unsigned long long repl_offset_sent;
    unsigned long long repl_applied_offset_ack;
    unsigned long long repl_durable_offset_ack;
    long long repl_last_ack_ms;
    long long repl_last_send_ms;
    unsigned char inbuf[BUFFER_CAP];
    size_t in_len;
    unsigned char out_ring[OUT_RING_SIZE];  /* ring buffer for batched output */
    size_t out_ring_head;                   /* read position */
    size_t out_ring_tail;                   /* write position */
    size_t out_ring_len;                    /* pending bytes */
    int fwd_healthy;                   /* kprobe fwd health: 1=healthy, 0=fallback */
    time_t fwd_last_active;            /* last successful kprobe fwd send timestamp */
    struct conn_s *next_replica;
} conn_t;

#define KVS_AUTOSNAP_RULES_MAX 8

typedef struct {
    long long seconds;
    long long changes;
} kvs_autosnap_rule_t;

typedef struct {
    int role;
    int port;
    char master_host[128];
    int master_port;
    char dump_path[256];
    char aof_path[256];
    char mem_backend[32];
    char net_backend[32];
    char repl_transport_backend[32];
    char repl_fullsync_transport[32];   /* transport for fullsync/snapshot (RDMA) */
    char repl_realtime_transport[32];   /* transport for realtime broadcast (eBPF) */
    int ebpf_enabled;               /* 0=禁用, 1=由独立进程管理eBPF */
    char ebpf_obj_path[256];
    char ebpf_pin_path[256];
    int ebpf_redirect;
    int ebpf_redirect_key;
    int ebpf_forward;
    char rdma_dev[32];
    int rdma_ib_port;
    int rdma_gid_idx;
    int rdma_port;          /* RDMA listener port (0 = auto: main port + 1) */
    int rdma_recv_slots;
    int rdma_chunk_size;
    int rdma_qp_wr_depth;
    kvs_aof_fsync_policy_t aof_fsync;
    char log_mode[32];
    int autosnap_rule_count;
    kvs_autosnap_rule_t autosnap_rules[KVS_AUTOSNAP_RULES_MAX];

    int is_sentinel;
    char sentinel_master_name[64];
    char sentinel_monitor_host[128];
    int sentinel_monitor_port;
    char sentinel_known_slaves[512];
    int sentinel_down_after_ms;
    int sentinel_failover_timeout_ms;
    int sentinel_quorum;

    /* kprobe+RDMA 增量同步 */
    int kprobe_enabled;                 /* 0=禁用 1=启用 */
    char repl_kprobe_obj_path[256];     /* kprobe BPF 对象文件路径 */
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
    size_t class_sizes[24];
    size_t class_total_chunks[24];
    size_t class_free_chunks[24];
    size_t class_page_count[24];
    size_t class_bytes_in_pages[24];
} kvs_mem_stats_t;

typedef struct {
    unsigned long long initialized;
    unsigned long long compiled;
    unsigned long long register_attempts;
    unsigned long long register_failures;
    int last_errno;
    char last_error[128];
    unsigned long long sk_msg_count;
    unsigned long long sk_msg_bytes;
    unsigned long long sk_msg_pass;
    unsigned long long sk_msg_drop;
    unsigned long long redirect_enabled;
    unsigned long long redirect_attempts;
    unsigned long long redirect_success;
    unsigned long long redirect_failures;
    unsigned long long forward_enabled;
    unsigned long long role_unknown;
    unsigned long long role_master;
    unsigned long long role_slave;
} kvs_repl_ebpf_stats_t;

extern kv_config_t g_cfg;
extern int g_epfd;
extern conn_t *g_replicas;
extern pthread_mutex_t g_repl_lock;
extern volatile int g_repl_fullsync_in_progress;
extern int g_aof_fd;

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
int proactor_start(unsigned short port);
int ntyco_start(unsigned short port);

int queue_bytes(conn_t *c, const unsigned char *buf, size_t len);
void flush_conn_output(conn_t *c);
void close_conn(conn_t *c);
int parse_resp_stream(conn_t *c, unsigned char *buf, size_t *len, int from_replication);
int handle_parsed_command(conn_t *c, int argc, char **argv, size_t *argl, const unsigned char *raw, size_t rawlen, int from_replication);

const char *repl_transport_name(void);
const char *repl_transport_configured_name(void);
const char *repl_transport_active_name(void);
const char *repl_transport_fallback_reason(void);
unsigned long long repl_transport_fallback_count(void);
long long repl_transport_fallback_until_ms(void);
const char *repl_fullsync_transport_name(void);
const char *repl_realtime_transport_name(void);
int repl_transport_send(conn_t *c, const unsigned char *buf, size_t len);
int repl_transport_send_many(conn_t *c, const unsigned char *buf1, size_t len1, const unsigned char *buf2, size_t len2);
int repl_fullsync_send(conn_t *c, const unsigned char *buf, size_t len);
int repl_realtime_send(conn_t *c, const unsigned char *buf, size_t len);
int repl_send_chunked(conn_t *c, const unsigned char *buf, size_t len);
int repl_send_chunked_ctx(conn_t *c, const unsigned char *buf, size_t len, int send_ctx);

void repl_add_slave(conn_t *c);
void repl_remove_slave(conn_t *c);
int repl_handle_replica_send_failure(conn_t *c, conn_t **linkp);
void repl_broadcast(const unsigned char *raw, size_t rawlen);
void repl_note_send_context(const char *stage, size_t len, unsigned long long offset, const unsigned char *buf);
void repl_get_last_send_context(char *stage, size_t stage_cap, unsigned long long *len, unsigned long long *offset, char *preview, size_t preview_cap);
int start_slave_thread(void);
int start_rdma_master_listener(void);
int repl_rdma_start_fullsync(conn_t *c);
void repl_rdma_stop_fullsync(void);
int repl_slaveof(const char *host, int port);
int repl_slaveof_noone(void);
const char *repl_master_link_state_name(void);
const char *repl_master_id(void);
extern volatile time_t g_last_write_ts;
unsigned long long repl_master_offset(void);
unsigned long long repl_connected_slaves(void);
unsigned long long repl_fullsync_count(void);
unsigned long long repl_partialsync_ok_count(void);
unsigned long long repl_partialsync_err_count(void);
unsigned long long repl_broadcast_bytes(void);
unsigned long long repl_snapshot_bytes(void);
unsigned long long repl_backlog_size(void);
unsigned long long repl_backlog_histlen(void);
unsigned long long repl_backlog_start_offset(void);
unsigned long long repl_backlog_end_offset(void);
int repl_rdma_effective_recv_slots(void);
int repl_rdma_effective_qp_wr_depth(void);
int repl_rdma_effective_chunk_size(void);
int repl_rdma_is_connected(void);
unsigned long long repl_rdma_disconnect_count(void);
unsigned long long repl_rdma_reject_count(void);
unsigned long long repl_rdma_send_cq_error_count(void);
unsigned long long repl_rdma_recv_cq_error_count(void);
void repl_note_fullsync(size_t snapshot_bytes);
void repl_note_broadcast(size_t bytes);
int repl_backlog_feed(const unsigned char *buf, size_t len);
int repl_backlog_can_continue(const char *replid, unsigned long long offset);
int repl_backlog_write_range(conn_t *c, unsigned long long offset);
int repl_backlog_send_continue(conn_t *c, unsigned long long offset);
void repl_note_partialsync_result(int ok);
void repl_slave_set_sync_state(const char *replid, unsigned long long applied_offset, unsigned long long durable_offset, int fullsync_loading, unsigned long long fullsync_target_bytes);
void repl_slave_finish_fullsync(void);
void repl_slave_note_applied(size_t rawlen);
void repl_slave_note_durable(size_t rawlen);
int repl_slave_send_ack(void);
void repl_replica_update_ack(conn_t *c, unsigned long long applied_offset, unsigned long long durable_offset);
const char *repl_slave_master_id(void);
unsigned long long repl_slave_offset(void);
unsigned long long repl_slave_applied_offset(void);
unsigned long long repl_slave_durable_offset(void);
int repl_slave_loading_fullsync(void);
int repl_slave_state_load(void);
int repl_slave_state_save(void);

int repl_ebpf_init(void);
void repl_ebpf_cleanup(void);
int repl_ebpf_supported(void);
int repl_ebpf_register_fd(int fd, int is_master_side);
int repl_ebpf_register_forward_fd(int fd);
int repl_ebpf_unregister_fd(int fd);
int repl_ebpf_get_stats(kvs_repl_ebpf_stats_t *stats);

int persist_init(void);
void persist_close(void);
int persist_append_raw(const unsigned char *buf, size_t len);
int persist_write_raw_fd(int fd, const unsigned char *buf, size_t len, long long *offset_io);
int persist_fsync_fd(int fd);
int persist_save_dump(void);
int persist_recover(void);
int persist_recover_in_progress(void);
int kvs_snapshot_to_fp(FILE *fp);
int kvs_snapshot_to_fd(int fd);
int kvs_dump_to_fd(int fd, unsigned long long aof_offset);
unsigned long long replay_dump_file(const char *path);
int kvs_load_dump_from_fd(int fd);
int persist_bgsave_start(void);
int persist_bgsave_poll(void);
int persist_bgsave_in_progress(void);
const char *persist_bgsave_state_name(void);
int persist_bgrewriteaof_start(void);
int persist_bgrewriteaof_poll(void);
int persist_bgrewriteaof_in_progress(void);
const char *persist_bgrewriteaof_state_name(void);
int persist_aof_disable(void);
int persist_set_aof_policy(kvs_aof_fsync_policy_t policy);
kvs_aof_fsync_policy_t persist_get_aof_policy(void);
const char *persist_aof_policy_name(void);
int persist_force_aof_flush(void);

void persist_note_write(void);
unsigned long long persist_dirty_count(void);
long long persist_last_snapshot_ms(void);
int persist_register_autosnap_rule(long long seconds, long long changes);
void persist_clear_autosnap_rules(void);
int persist_build_autosnap_text(char *buf, size_t cap);
int persist_autosnap_cron(void);
int persist_build_recover_text(char *buf, size_t cap);
int sentinel_start(void);

extern pid_t g_bgsave_pid;
extern long long g_bgsave_last_start_ms;
extern long long g_bgsave_last_end_ms;
extern unsigned long long g_dirty_counter;

int resp_simple_string(char *out, size_t cap, const char *s);
int resp_error(char *out, size_t cap, const char *s);
int resp_integer(char *out, size_t cap, long long v);
int resp_bulk(char *out, size_t cap, const char *s, size_t len);
int resp_null_bulk(char *out, size_t cap);
size_t resp_build_cmd4(unsigned char *out, size_t cap, const char *cmd, const char *a1, const char *a2, const char *a3);
size_t resp_build_cmd3(unsigned char *out, size_t cap, const char *cmd, const char *a1, const char *a2);
size_t resp_build_cmd2(unsigned char *out, size_t cap, const char *cmd, const char *a1);
size_t resp_build_cmd1(unsigned char *out, size_t cap, const char *cmd);

/* ---- kprobe+RDMA 增量同步 ---- */
struct ibv_pd; /* 前向声明，避免非 RDMA 编译单元警告 */
typedef struct {
    unsigned long long total_events;
    unsigned long long total_bytes;
    unsigned long long rdma_writes;
    unsigned long long rdma_errors;
    unsigned long long kprobe_hits;
    unsigned long long kprobe_bytes;
    int kprobe_initialized;
    int rdma_connected;
} kvs_repl_kprobe_stats_t;

int repl_kprobe_rdma_master_init(void);
int repl_kprobe_rdma_slave_init(void);
int repl_kprobe_rdma_establish(const char *host, int port);
int repl_kprobe_rdma_connect_mr(const char *host, int port, int tcp_fd);
int repl_kprobe_rdma_slave_accept(struct ibv_pd *pd, char *resp, size_t resp_cap);
void repl_kprobe_rdma_cleanup(void);
int repl_kprobe_rdma_get_stats(kvs_repl_kprobe_stats_t *stats);
int repl_kprobe_rdma_set_pid(pid_t pid);
int repl_kprobe_rdma_parse_mr_info(const char *resp);
void repl_kprobe_fullsync_done(void);
int repl_kprobe_rdma_get_mr_text(char *buf, size_t cap);
int repl_kprobe_rdma_parse_mr_info_direct(uint32_t rkey, uint64_t addr,
    size_t total_size, size_t slot_count, size_t slot_capacity);
int repl_kprobe_rdma_enqueue(const unsigned char *data, size_t len);

const char *repl_master_link_state_name(void);

#endif