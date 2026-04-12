#include "kvstore/kvstore.h"
#include <sys/wait.h>
#include <signal.h>

FILE *g_aof_fp = NULL;
pid_t g_bgsave_pid = -1;
long long g_bgsave_last_start_ms = 0;
long long g_bgsave_last_end_ms = 0;
unsigned long long g_dirty_counter = 0;

static int g_bgsave_status = 0; /* 0 idle, 1 running, 2 ok, 3 err */
static unsigned long long g_bgsave_base_dirty = 0;
static long long g_last_snapshot_ms = 0;

static int g_aof_dirty = 0;
static long long g_aof_last_flush_ms = 0;

static pid_t g_bgrewrite_pid = -1;
static int g_bgrewrite_status = 0; /* 0 idle, 1 running, 2 ok, 3 err */
static char g_rewrite_tmp_path[512] = {0};

typedef struct rewrite_buf_node_s {
    unsigned char *data;
    size_t len;
    struct rewrite_buf_node_s *next;
} rewrite_buf_node_t;

static rewrite_buf_node_t *g_rewrite_buf_head = NULL;
static rewrite_buf_node_t *g_rewrite_buf_tail = NULL;
static pthread_mutex_t g_rewrite_buf_lock = PTHREAD_MUTEX_INITIALIZER;

static int replay_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    unsigned char buf[BUFFER_CAP];
    size_t len = 0, n;
    while ((n = fread(buf + len, 1, sizeof(buf) - len, fp)) > 0) {
        len += n;
        parse_resp_stream(NULL, buf, &len, 1);
    }
    fclose(fp);
    return 0;
}

static int persist_flush_aof_fp(FILE *fp) {
    if (!fp) return -1;
    if (fflush(fp) != 0) return -1;
    if (fsync(fileno(fp)) != 0) return -1;
    return 0;
}

static int persist_save_dump_to(const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    int rc = kvs_snapshot_to_fp(fp);
    if (rc == 0 && fflush(fp) != 0) rc = -1;
    if (rc == 0 && fsync(fileno(fp)) != 0) rc = -1;
    fclose(fp);
    return rc;
}

static int persist_write_aof_snapshot_to(const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    int rc = kvs_snapshot_to_fp(fp);
    if (rc == 0 && fflush(fp) != 0) rc = -1;
    if (rc == 0 && fsync(fileno(fp)) != 0) rc = -1;
    fclose(fp);
    return rc;
}

static void persist_mark_snapshot_success(unsigned long long snap_dirty) {
    g_bgsave_last_end_ms = kvs_now_ms();
    g_last_snapshot_ms = g_bgsave_last_end_ms;
    if (g_dirty_counter >= snap_dirty) g_dirty_counter -= snap_dirty;
    else g_dirty_counter = 0;
}

static void free_rewrite_buffer_locked(void) {
    rewrite_buf_node_t *cur = g_rewrite_buf_head;
    while (cur) {
        rewrite_buf_node_t *next = cur->next;
        kvs_free(cur->data);
        kvs_free(cur);
        cur = next;
    }
    g_rewrite_buf_head = NULL;
    g_rewrite_buf_tail = NULL;
}

static int append_to_rewrite_buffer(const unsigned char *buf, size_t len) {
    rewrite_buf_node_t *node = (rewrite_buf_node_t *)kvs_malloc(sizeof(*node));
    if (!node) return -1;
    node->data = (unsigned char *)kvs_malloc(len);
    if (!node->data) {
        kvs_free(node);
        return -1;
    }
    memcpy(node->data, buf, len);
    node->len = len;
    node->next = NULL;

    pthread_mutex_lock(&g_rewrite_buf_lock);
    if (g_bgrewrite_pid <= 0) {
        pthread_mutex_unlock(&g_rewrite_buf_lock);
        kvs_free(node->data);
        kvs_free(node);
        return 0;
    }
    if (g_rewrite_buf_tail) g_rewrite_buf_tail->next = node;
    else g_rewrite_buf_head = node;
    g_rewrite_buf_tail = node;
    pthread_mutex_unlock(&g_rewrite_buf_lock);
    return 0;
}

static int finalize_rewrite_parent(void) {
    FILE *fp = fopen(g_rewrite_tmp_path, "ab");
    if (!fp) return -1;

    pthread_mutex_lock(&g_rewrite_buf_lock);
    for (rewrite_buf_node_t *cur = g_rewrite_buf_head; cur; cur = cur->next) {
        if (fwrite(cur->data, 1, cur->len, fp) != cur->len) {
            pthread_mutex_unlock(&g_rewrite_buf_lock);
            fclose(fp);
            return -1;
        }
    }
    pthread_mutex_unlock(&g_rewrite_buf_lock);

    if (persist_flush_aof_fp(fp) != 0) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    if (rename(g_rewrite_tmp_path, g_cfg.aof_path) != 0) return -1;

    if (g_aof_fp) fclose(g_aof_fp);
    g_aof_fp = fopen(g_cfg.aof_path, "ab+");
    if (!g_aof_fp) return -1;

    pthread_mutex_lock(&g_rewrite_buf_lock);
    free_rewrite_buffer_locked();
    pthread_mutex_unlock(&g_rewrite_buf_lock);

    g_aof_dirty = 0;
    g_aof_last_flush_ms = kvs_now_ms();
    return 0;
}

int persist_init(void) {
    g_aof_fp = fopen(g_cfg.aof_path, "ab+");
    g_aof_last_flush_ms = kvs_now_ms();
    return g_aof_fp ? 0 : -1;
}

void persist_close(void) {
    if (g_aof_fp) {
        persist_flush_aof_fp(g_aof_fp);
        fclose(g_aof_fp);
    }
    g_aof_fp = NULL;
}

int persist_set_aof_policy(kvs_aof_fsync_policy_t policy) {
    if (policy != KVS_AOF_FSYNC_ALWAYS && policy != KVS_AOF_FSYNC_EVERYSEC) return -1;
    g_cfg.aof_fsync = policy;
    return 0;
}

kvs_aof_fsync_policy_t persist_get_aof_policy(void) {
    return g_cfg.aof_fsync;
}

const char *persist_aof_policy_name(void) {
    return g_cfg.aof_fsync == KVS_AOF_FSYNC_EVERYSEC ? "everysec" : "always";
}

int persist_force_aof_flush(void) {
    if (!g_aof_fp) return -1;
    if (persist_flush_aof_fp(g_aof_fp) != 0) return -1;
    g_aof_dirty = 0;
    g_aof_last_flush_ms = kvs_now_ms();
    return 0;
}

int persist_append_raw(const unsigned char *buf, size_t len) {
    if (!g_aof_fp) return -1;
    if (fwrite(buf, 1, len, g_aof_fp) != len) return -1;
    g_aof_dirty = 1;

    if (g_bgrewrite_pid > 0) append_to_rewrite_buffer(buf, len);

    if (g_cfg.aof_fsync == KVS_AOF_FSYNC_ALWAYS) {
        if (persist_force_aof_flush() != 0) return -1;
    }
    return 0;
}

int persist_save_dump(void) {
    int rc = persist_save_dump_to(g_cfg.dump_path);
    if (rc == 0) persist_mark_snapshot_success(g_dirty_counter);
    return rc;
}

int persist_recover(void) {
    replay_file(g_cfg.dump_path);
    replay_file(g_cfg.aof_path);
    g_dirty_counter = 0;
    g_last_snapshot_ms = kvs_now_ms();
    g_bgsave_last_end_ms = g_last_snapshot_ms;
    g_aof_last_flush_ms = g_last_snapshot_ms;
    g_aof_dirty = 0;
    return 0;
}

int persist_bgsave_start(void) {
    if (g_bgsave_pid > 0) return 1;

    unsigned long long snap_dirty = g_dirty_counter;
    long long start_ms = kvs_now_ms();
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", g_cfg.dump_path, (long)getpid());

    pid_t pid = fork();
    if (pid < 0) {
        g_bgsave_status = 3;
        return -1;
    }
    if (pid == 0) {
        int rc = persist_save_dump_to(tmp_path);
        if (rc == 0 && rename(tmp_path, g_cfg.dump_path) != 0) rc = -1;
        if (rc != 0) unlink(tmp_path);
        _exit(rc == 0 ? 0 : 1);
    }

    g_bgsave_pid = pid;
    g_bgsave_status = 1;
    g_bgsave_last_start_ms = start_ms;
    g_bgsave_base_dirty = snap_dirty;
    return 0;
}

int persist_bgsave_poll(void) {
    if (g_bgsave_pid <= 0) return 0;
    int status = 0;
    pid_t rc = waitpid(g_bgsave_pid, &status, WNOHANG);
    if (rc == 0) return 0;
    if (rc < 0) {
        g_bgsave_status = 3;
        g_bgsave_pid = -1;
        g_bgsave_last_end_ms = kvs_now_ms();
        return -1;
    }

    g_bgsave_last_end_ms = kvs_now_ms();
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        g_bgsave_status = 2;
        persist_mark_snapshot_success(g_bgsave_base_dirty);
    } else {
        g_bgsave_status = 3;
    }
    g_bgsave_pid = -1;
    return 1;
}

int persist_bgsave_in_progress(void) {
    return g_bgsave_pid > 0 ? 1 : 0;
}

const char *persist_bgsave_state_name(void) {
    switch (g_bgsave_status) {
        case 1: return "running";
        case 2: return "ok";
        case 3: return "err";
        default: return "idle";
    }
}

int persist_bgrewriteaof_start(void) {
    if (g_bgrewrite_pid > 0) return 1;

    if (persist_force_aof_flush() != 0 && g_aof_fp) return -1;

    snprintf(g_rewrite_tmp_path, sizeof(g_rewrite_tmp_path), "%s.rewrite.tmp.%ld", g_cfg.aof_path, (long)getpid());

    pthread_mutex_lock(&g_rewrite_buf_lock);
    free_rewrite_buffer_locked();
    pthread_mutex_unlock(&g_rewrite_buf_lock);

    pid_t pid = fork();
    if (pid < 0) {
        g_bgrewrite_status = 3;
        g_rewrite_tmp_path[0] = '\0';
        return -1;
    }
    if (pid == 0) {
        int rc = persist_write_aof_snapshot_to(g_rewrite_tmp_path);
        _exit(rc == 0 ? 0 : 1);
    }

    g_bgrewrite_pid = pid;
    g_bgrewrite_status = 1;
    return 0;
}

int persist_bgrewriteaof_poll(void) {
    if (g_bgrewrite_pid <= 0) return 0;

    int status = 0;
    pid_t rc = waitpid(g_bgrewrite_pid, &status, WNOHANG);
    if (rc == 0) return 0;
    if (rc < 0) {
        g_bgrewrite_status = 3;
        g_bgrewrite_pid = -1;
        unlink(g_rewrite_tmp_path);
        g_rewrite_tmp_path[0] = '\0';
        return -1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        if (finalize_rewrite_parent() == 0) g_bgrewrite_status = 2;
        else {
            g_bgrewrite_status = 3;
            unlink(g_rewrite_tmp_path);
        }
    } else {
        g_bgrewrite_status = 3;
        unlink(g_rewrite_tmp_path);
    }

    g_bgrewrite_pid = -1;
    g_rewrite_tmp_path[0] = '\0';
    return 1;
}

int persist_bgrewriteaof_in_progress(void) {
    return g_bgrewrite_pid > 0 ? 1 : 0;
}

const char *persist_bgrewriteaof_state_name(void) {
    switch (g_bgrewrite_status) {
        case 1: return "running";
        case 2: return "ok";
        case 3: return "err";
        default: return "idle";
    }
}

void persist_note_write(void) {
    g_dirty_counter++;
}

unsigned long long persist_dirty_count(void) {
    return g_dirty_counter;
}

long long persist_last_snapshot_ms(void) {
    return g_last_snapshot_ms;
}

int persist_register_autosnap_rule(long long seconds, long long changes) {
    if (seconds <= 0 || changes <= 0) return -1;
    for (int i = 0; i < g_cfg.autosnap_rule_count; ++i) {
        if (g_cfg.autosnap_rules[i].seconds == seconds) {
            g_cfg.autosnap_rules[i].changes = changes;
            return 0;
        }
    }
    if (g_cfg.autosnap_rule_count >= KVS_AUTOSNAP_RULES_MAX) return -1;
    g_cfg.autosnap_rules[g_cfg.autosnap_rule_count].seconds = seconds;
    g_cfg.autosnap_rules[g_cfg.autosnap_rule_count].changes = changes;
    g_cfg.autosnap_rule_count++;
    return 0;
}

void persist_clear_autosnap_rules(void) {
    g_cfg.autosnap_rule_count = 0;
    memset(g_cfg.autosnap_rules, 0, sizeof(g_cfg.autosnap_rules));
}

int persist_build_autosnap_text(char *buf, size_t cap) {
    int n = snprintf(buf, cap,
        "autosnap_rules=%d\n"
        "dirty=%llu\n"
        "last_snapshot_ms=%lld\n"
        "bgsave=%s\n"
        "bgsave_pid=%ld\n"
        "aof_fsync=%s\n"
        "aof_rewrite=%s\n"
        "aof_rewrite_pid=%ld\n",
        g_cfg.autosnap_rule_count,
        (unsigned long long)g_dirty_counter,
        g_last_snapshot_ms,
        persist_bgsave_state_name(),
        (long)g_bgsave_pid,
        persist_aof_policy_name(),
        persist_bgrewriteaof_state_name(),
        (long)g_bgrewrite_pid);
    if (n < 0 || (size_t)n >= cap) return -1;
    size_t pos = (size_t)n;
    for (int i = 0; i < g_cfg.autosnap_rule_count; ++i) {
        n = snprintf(buf + pos, cap - pos, "rule_%d=%lld:%lld\n", i,
            g_cfg.autosnap_rules[i].seconds, g_cfg.autosnap_rules[i].changes);
        if (n < 0 || (size_t)n >= cap - pos) return -1;
        pos += (size_t)n;
    }
    return (int)pos;
}

int persist_autosnap_cron(void) {
    if (g_cfg.aof_fsync == KVS_AOF_FSYNC_EVERYSEC && g_aof_dirty) {
        long long now = kvs_now_ms();
        if (now - g_aof_last_flush_ms >= 1000) {
            persist_force_aof_flush();
        }
    }

    persist_bgsave_poll();
    persist_bgrewriteaof_poll();

    if (g_cfg.role != ROLE_MASTER) return 0;
    if (g_bgsave_pid > 0) return 0;
    if (g_cfg.autosnap_rule_count <= 0) return 0;

    long long now = kvs_now_ms();
    long long last_ms = g_last_snapshot_ms > 0 ? g_last_snapshot_ms : g_bgsave_last_end_ms;
    if (last_ms <= 0) last_ms = now;

    for (int i = 0; i < g_cfg.autosnap_rule_count; ++i) {
        long long sec = g_cfg.autosnap_rules[i].seconds;
        long long changes = g_cfg.autosnap_rules[i].changes;
        if ((long long)g_dirty_counter >= changes && now - last_ms >= sec * 1000) {
            return persist_bgsave_start();
        }
    }
    return 0;
}
