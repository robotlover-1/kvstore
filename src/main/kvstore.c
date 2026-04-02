#include "kvstore/kvstore.h"
#include <ctype.h>

kv_config_t g_cfg = { .role = ROLE_MASTER, .port = 5000, .master_host = "127.0.0.1", .master_port = 5000, .dump_path = "kvstore.dump", .aof_path = "kvstore.aof", .mem_backend = "libc" };
conn_t *g_replicas = NULL;
pthread_mutex_t g_repl_lock = PTHREAD_MUTEX_INITIALIZER;

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
        else if (!strcmp(argv[i], "--mem") && i + 1 < argc) snprintf(g_cfg.mem_backend, sizeof(g_cfg.mem_backend), "%s", argv[++i]);
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
    return strcmp(cmd, "GET") && strcmp(cmd, "TTL") && strcmp(cmd, "EXIST") && strcmp(cmd, "RGET") && strcmp(cmd, "RTTL") && strcmp(cmd, "REXIST") && strcmp(cmd, "HGET") && strcmp(cmd, "HTTL") && strcmp(cmd, "HEXIST") && strcmp(cmd, "INFO") && strcmp(cmd, "MEMSTAT");
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

static int build_memstat_text(char *buf, size_t cap) {
    kvs_mem_stats_t st;
    if (kvs_mem_get_stats(&st) != 0) return -1;

    int n = snprintf(
        buf, cap,
        "backend=%s\n"
        "backend_id=%d\n"
        "initialized=%d\n"
        "alloc_calls=%llu\n"
        "calloc_calls=%llu\n"
        "realloc_calls=%llu\n"
        "free_calls=%llu\n"
        "small_max_size=%zu\n"
        "small_alloc_calls=%llu\n"
        "small_free_calls=%llu\n"
        "current_small_inuse=%llu\n"
        "peak_small_inuse=%llu\n"
        "total_small_page_bytes=%llu\n"
        "large_alloc_calls=%llu\n"
        "large_free_calls=%llu\n"
        "fallback_alloc_calls=%llu\n"
        "fallback_free_calls=%llu\n"
        "current_large_inuse_bytes=%llu\n"
        "peak_large_inuse_bytes=%llu\n"
        "current_fallback_inuse_bytes=%llu\n"
        "peak_fallback_inuse_bytes=%llu\n"
        "total_large_map_bytes=%llu\n"
        "active_large_map_bytes=%llu\n"
        "peak_active_large_map_bytes=%llu\n"
        "current_requested_bytes=%llu\n"
        "current_allocated_bytes=%llu\n"
        "internal_fragment_bytes=%llu\n"
        "internal_fragment_rate=%.6f\n"
        "small_page_used_bytes=%llu\n"
        "page_utilization=%.6f\n",
        st.backend_name ? st.backend_name : "unknown",
        st.backend_id,
        st.initialized,
        st.alloc_calls,
        st.calloc_calls,
        st.realloc_calls,
        st.free_calls,
        st.small_max_size,
        st.small_alloc_calls,
        st.small_free_calls,
        st.current_small_inuse,
        st.peak_small_inuse,
        st.total_small_page_bytes,
        st.large_alloc_calls,
        st.large_free_calls,
        st.fallback_alloc_calls,
        st.fallback_free_calls,
        st.current_large_inuse_bytes,
        st.peak_large_inuse_bytes,
        st.current_fallback_inuse_bytes,
        st.peak_fallback_inuse_bytes,
        st.total_large_map_bytes,
        st.active_large_map_bytes,
        st.peak_active_large_map_bytes,
        st.current_requested_bytes,
        st.current_allocated_bytes,
        st.internal_fragment_bytes,
        st.internal_fragment_ppm / 1000000.0,
        st.small_page_used_bytes,
        st.page_utilization_ppm / 1000000.0
    );
    if (n < 0 || (size_t)n >= cap) return -1;

    size_t pos = (size_t)n;
    for (size_t i = 0; i < st.class_count && i < 16; ++i) {
        n = snprintf(
            buf + pos, cap - pos,
            "class_%zu_size=%zu\nclass_%zu_pages=%zu\nclass_%zu_total_chunks=%zu\nclass_%zu_free_chunks=%zu\nclass_%zu_page_bytes=%zu\n",
            i, st.class_sizes[i],
            i, st.class_page_count[i],
            i, st.class_total_chunks[i],
            i, st.class_free_chunks[i],
            i, st.class_bytes_in_pages[i]
        );
        if (n < 0 || (size_t)n >= cap - pos) return -1;
        pos += (size_t)n;
    }
    return (int)pos;
}
static void kvs_ascii_upper(char *s) {
    if (!s) return;
    for (; *s; ++s) *s = (char)toupper((unsigned char)*s);
}

static int resp_array_header(char *out, size_t cap, int count) {
    return snprintf(out, cap, "*%d\r\n", count);
}

static int resp_empty_array(char *out, size_t cap) {
    return snprintf(out, cap, "*0\r\n");
}

static int resp_array_two_bulk(char *out, size_t cap, const char *a, const char *b) {
    size_t pos = 0;
    int n = resp_array_header(out + pos, cap - pos, 2);
    if (n < 0 || (size_t)n >= cap - pos) return -1;
    pos += (size_t)n;
    n = resp_bulk(out + pos, cap - pos, a, strlen(a));
    if (n < 0 || (size_t)n >= cap - pos) return -1;
    pos += (size_t)n;
    n = resp_bulk(out + pos, cap - pos, b, strlen(b));
    if (n < 0 || (size_t)n >= cap - pos) return -1;
    pos += (size_t)n;
    return (int)pos;
}

static int split_inline_argv(char *line, char **argv, int maxargc) {
    int argc = 0;
    char *p = line;
    while (*p && argc < maxargc) {
        while (*p && isspace((unsigned char)*p)) ++p;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && !isspace((unsigned char)*p)) ++p;
        if (!*p) break;
        *p++ = '\0';
    }
    return argc;
}

int handle_parsed_command(conn_t *c, int argc, char **argv, size_t *argl, const unsigned char *raw, size_t rawlen, int from_replication) {
    (void)argl;
    char resp[BUFFER_CAP]; int n = 0;
    if (argc <= 0) return -1;
    kvs_ascii_upper(argv[0]);
    const char *cmd = argv[0];

    if (g_cfg.role == ROLE_SLAVE && !from_replication && is_readonly_slave_blocked(cmd)) {
        n = resp_error(resp, sizeof(resp), "read only slave");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        return 0;
    }
    if (!strcmp(cmd, "PING")) {
        if (argc >= 2) n = resp_bulk(resp, sizeof(resp), argv[1], strlen(argv[1]));
        else n = resp_simple_string(resp, sizeof(resp), "PONG");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        return 0;
    }
    if (!strcmp(cmd, "ECHO") && argc == 2) {
        n = resp_bulk(resp, sizeof(resp), argv[1], strlen(argv[1]));
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        return 0;
    }
    if (!strcmp(cmd, "QUIT")) {
        n = resp_simple_string(resp, sizeof(resp), "OK");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        return 0;
    }
    if (!strcmp(cmd, "COMMAND")) {
        n = resp_empty_array(resp, sizeof(resp));
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        return 0;
    }
    if (!strcmp(cmd, "CLIENT")) {
        if (argc >= 2 && !strcmp(argv[1], "SETINFO")) {
            n = resp_simple_string(resp, sizeof(resp), "OK");
        } else {
            n = resp_array_two_bulk(resp, sizeof(resp), "id", "1");
            if (n < 0) n = resp_error(resp, sizeof(resp), "client subcommand failed");
        }
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        return 0;
    }
    if (!strcmp(cmd, "HELLO")) {
        n = resp_error(resp, sizeof(resp), "NOPROTO unsupported RESP version");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        return 0;
    }
    if (!strcmp(cmd, "REPLSYNC")) { repl_add_slave(c); queue_snapshot(c); return 0; }
    if (!strcmp(cmd, "REPLDONE")) return 0;
    if (!strcmp(cmd, "INFO")) {
        char info[256]; snprintf(info, sizeof(info), "role:%s mem:%s", g_cfg.role == ROLE_MASTER ? "master" : "slave", kvs_mem_backend_name());
        n = resp_bulk(resp, sizeof(resp), info, strlen(info));
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        return 0;
    }
    if (!strcmp(cmd, "MEMSTAT")) {
        char info[BUFFER_CAP];
        n = build_memstat_text(info, sizeof(info));
        if (n < 0) n = resp_error(resp, sizeof(resp), "memstat build failed");
        else n = resp_bulk(resp, sizeof(resp), info, (size_t)n);
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
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        return 0;
    } else if (!strcmp(op, "EXIST") && argc == 2) {
        try_expire(engine, argv[1]);
        n = resp_integer(resp, sizeof(resp), engine_exist(engine, argv[1]) == 0 ? 1 : 0);
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        return 0;
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
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        return 0;
    } else if (!strcmp(op, "PERSIST") && argc == 2) {
        try_expire(engine, argv[1]);
        if (engine_exist(engine, argv[1]) != 0) rc = 1;
        else rc = kvs_expire_persist(&global_expire, engine, argv[1]);
    } else {
        n = resp_error(resp, sizeof(resp), "unknown command or wrong args");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        return 0;
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
            pos += 2;
            continue;
        }

        if (buf[pos] != '*') {
            size_t line_end = pos;
            while (line_end < *len && buf[line_end] != '\n') line_end++;
            if (line_end >= *len) break;

            size_t line_len = line_end - pos;
            if (line_len > 0 && buf[pos + line_len - 1] == '\r') line_len--;
            if (line_len == 0) {
                pos = line_end + 1;
                continue;
            }

            char *line = (char *)kvs_malloc(line_len + 1);
            if (!line) {
                if (c) {
                    char r[64];
                    int n = resp_error(r, sizeof(r), "oom");
                    queue_bytes(c, (unsigned char *)r, (size_t)n);
                }
                pos = line_end + 1;
                continue;
            }
            memcpy(line, buf + pos, line_len);
            line[line_len] = '\0';

            char *argv[32] = {0};
            size_t argl[32] = {0};
            int argc = split_inline_argv(line, argv, 32);
            if (argc > 0) {
                for (int i = 0; i < argc; ++i) argl[i] = strlen(argv[i]);
                handle_parsed_command(c, argc, argv, argl, buf + pos, line_end + 1 - pos, from_replication);
            }
            kvs_free(line);
            pos = line_end + 1;
            continue;
        }

        size_t start = pos, p = pos + 1;
        while (p + 1 < *len && !(buf[p] == '\r' && buf[p + 1] == '\n')) p++;
        if (p + 1 >= *len) break;
        char nbuf[32] = {0};
        memcpy(nbuf, buf + pos + 1, p - (pos + 1));
        int argc = atoi(nbuf);
        if (argc <= 0 || argc > 32) {
            if (c) {
                char r[64];
                int n = resp_error(r, sizeof(r), "invalid argc");
                queue_bytes(c, (unsigned char *)r, (size_t)n);
            }
            pos = p + 2;
            continue;
        }
        p += 2;
        char *argv[32] = {0};
        size_t argl[32] = {0};
        int ok = 1;
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
                if (c) {
                    char r[128];
                    int n = resp_error(r, sizeof(r), "invalid bulk length");
                    queue_bytes(c, (unsigned char *)r, (size_t)n);
                }
                break;
            }
            p = lp + 2;
            if (p + (size_t)blen + 2 > *len) { ok = 0; break; }
            argv[i] = (char *)kvs_malloc((size_t)blen + 1);
            if (!argv[i]) { ok = 0; break; }
            memcpy(argv[i], buf + p, (size_t)blen);
            argv[i][blen] = 0;
            argl[i] = (size_t)blen;
            p += (size_t)blen;
            if (!(buf[p] == '\r' && buf[p + 1] == '\n')) { ok = 0; break; }
            p += 2;
        }
        if (!ok) {
            for (int i = 0; i < argc; ++i) kvs_free(argv[i]);
            if (p > pos) pos = p; else break;
            continue;
        }
        handle_parsed_command(c, argc, argv, argl, buf + start, p - start, from_replication);
        for (int i = 0; i < argc; ++i) kvs_free(argv[i]);
        pos = p;
    }
    if (pos > 0 && pos < *len) {
        memmove(buf, buf + pos, *len - pos);
        *len -= pos;
    } else if (pos >= *len) {
        *len = 0;
    }
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
        fprintf(stderr, "Usage: %s --port 5000 [--role master|slave] [--master-host 127.0.0.1 --master-port 5000] [--mem libc|jemalloc|custom]\n", argv[0]);
        return 1;
    }
    if (!strcmp(g_cfg.mem_backend, "jemalloc")) {
        if (kvs_mem_prepare_process(g_cfg.mem_backend, argv[0], argv) != 0 && getenv("KVS_MEM_JEMALLOC_ACTIVE") == NULL) {
            fprintf(stderr, "failed to prepare jemalloc process image\n");
            return 1;
        }
    }
    if (kvs_mem_init(g_cfg.mem_backend) != 0) {
        fprintf(stderr, "failed to init memory backend: %s\n", g_cfg.mem_backend);
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
