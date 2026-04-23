#include "kvstore/kvstore.h"

#define KVS_REPL_BACKLOG_SIZE (1024 * 1024)

typedef struct repl_backlog_s {
    unsigned char *buf;
    size_t cap;
    size_t histlen;
    size_t head;
    unsigned long long start_offset;
    unsigned long long end_offset;
} repl_backlog_t;

static pthread_mutex_t g_slave_conf_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_slave_host[128] = "";
static int g_slave_port = 0;
static int g_slave_conf_gen = 0;
static int g_master_link_up = 0;
static long long g_master_last_io_ms = 0;
static int g_slave_thread_started = 0;
static char g_master_replid[41] = {0};
static unsigned long long g_master_repl_offset = 0;
static unsigned long long g_repl_fullsync_count = 0;
static unsigned long long g_repl_partialsync_ok_count = 0;
static unsigned long long g_repl_partialsync_err_count = 0;
static unsigned long long g_repl_broadcast_bytes = 0;
static unsigned long long g_repl_snapshot_bytes = 0;
static repl_backlog_t g_repl_backlog = {0};
static char g_slave_master_replid[41] = "?";
static unsigned long long g_slave_repl_offset = 0;
static int g_slave_loading_fullsync = 0;
static char g_slave_state_path[512] = {0};

static void build_slave_state_path(void) {
    if (g_slave_state_path[0]) return;
    snprintf(g_slave_state_path, sizeof(g_slave_state_path), "%s.replstate", g_cfg.aof_path);
}


void repl_note_fullsync(size_t snapshot_bytes) {
    g_repl_fullsync_count++;
    g_repl_snapshot_bytes += (unsigned long long)snapshot_bytes;
}

static int ensure_repl_backlog(void) {
    if (g_repl_backlog.buf) return 0;
    g_repl_backlog.buf = (unsigned char *)kvs_malloc(KVS_REPL_BACKLOG_SIZE);
    if (!g_repl_backlog.buf) return -1;
    g_repl_backlog.cap = KVS_REPL_BACKLOG_SIZE;
    g_repl_backlog.histlen = 0;
    g_repl_backlog.head = 0;
    g_repl_backlog.start_offset = g_master_repl_offset;
    g_repl_backlog.end_offset = g_master_repl_offset;
    return 0;
}

int repl_backlog_feed(const unsigned char *buf, size_t len) {
    if (!buf || len == 0) return 0;
    if (ensure_repl_backlog() != 0) return -1;
    if (len >= g_repl_backlog.cap) {
        buf += len - g_repl_backlog.cap;
        len = g_repl_backlog.cap;
        memcpy(g_repl_backlog.buf, buf, len);
        g_repl_backlog.head = 0;
        g_repl_backlog.histlen = len;
        g_repl_backlog.end_offset += len;
        g_repl_backlog.start_offset = g_repl_backlog.end_offset - g_repl_backlog.histlen;
        return 0;
    }

    size_t tail = (g_repl_backlog.head + g_repl_backlog.histlen) % g_repl_backlog.cap;
    size_t first = g_repl_backlog.cap - tail;
    if (first > len) first = len;
    memcpy(g_repl_backlog.buf + tail, buf, first);
    if (len > first) memcpy(g_repl_backlog.buf, buf + first, len - first);

    if (g_repl_backlog.histlen + len <= g_repl_backlog.cap) {
        g_repl_backlog.histlen += len;
    } else {
        size_t overflow = g_repl_backlog.histlen + len - g_repl_backlog.cap;
        g_repl_backlog.head = (g_repl_backlog.head + overflow) % g_repl_backlog.cap;
        g_repl_backlog.histlen = g_repl_backlog.cap;
    }

    g_repl_backlog.end_offset += len;
    g_repl_backlog.start_offset = g_repl_backlog.end_offset - g_repl_backlog.histlen;
    return 0;
}

void repl_note_broadcast(size_t bytes) {
    g_repl_broadcast_bytes += (unsigned long long)bytes;
    g_master_repl_offset += (unsigned long long)bytes;
}

static void ensure_master_replid(void) {
    unsigned int a, b, c, d, e;
    if (g_master_replid[0]) return;
    a = (unsigned int)(kvs_now_ms() & 0xffffffffu);
    b = (unsigned int)getpid();
    c = (unsigned int)((uintptr_t)&g_master_replid & 0xffffffffu);
    d = (unsigned int)(time(NULL) & 0xffffffffu);
    e = (unsigned int)((uintptr_t)pthread_self() & 0xffffffffu);
    snprintf(g_master_replid, sizeof(g_master_replid), "%08x%08x%08x%08x%08x", a, b, c, d, e);
    g_master_replid[40] = '\0';
}

static void repl_set_link_state(int up) {
    pthread_mutex_lock(&g_slave_conf_lock);
    g_master_link_up = up;
    if (up) g_master_last_io_ms = kvs_now_ms();
    pthread_mutex_unlock(&g_slave_conf_lock);
}

int repl_slaveof(const char *host, int port) {
    if (!host || port <= 0) return -1;
    pthread_mutex_lock(&g_slave_conf_lock);
    snprintf(g_slave_host, sizeof(g_slave_host), "%s", host);
    g_slave_port = port;
    g_cfg.role = ROLE_SLAVE;
    snprintf(g_cfg.master_host, sizeof(g_cfg.master_host), "%s", host);
    g_cfg.master_port = port;
    g_slave_conf_gen++;
    g_master_link_up = 0;
    pthread_mutex_unlock(&g_slave_conf_lock);
    repl_slave_state_save();
    return 0;
}

int repl_slaveof_noone(void) {
    pthread_mutex_lock(&g_slave_conf_lock);
    g_slave_host[0] = '\0';
    g_slave_port = 0;
    g_cfg.role = ROLE_MASTER;
    g_cfg.master_host[0] = '\0';
    g_cfg.master_port = 0;
    g_slave_conf_gen++;
    g_master_link_up = 0;
    pthread_mutex_unlock(&g_slave_conf_lock);
    repl_slave_state_save();
    return 0;
}

int repl_get_master_addr(char *host, size_t cap, int *port) {
    if (!host || cap == 0 || !port) return -1;
    pthread_mutex_lock(&g_slave_conf_lock);
    snprintf(host, cap, "%s", g_slave_host);
    *port = g_slave_port;
    pthread_mutex_unlock(&g_slave_conf_lock);
    return 0;
}

int repl_is_master_link_up(void) {
    int up;
    pthread_mutex_lock(&g_slave_conf_lock);
    up = g_master_link_up;
    pthread_mutex_unlock(&g_slave_conf_lock);
    return up;
}

const char *repl_master_link_state_name(void) {
    return repl_is_master_link_up() ? "up" : "down";
}

const char *repl_master_id(void) {
    ensure_master_replid();
    return g_master_replid;
}

unsigned long long repl_master_offset(void) {
    return g_master_repl_offset;
}

unsigned long long repl_connected_slaves(void) {
    unsigned long long n = 0;
    pthread_mutex_lock(&g_repl_lock);
    for (conn_t *c = g_replicas; c; c = c->next_replica) n++;
    pthread_mutex_unlock(&g_repl_lock);
    return n;
}

unsigned long long repl_fullsync_count(void) {
    return g_repl_fullsync_count;
}

unsigned long long repl_partialsync_ok_count(void) {
    return g_repl_partialsync_ok_count;
}

unsigned long long repl_partialsync_err_count(void) {
    return g_repl_partialsync_err_count;
}

unsigned long long repl_broadcast_bytes(void) {
    return g_repl_broadcast_bytes;
}

unsigned long long repl_snapshot_bytes(void) {
    return g_repl_snapshot_bytes;
}

unsigned long long repl_backlog_size(void) {
    return (unsigned long long)g_repl_backlog.cap;
}

unsigned long long repl_backlog_histlen(void) {
    return (unsigned long long)g_repl_backlog.histlen;
}

unsigned long long repl_backlog_start_offset(void) {
    return g_repl_backlog.start_offset;
}

unsigned long long repl_backlog_end_offset(void) {
    return g_repl_backlog.end_offset;
}

void repl_note_partialsync_result(int ok) {
    if (ok) g_repl_partialsync_ok_count++;
    else g_repl_partialsync_err_count++;
}

void repl_slave_set_sync_state(const char *replid, unsigned long long offset, int fullsync_loading) {
    if (replid && *replid) {
        snprintf(g_slave_master_replid, sizeof(g_slave_master_replid), "%s", replid);
    }
    g_slave_repl_offset = offset;
    g_slave_loading_fullsync = fullsync_loading;
    repl_slave_state_save();
}

void repl_slave_finish_fullsync(void) {
    g_slave_loading_fullsync = 0;
    repl_slave_state_save();
}

void repl_slave_note_applied(size_t rawlen) {
    if (!g_slave_loading_fullsync) {
        g_slave_repl_offset += (unsigned long long)rawlen;
        repl_slave_state_save();
    }
}

const char *repl_slave_master_id(void) {
    return g_slave_master_replid;
}

unsigned long long repl_slave_offset(void) {
    return g_slave_repl_offset;
}

int repl_slave_state_load(void) {
    FILE *fp;
    char replid[41] = {0};
    unsigned long long offset = 0;
    build_slave_state_path();
    fp = fopen(g_slave_state_path, "r");
    if (!fp) return 0;
    if (fscanf(fp, "%40s %llu", replid, &offset) == 2) {
        snprintf(g_slave_master_replid, sizeof(g_slave_master_replid), "%s", replid);
        g_slave_repl_offset = offset;
    }
    fclose(fp);
    return 0;
}

int repl_slave_state_save(void) {
    FILE *fp;
    build_slave_state_path();
    fp = fopen(g_slave_state_path, "w");
    if (!fp) return -1;
    fprintf(fp, "%s %llu\n", g_slave_master_replid[0] ? g_slave_master_replid : "?", g_slave_repl_offset);
    fclose(fp);
    return 0;
}

int repl_backlog_can_continue(const char *replid, unsigned long long offset) {
    unsigned long long want_offset;
    ensure_master_replid();
    if (!replid || strcmp(replid, g_master_replid) != 0) return 0;
    if (!g_repl_backlog.buf) return 0;
    want_offset = offset;
    if (want_offset > g_repl_backlog.end_offset) return 0;
    return want_offset >= g_repl_backlog.start_offset;
}

int repl_backlog_write_range(conn_t *c, unsigned long long offset) {
    size_t delta, start_index, first;
    if (!c || !g_repl_backlog.buf) return -1;
    if (offset < g_repl_backlog.start_offset || offset > g_repl_backlog.end_offset) return -1;
    delta = (size_t)(offset - g_repl_backlog.start_offset);
    start_index = (g_repl_backlog.head + delta) % g_repl_backlog.cap;
    first = g_repl_backlog.histlen - delta;
    if (first == 0) return 0;
    if (start_index + first <= g_repl_backlog.cap) {
        queue_bytes(c, g_repl_backlog.buf + start_index, first);
    } else {
        size_t part1 = g_repl_backlog.cap - start_index;
        size_t part2 = first - part1;
        queue_bytes(c, g_repl_backlog.buf + start_index, part1);
        queue_bytes(c, g_repl_backlog.buf, part2);
    }
    c->repl_offset_sent = g_repl_backlog.end_offset;
    c->repl_last_send_ms = kvs_now_ms();
    return 0;
}

static int snapshot_slave_conf(char *host, size_t cap, int *port, int *gen, int *role) {
    if (!host || !port || !gen || !role) return -1;
    pthread_mutex_lock(&g_slave_conf_lock);
    snprintf(host, cap, "%s", g_slave_host);
    *port = g_slave_port;
    *gen = g_slave_conf_gen;
    *role = g_cfg.role;
    pthread_mutex_unlock(&g_slave_conf_lock);
    return 0;
}

static int slave_should_reconnect(int local_gen) {
    int changed = 0;
    pthread_mutex_lock(&g_slave_conf_lock);
    if (g_cfg.role != ROLE_SLAVE || local_gen != g_slave_conf_gen) changed = 1;
    pthread_mutex_unlock(&g_slave_conf_lock);
    return changed;
}

static void *slave_thread(void *arg) {
    (void)arg;
    for (;;) {
        char host[128];
        int port = 0;
        int gen = 0;
        int role = ROLE_MASTER;
        snapshot_slave_conf(host, sizeof(host), &port, &gen, &role);

        if (role != ROLE_SLAVE || host[0] == '\0' || port <= 0) {
            repl_set_link_state(0);
            sleep(1);
            continue;
        }

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            sleep(1);
            continue;
        }

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
            close(fd);
            sleep(1);
            continue;
        }

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            repl_set_link_state(0);
            sleep(1);
            continue;
        }

        unsigned char cmd[256];
        char offbuf[32];
        snprintf(offbuf, sizeof(offbuf), "%llu", g_slave_repl_offset);
        size_t n = resp_build_cmd3(cmd, sizeof(cmd), "REPLSYNC", g_slave_master_replid[0] ? g_slave_master_replid : "?", offbuf);
        if (send(fd, cmd, n, 0) < 0) {
            close(fd);
            repl_set_link_state(0);
            sleep(1);
            continue;
        }

        repl_set_link_state(1);
        unsigned char buf[BUFFER_CAP];
        size_t blen = 0;

        for (;;) {
            if (slave_should_reconnect(gen)) break;
            ssize_t r = recv(fd, buf + blen, sizeof(buf) - blen, 0);
            if (r > 0) {
                blen += (size_t)r;
                parse_resp_stream(NULL, buf, &blen, 1);
                repl_set_link_state(1);
                continue;
            }
            if (r == 0) break;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }

        close(fd);
        repl_set_link_state(0);
        sleep(1);
    }
    return NULL;
}

int start_slave_thread(void) {
    pthread_t tid;
    if (g_slave_thread_started) return 0;
    if (g_cfg.role == ROLE_SLAVE && g_cfg.master_host[0] && g_cfg.master_port > 0) {
        repl_slaveof(g_cfg.master_host, g_cfg.master_port);
    }
    if (pthread_create(&tid, NULL, slave_thread, NULL) != 0) return -1;
    pthread_detach(tid);
    g_slave_thread_started = 1;
    return 0;
}
