#include "kvstore/kvstore.h"

FILE *g_aof_fp = NULL;
pid_t g_bgsave_pid = -1;
int g_bgsave_state = BGSAVE_IDLE;
long long g_bgsave_last_start_ms = 0;
long long g_bgsave_last_end_ms = 0;
unsigned long long g_bgsave_seq = 0;
unsigned long long g_bgsave_done_seq = 0;

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

static void build_tmp_dump_path(char *out, size_t cap) {
    snprintf(out, cap, "%s.tmp.%ld", g_cfg.dump_path, (long)getpid());
}

static int persist_save_dump_to(const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    int rc = kvs_snapshot_to_fp(fp);
    if (fflush(fp) != 0) rc = -1;
    fclose(fp);
    return rc;
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
    return persist_save_dump_to(g_cfg.dump_path);
}

int persist_bgsave_in_progress(void) {
    return g_bgsave_state == BGSAVE_RUNNING;
}

const char *persist_bgsave_state_name(void) {
    switch (g_bgsave_state) {
        case BGSAVE_RUNNING: return "running";
        case BGSAVE_OK: return "ok";
        case BGSAVE_ERR: return "err";
        default: return "idle";
    }
}

int persist_bgsave_start(void) {
    if (persist_bgsave_in_progress()) return 1;

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        char tmp_path[512];
        build_tmp_dump_path(tmp_path, sizeof(tmp_path));

        if (g_aof_fp) {
            fclose(g_aof_fp);
            g_aof_fp = NULL;
        }

        int rc = persist_save_dump_to(tmp_path);
        if (rc == 0) {
            if (rename(tmp_path, g_cfg.dump_path) != 0) {
                unlink(tmp_path);
                _exit(1);
            }
            _exit(0);
        }

        unlink(tmp_path);
        _exit(1);
    }

    g_bgsave_pid = pid;
    g_bgsave_state = BGSAVE_RUNNING;
    g_bgsave_last_start_ms = kvs_now_ms();
    g_bgsave_seq++;
    return 0;
}

void persist_bgsave_poll(void) {
    if (!persist_bgsave_in_progress() || g_bgsave_pid <= 0) return;

    int status = 0;
    pid_t rc = waitpid(g_bgsave_pid, &status, WNOHANG);
    if (rc == 0) return;
    if (rc < 0) {
        if (errno == EINTR) return;
        g_bgsave_state = BGSAVE_ERR;
        g_bgsave_pid = -1;
        g_bgsave_last_end_ms = kvs_now_ms();
        return;
    }

    g_bgsave_last_end_ms = kvs_now_ms();
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        g_bgsave_state = BGSAVE_OK;
        g_bgsave_done_seq = g_bgsave_seq;
    } else {
        g_bgsave_state = BGSAVE_ERR;
    }
    g_bgsave_pid = -1;
}

int persist_recover(void) {
    replay_file(g_cfg.dump_path);
    replay_file(g_cfg.aof_path);
    return 0;
}
