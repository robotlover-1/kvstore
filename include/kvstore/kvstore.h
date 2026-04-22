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
#include <sys/stat.h>
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

typedef int (*msg_handler)(char *msg, int length, char *response);

typedef enum {
    KVS_AOF_FSYNC_ALWAYS = 0,
    KVS_AOF_FSYNC_EVERYSEC = 1,
} kvs_aof_fsync_policy_t;

/* ... existing content unchanged ... */

extern kv_config_t g_cfg;
extern int g_epfd;
extern conn_t *g_replicas;
extern pthread_mutex_t g_repl_lock;
extern FILE *g_aof_fp;

int kvs_validate_config(char *err, size_t cap);

void *kvs_malloc(size_t size);
void *kvs_calloc(size_t n, size_t size);
void *kvs_realloc(void *ptr, size_t size);
void kvs_free(void *ptr);
long long kvs_now_ms(void);
int kvs_mem_prepare_process(const char *backend_name, char *argv0, char **argv);
int kvs_mem_init(const char *backend_name);
const char *kvs_mem_backend_name(void);
int kvs_mem_get_stats(kvs_mem_stats_t *stats);

/* rest unchanged */

#endif
