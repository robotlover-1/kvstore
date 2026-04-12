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

static int persist_save_dump_to(const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    int rc = kvs_snapshot_to_fp(fp);
    fclose(fp);
    return rc;
}

static void persist_mark_snapshot_success(unsigned long long snap_dirty) {
    g_bgsave_last_end_ms = kvs_now_ms();
    g_last_snapshot_ms = g_bgsave_last_end_ms;
    if (g_dirty_counter >= snap_dirty) g_dirty_counter -= snap_dirty;
    else g_dirty_counter = 0;
}

int persist_init(void) {
    g_aof_fp = fopen(g_cfg.aof_path, "ab+");
    return g_aof_fp ? 0 : -1;
}

void persist_close(void) {
    if (g_aof_fp) fclose(g_aof_fp);
    g_aof_fp = NULL;
}

int persist_append_raw(const unsigned char *buf, size_t len) {
    if (!g_aof_fp) return -1;
    if (fwrite(buf, 1, len, g_aof_fp) != len) return -1;
    fflush(g_aof_fp);
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
        "bgsave_pid=%ld\n",
        g_cfg.autosnap_rule_count,
        (unsigned long long)g_dirty_counter,
        g_last_snapshot_ms,
        persist_bgsave_state_name(),
        (long)g_bgsave_pid);
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
    persist_bgsave_poll();
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
