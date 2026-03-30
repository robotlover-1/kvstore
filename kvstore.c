#include "kvstore.h"

kv_config_t g_cfg = { .role = ROLE_MASTER, .port = 5000, .master_host = "127.0.0.1", .master_port = 5000, .dump_path = "kvstore.dump", .aof_path = "kvstore.aof" };
conn_t *g_replicas = NULL;
pthread_mutex_t g_repl_lock = PTHREAD_MUTEX_INITIALIZER;

void *kvs_malloc(size_t size) { return malloc(size); }
void kvs_free(void *ptr) { free(ptr); }

int resp_simple_string(char *out, size_t cap, const char *s) { return snprintf(out, cap, "+%s\r\n", s); }
int resp_error(char *out, size_t cap, const char *s) { return snprintf(out, cap, "-ERR %s\r\n", s); }
int resp_integer(char *out, size_t cap, long long v) { return snprintf(out, cap, ":%lld\r\n", v); }
int resp_bulk(char *out, size_t cap, const char *s, size_t len) { int n = snprintf(out, cap, "$%zu\r\n", len); if ((size_t)n + len + 2 > cap) return -1; memcpy(out + n, s, len); out[n + len] = '\r'; out[n + len + 1] = '\n'; return n + (int)len + 2; }
int resp_null_bulk(char *out, size_t cap) { return snprintf(out, cap, "$-1\r\n"); }

static size_t append_bulk(unsigned char *out, size_t pos, size_t cap, const char *s) {
    size_t len = strlen(s);
    int n = snprintf((char *)out + pos, cap - pos, "$%zu\r\n", len);
    pos += (size_t)n;
    memcpy(out + pos, s, len); pos += len;
    out[pos++] = '\r'; out[pos++] = '\n';
    return pos;
}
size_t resp_build_cmd3(unsigned char *out, size_t cap, const char *cmd, const char *a1, const char *a2) { size_t pos=0; pos += (size_t)snprintf((char*)out+pos, cap-pos, "*3\r\n"); pos=append_bulk(out,pos,cap,cmd); pos=append_bulk(out,pos,cap,a1); pos=append_bulk(out,pos,cap,a2); return pos; }
size_t resp_build_cmd2(unsigned char *out, size_t cap, const char *cmd, const char *a1) { size_t pos=0; pos += (size_t)snprintf((char*)out+pos, cap-pos, "*2\r\n"); pos=append_bulk(out,pos,cap,cmd); pos=append_bulk(out,pos,cap,a1); return pos; }
size_t resp_build_cmd1(unsigned char *out, size_t cap, const char *cmd) { size_t pos=0; pos += (size_t)snprintf((char*)out+pos, cap-pos, "*1\r\n"); pos=append_bulk(out,pos,cap,cmd); return pos; }

static int parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) g_cfg.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--role") && i + 1 < argc) g_cfg.role = !strcmp(argv[++i], "slave") ? ROLE_SLAVE : ROLE_MASTER;
        else if (!strcmp(argv[i], "--master-host") && i + 1 < argc) snprintf(g_cfg.master_host, sizeof(g_cfg.master_host), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--master-port") && i + 1 < argc) g_cfg.master_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) snprintf(g_cfg.dump_path, sizeof(g_cfg.dump_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--aof") && i + 1 < argc) snprintf(g_cfg.aof_path, sizeof(g_cfg.aof_path), "%s", argv[++i]);
        else return -1;
    }
    return 0;
}

void repl_add_slave(conn_t *c) {
    pthread_mutex_lock(&g_repl_lock);
    c->is_replica = 1;
    c->next_replica = g_replicas;
    g_replicas = c;
    pthread_mutex_unlock(&g_repl_lock);
}

void repl_remove_slave(conn_t *c) {
    pthread_mutex_lock(&g_repl_lock);
    conn_t **pp = &g_replicas;
    while (*pp) {
        if (*pp == c) { *pp = c->next_replica; break; }
        pp = &(*pp)->next_replica;
    }
    pthread_mutex_unlock(&g_repl_lock);
}

void repl_broadcast(const unsigned char *raw, size_t rawlen) {
    pthread_mutex_lock(&g_repl_lock);
    for (conn_t *c = g_replicas; c; c = c->next_replica) queue_bytes(c, raw, rawlen);
    pthread_mutex_unlock(&g_repl_lock);
}

static int queue_snapshot(conn_t *c) {
    char hdr[64];
    int n = resp_simple_string(hdr, sizeof(hdr), "FULLRESYNC");
    queue_bytes(c, (unsigned char *)hdr, (size_t)n);
    FILE *fp = fopen(g_cfg.dump_path, "wb");
    if (!fp) return -1;
    if (kvs_snapshot_to_fp(fp) != 0) { fclose(fp); return -1; }
    fclose(fp);
    fp = fopen(g_cfg.dump_path, "rb");
    if (!fp) return -1;
    unsigned char buf[8192]; size_t r;
    while ((r = fread(buf, 1, sizeof(buf), fp)) > 0) queue_bytes(c, buf, r);
    fclose(fp);
    size_t done = resp_build_cmd1(buf, sizeof(buf), "REPLDONE");
    queue_bytes(c, buf, done);
    return 0;
}

static int is_readonly_slave_blocked(const char *cmd) {
    return strcmp(cmd, "GET") && strcmp(cmd, "TTL") && strcmp(cmd, "EXIST") && strcmp(cmd, "RGET") && strcmp(cmd, "RTTL") && strcmp(cmd, "REXIST") && strcmp(cmd, "HGET") && strcmp(cmd, "HTTL") && strcmp(cmd, "HEXIST") && strcmp(cmd, "INFO");
}

static int is_write_cmd(const char *cmd) {
    const char *writes[] = {"SET","MOD","DEL","EXPIRE","PERSIST","RSET","RMOD","RDEL","REXPIRE","RPERSIST","HSET","HMOD","HDEL","HEXPIRE","HPERSIST",NULL};
    for (int i = 0; writes[i]; ++i) if (!strcmp(cmd, writes[i])) return 1;
    return 0;
}

static int cmd_engine(const char *cmd) {
    if (cmd[0] == 'R') return KVS_ENGINE_RBTREE;
    if (cmd[0] == 'H') return KVS_ENGINE_HASH;
    return KVS_ENGINE_ARRAY;
}

static const char *strip_prefix(const char *cmd) {
    if (cmd[0] == 'R' || cmd[0] == 'H') return cmd + 1;
    return cmd;
}

static int engine_exist(int engine, char *key) {
    switch (engine) {
        case KVS_ENGINE_ARRAY: return kvs_array_exist(&global_array, key);
        case KVS_ENGINE_RBTREE: return kvs_rbtree_exist(&global_rbtree, key);
        case KVS_ENGINE_HASH: return kvs_hash_exist(&global_hash, key);
        default: return 1;
    }
}
static char *engine_get(int engine, char *key) {
    switch (engine) {
        case KVS_ENGINE_ARRAY: return kvs_array_get(&global_array, key);
        case KVS_ENGINE_RBTREE: return kvs_rbtree_get(&global_rbtree, key);
        case KVS_ENGINE_HASH: return kvs_hash_get(&global_hash, key);
        default: return NULL;
    }
}
static int engine_set(int engine, char *key, char *value) {
    switch (engine) {
        case KVS_ENGINE_ARRAY: return kvs_array_set(&global_array, key, value);
        case KVS_ENGINE_RBTREE: return kvs_rbtree_set(&global_rbtree, key, value);
        case KVS_ENGINE_HASH: return kvs_hash_set(&global_hash, key, value);
        default: return -1;
    }
}
static int engine_mod(int engine, char *key, char *value) {
    switch (engine) {
        case KVS_ENGINE_ARRAY: return kvs_array_mod(&global_array, key, value);
        case KVS_ENGINE_RBTREE: return kvs_rbtree_mod(&global_rbtree, key, value);
        case KVS_ENGINE_HASH: return kvs_hash_mod(&global_hash, key, value);
        default: return -1;
    }
}
static int engine_del(int engine, char *key) {
    switch (engine) {
        case KVS_ENGINE_ARRAY: return kvs_array_del(&global_array, key);
        case KVS_ENGINE_RBTREE: return kvs_rbtree_del(&global_rbtree, key);
        case KVS_ENGINE_HASH: return kvs_hash_del(&global_hash, key);
        default: return -1;
    }
}
static int try_expire(int engine, char *key) {
    if (kvs_expire_is_expired(&global_expire, engine, key)) {
        engine_del(engine, key);
        kvs_expire_del(&global_expire, engine, key);
        return 1;
    }
    return 0;
}

int handle_parsed_command(conn_t *c, int argc, char **argv, size_t *argl, const unsigned char *raw, size_t rawlen, int from_replication) {
    char resp[BUFFER_CAP]; int n = 0;
    if (argc <= 0) return -1;
    const char *cmd = argv[0];

    if (g_cfg.role == ROLE_SLAVE && !from_replication && is_readonly_slave_blocked(cmd)) {
        n = resp_error(resp, sizeof(resp), "read only slave");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        return 0;
    }
    if (!strcmp(cmd, "REPLSYNC")) { repl_add_slave(c); queue_snapshot(c); return 0; }
    if (!strcmp(cmd, "REPLDONE")) return 0;
    if (!strcmp(cmd, "INFO")) {
        char info[128]; snprintf(info, sizeof(info), "role:%s", g_cfg.role == ROLE_MASTER ? "master" : "slave");
        n = resp_bulk(resp, sizeof(resp), info, strlen(info));
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        return 0;
    }
    if (!strcmp(cmd, "SAVE")) {
        n = (persist_save_dump() == 0) ? resp_simple_string(resp, sizeof(resp), "OK") : resp_error(resp, sizeof(resp), "save failed");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        return 0;
    }

    int engine = cmd_engine(cmd);
    const char *op = strip_prefix(cmd);
    int rc = -1;

    if (!strcmp(op, "SET") && argc == 3) rc = engine_set(engine, argv[1], argv[2]);
    else if (!strcmp(op, "MOD") && argc == 3) { try_expire(engine, argv[1]); rc = engine_mod(engine, argv[1], argv[2]); }
    else if (!strcmp(op, "DEL") && argc == 2) { try_expire(engine, argv[1]); rc = engine_del(engine, argv[1]); kvs_expire_del(&global_expire, engine, argv[1]); }
    else if (!strcmp(op, "GET") && argc == 2) {
        try_expire(engine, argv[1]);
        char *v = engine_get(engine, argv[1]);
        n = v ? resp_bulk(resp, sizeof(resp), v, strlen(v)) : resp_null_bulk(resp, sizeof(resp));
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n); return 0;
    } else if (!strcmp(op, "EXIST") && argc == 2) {
        try_expire(engine, argv[1]);
        n = resp_integer(resp, sizeof(resp), engine_exist(engine, argv[1]) == 0 ? 1 : 0);
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n); return 0;
    } else if (!strcmp(op, "EXPIRE") && argc == 3) {
        try_expire(engine, argv[1]);
        if (engine_exist(engine, argv[1]) != 0) rc = 1;
        else rc = kvs_expire_set(&global_expire, engine, argv[1], atoll(argv[2]) * 1000);
    } else if (!strcmp(op, "TTL") && argc == 2) {
        try_expire(engine, argv[1]);
        if (engine_exist(engine, argv[1]) != 0) n = resp_integer(resp, sizeof(resp), -2);
        else {
            long long ttl = kvs_expire_ttl(&global_expire, engine, argv[1]);
            n = resp_integer(resp, sizeof(resp), ttl);
        }
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n); return 0;
    } else if (!strcmp(op, "PERSIST") && argc == 2) {
        try_expire(engine, argv[1]);
        if (engine_exist(engine, argv[1]) != 0) rc = 1;
        else rc = kvs_expire_persist(&global_expire, engine, argv[1]);
    } else {
        n = resp_error(resp, sizeof(resp), "unknown command or wrong args");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n); return 0;
    }

    if (rc == 0) {
        n = resp_simple_string(resp, sizeof(resp), "OK");
        if (!from_replication && is_write_cmd(cmd)) {
            persist_append_raw(raw, rawlen);
            if (g_cfg.role == ROLE_MASTER) repl_broadcast(raw, rawlen);
        }
    } else if (rc == 1) {
        n = resp_error(resp, sizeof(resp), "not found or exists");
    } else {
        n = resp_error(resp, sizeof(resp), "operation failed");
    }
    if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
    return 0;
}

int parse_resp_stream(conn_t *c, unsigned char *buf, size_t *len, int from_replication) {
    size_t pos = 0;
    while (pos < *len) {
        if (buf[pos] == '+') {
            while (pos + 1 < *len && !(buf[pos] == '\r' && buf[pos + 1] == '\n')) pos++;
            if (pos + 1 >= *len) break;
            pos += 2; continue;
        }
        if (buf[pos] != '*') {
            if (c) { char r[64]; int n = resp_error(r, sizeof(r), "invalid resp type"); queue_bytes(c, (unsigned char *)r, (size_t)n); }
            pos++; continue;
        }
        size_t start = pos, p = pos + 1;
        while (p + 1 < *len && !(buf[p] == '\r' && buf[p + 1] == '\n')) p++;
        if (p + 1 >= *len) break;
        char nbuf[32] = {0};
        memcpy(nbuf, buf + pos + 1, p - (pos + 1));
        int argc = atoi(nbuf);
        if (argc <= 0 || argc > 32) {
            if (c) { char r[64]; int n = resp_error(r, sizeof(r), "invalid argc"); queue_bytes(c, (unsigned char *)r, (size_t)n); }
            pos = p + 2; continue;
        }
        p += 2;
        char *argv[32] = {0}; size_t argl[32] = {0}; int ok = 1;
        for (int i = 0; i < argc; ++i) {
            if (p >= *len || buf[p] != '$') { ok = 0; break; }
            size_t lp = p + 1;
            while (lp + 1 < *len && !(buf[lp] == '\r' && buf[lp + 1] == '\n')) lp++;
            if (lp + 1 >= *len) { ok = 0; break; }
            char lbuf[32] = {0};
            memcpy(lbuf, buf + p + 1, lp - (p + 1));
            char *endp = NULL;
            long blen = strtol(lbuf, &endp, 10);
            if (!endp || *endp != '\0' || blen < 0) {
                ok = 0;
                if (c) { char r[128]; int n = resp_error(r, sizeof(r), "invalid bulk length"); queue_bytes(c, (unsigned char *)r, (size_t)n); }
                break;
            }
            p = lp + 2;
            if (p + (size_t)blen + 2 > *len) { ok = 0; break; }
            argv[i] = (char *)malloc((size_t)blen + 1);
            memcpy(argv[i], buf + p, (size_t)blen);
            argv[i][blen] = 0;
            argl[i] = (size_t)blen;
            p += (size_t)blen;
            if (!(buf[p] == '\r' && buf[p + 1] == '\n')) { ok = 0; break; }
            p += 2;
        }
        if (!ok) {
            for (int i = 0; i < argc; ++i) free(argv[i]);
            if (p > pos) pos = p; else break;
            continue;
        }
        handle_parsed_command(c, argc, argv, argl, buf + start, p - start, from_replication);
        for (int i = 0; i < argc; ++i) free(argv[i]);
        pos = p;
    }
    if (pos > 0 && pos < *len) { memmove(buf, buf + pos, *len - pos); *len -= pos; }
    else if (pos >= *len) *len = 0;
    return 0;
}

static int emit_cmd3_fp(FILE *fp, const char *cmd, const char *a1, const char *a2) {
    unsigned char buf[BUFFER_CAP];
    size_t n = resp_build_cmd3(buf, sizeof(buf), cmd, a1, a2);
    return fwrite(buf, 1, n, fp) == n ? 0 : -1;
}

static int maybe_emit_expire(FILE *fp, int engine, const char *key) {
    long long ttl = kvs_expire_ttl(&global_expire, engine, key);
    if (ttl < 0) return 0;
    char sec[32]; snprintf(sec, sizeof(sec), "%lld", ttl);
    const char *cmd = engine == KVS_ENGINE_ARRAY ? "EXPIRE" : engine == KVS_ENGINE_RBTREE ? "REXPIRE" : "HEXPIRE";
    return emit_cmd3_fp(fp, cmd, key, sec);
}

static int snapshot_array(FILE *fp) {
    for (int i = 0; i < KVS_ARRAY_SIZE; ++i) {
        if (global_array.table && global_array.table[i].key) {
            if (emit_cmd3_fp(fp, "SET", global_array.table[i].key, global_array.table[i].value) != 0) return -1;
            if (maybe_emit_expire(fp, KVS_ENGINE_ARRAY, global_array.table[i].key) != 0) return -1;
        }
    }
    return 0;
}

static int snapshot_hash(FILE *fp) {
    if (!global_hash.nodes) return 0;
    for (int i = 0; i < global_hash.max_slots; ++i) {
        for (hashnode_t *node = global_hash.nodes[i]; node; node = node->next) {
            if (emit_cmd3_fp(fp, "HSET", node->key, node->value) != 0) return -1;
            if (maybe_emit_expire(fp, KVS_ENGINE_HASH, node->key) != 0) return -1;
        }
    }
    return 0;
}

static int snapshot_rbtree_node(FILE *fp, rbtree_node *node, rbtree_node *nil) {
    if (node == nil) return 0;
    if (snapshot_rbtree_node(fp, node->left, nil) != 0) return -1;
    if (emit_cmd3_fp(fp, "RSET", node->key, (char *)node->value) != 0) return -1;
    if (maybe_emit_expire(fp, KVS_ENGINE_RBTREE, node->key) != 0) return -1;
    if (snapshot_rbtree_node(fp, node->right, nil) != 0) return -1;
    return 0;
}

int kvs_snapshot_to_fp(FILE *fp) {
    if (!fp) return -1;
    if (snapshot_array(fp) != 0) return -1;
    if (snapshot_rbtree_node(fp, global_rbtree.root, global_rbtree.nil) != 0) return -1;
    if (snapshot_hash(fp) != 0) return -1;
    return 0;
}

int main(int argc, char **argv) {
    if (parse_args(argc, argv) != 0) {
        fprintf(stderr, "Usage: %s --port 5000 [--role master|slave] [--master-host 127.0.0.1 --master-port 5000]\n", argv[0]);
        return 1;
    }
    kvs_array_create(&global_array);
    kvs_rbtree_create(&global_rbtree);
    kvs_hash_create(&global_hash);
    kvs_expire_create(&global_expire);
    if (persist_init() != 0) { perror("persist_init"); return 1; }
    persist_recover();
    return reactor_start();
}
