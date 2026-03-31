#include "kvstore/kvstore.h"

FILE *g_aof_fp = NULL;

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
    FILE *fp = fopen(g_cfg.dump_path, "wb");
    if (!fp) return -1;
    int rc = kvs_snapshot_to_fp(fp);
    fclose(fp);
    return rc;
}

int persist_recover(void) {
    replay_file(g_cfg.dump_path);
    replay_file(g_cfg.aof_path);
    return 0;
}
