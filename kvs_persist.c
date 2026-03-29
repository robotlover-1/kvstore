#include "kvstore.h"

static kvs_dataset_table_t g_dataset = {0};
static char g_dump_path[256] = "kvstore.dump";
static char g_aof_path[256] = "kvstore.aof";
static int g_replaying = 0;

static unsigned int dataset_hash(int engine, const char *key) {
    unsigned int h = 2166136261u;
    while (*key) { h ^= (unsigned char)(*key++); h *= 16777619u; }
    h ^= (unsigned int)engine;
    return h % KVS_PERSIST_BUCKETS;
}

static kvs_dataset_item_t *dataset_find(int engine, const char *key) {
    if (!g_dataset.buckets || !key) return NULL;
    unsigned int idx = dataset_hash(engine, key);
    kvs_dataset_item_t *cur = g_dataset.buckets[idx];
    while (cur) {
        if (cur->engine == engine && strcmp(cur->key, key) == 0) return cur;
        cur = cur->next;
    }
    return NULL;
}

static void dataset_free_all(void) {
    if (!g_dataset.buckets) return;
    for (int i = 0; i < g_dataset.size; ++i) {
        kvs_dataset_item_t *cur = g_dataset.buckets[i];
        while (cur) {
            kvs_dataset_item_t *next = cur->next;
            kvs_free(cur->key);
            kvs_free(cur->value);
            kvs_free(cur);
            cur = next;
        }
    }
    kvs_free(g_dataset.buckets);
    memset(&g_dataset, 0, sizeof(g_dataset));
}

static int write_resp_bulk(FILE *fp, const char *s) {
    if (!s) s = "";
    return fprintf(fp, "$%zu\r\n%s\r\n", strlen(s), s) < 0 ? -1 : 0;
}

static int write_resp_cmd(FILE *fp, int argc, const char **argv) {
    if (fprintf(fp, "*%d\r\n", argc) < 0) return -1;
    for (int i = 0; i < argc; ++i) if (write_resp_bulk(fp, argv[i]) < 0) return -1;
    return 0;
}

int kvs_persist_init(const char *dump_path, const char *aof_path) {
    g_dataset.buckets = calloc(KVS_PERSIST_BUCKETS, sizeof(kvs_dataset_item_t*));
    if (!g_dataset.buckets) return -1;
    g_dataset.size = KVS_PERSIST_BUCKETS;
    if (dump_path) snprintf(g_dump_path, sizeof(g_dump_path), "%s", dump_path);
    if (aof_path) snprintf(g_aof_path, sizeof(g_aof_path), "%s", aof_path);
    return 0;
}

void kvs_persist_shutdown(void) {
    dataset_free_all();
}

void kvs_dataset_set(int engine, const char *key, const char *value) {
    if (!key || !value) return;
    kvs_dataset_item_t *item = dataset_find(engine, key);
    if (!item) {
        unsigned int idx = dataset_hash(engine, key);
        item = kvs_malloc(sizeof(*item));
        memset(item, 0, sizeof(*item));
        item->engine = engine;
        item->key = kvs_malloc(strlen(key) + 1);
        strcpy(item->key, key);
        item->next = g_dataset.buckets[idx];
        g_dataset.buckets[idx] = item;
    }
    kvs_free(item->value);
    item->value = kvs_malloc(strlen(value) + 1);
    strcpy(item->value, value);
}

void kvs_dataset_del(int engine, const char *key) {
    if (!g_dataset.buckets || !key) return;
    unsigned int idx = dataset_hash(engine, key);
    kvs_dataset_item_t *cur = g_dataset.buckets[idx], *prev = NULL;
    while (cur) {
        if (cur->engine == engine && strcmp(cur->key, key) == 0) {
            if (prev) prev->next = cur->next; else g_dataset.buckets[idx] = cur->next;
            kvs_free(cur->key); kvs_free(cur->value); kvs_free(cur); return;
        }
        prev = cur; cur = cur->next;
    }
}

void kvs_dataset_expire(int engine, const char *key, long long expire_at_ms) {
    kvs_dataset_item_t *item = dataset_find(engine, key);
    if (item) item->expire_at_ms = expire_at_ms;
}

void kvs_dataset_persist(int engine, const char *key) {
    kvs_dataset_item_t *item = dataset_find(engine, key);
    if (item) item->expire_at_ms = 0;
}

int kvs_persist_append_command(const kvs_resp_request_t *req) {
    if (g_replaying || !req || req->argc <= 0) return 0;
    FILE *fp = fopen(g_aof_path, "ab");
    if (!fp) return -1;
    if (fprintf(fp, "*%d\r\n", req->argc) < 0) { fclose(fp); return -1; }
    for (int i = 0; i < req->argc; ++i) {
        if (fprintf(fp, "$%zu\r\n", req->argv[i].len) < 0) { fclose(fp); return -1; }
        if (fwrite(req->argv[i].data, 1, req->argv[i].len, fp) != req->argv[i].len) { fclose(fp); return -1; }
        if (fwrite("\r\n", 1, 2, fp) != 2) { fclose(fp); return -1; }
    }
    fclose(fp);
    return 0;
}

static int load_file_replay(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return 0; }
    unsigned char *buf = kvs_malloc((size_t)sz);
    if (!buf) { fclose(fp); return -1; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) { kvs_free(buf); fclose(fp); return -1; }
    fclose(fp);
    size_t off = 0;
    g_replaying = 1;
    while (off < (size_t)sz) {
        kvs_resp_request_t req; char err[128] = {0}; size_t used = 0;
        int rc = kvs_resp_parse_one(buf + off, (size_t)sz - off, &used, &req, err, sizeof(err));
        if (rc <= 0) break;
        unsigned char out[256]; size_t outlen = 0;
        kvs_dispatch_request(&req, out, sizeof(out), &outlen, 1);
        kvs_resp_request_reset(&req);
        off += used;
    }
    g_replaying = 0;
    kvs_free(buf);
    return 0;
}

int kvs_persist_load_all(void) {
    if (load_file_replay(g_dump_path) < 0) return -1;
    if (load_file_replay(g_aof_path) < 0) return -1;
    return 0;
}

int kvs_persist_save_snapshot(void) {
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_dump_path);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return -1;
    long long now = kvs_now_ms();
    for (int i = 0; i < g_dataset.size; ++i) {
        for (kvs_dataset_item_t *cur = g_dataset.buckets[i]; cur; cur = cur->next) {
            if (!cur->value) continue;
            const char *setcmd = cur->engine == KVS_ENGINE_ARRAY ? "SET" : (cur->engine == KVS_ENGINE_RBTREE ? "RSET" : "HSET");
            const char *argv1[] = {setcmd, cur->key, cur->value};
            if (write_resp_cmd(fp, 3, argv1) < 0) { fclose(fp); unlink(tmp); return -1; }
            if (cur->expire_at_ms > now) {
                char ttlbuf[32];
                snprintf(ttlbuf, sizeof(ttlbuf), "%lld", (cur->expire_at_ms - now + 999) / 1000);
                const char *excmd = cur->engine == KVS_ENGINE_ARRAY ? "EXPIRE" : (cur->engine == KVS_ENGINE_RBTREE ? "REXPIRE" : "HEXPIRE");
                const char *argv2[] = {excmd, cur->key, ttlbuf};
                if (write_resp_cmd(fp, 3, argv2) < 0) { fclose(fp); unlink(tmp); return -1; }
            }
        }
    }
    fclose(fp);
    if (rename(tmp, g_dump_path) != 0) { unlink(tmp); return -1; }
    return 0;
}
