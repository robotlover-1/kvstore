#include "kvstore/kvstore.h"

static pthread_mutex_t g_slave_conf_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_slave_host[128] = "";
static int g_slave_port = 0;
static int g_slave_conf_gen = 0;
static int g_master_link_up = 0;
static long long g_master_last_io_ms = 0;
static int g_slave_thread_started = 0;

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

        unsigned char cmd[128];
        size_t n = resp_build_cmd1(cmd, sizeof(cmd), "REPLSYNC");
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
