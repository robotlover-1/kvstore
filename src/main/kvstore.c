
#include "kvstore/kvstore.h"
#include <ctype.h>
#include <strings.h>

#define KVS_DEFAULT_CONFIG_PATH "kvstore.conf"

kv_config_t g_cfg = {
    .role = ROLE_MASTER,
    .port = 5000,
    .master_host = "127.0.0.1",
    .master_port = 5000,
    .dump_path = "kvstore.dump",
    .aof_path = "kvstore.aof",
    .mem_backend = "libc",
    .net_backend = "reactor",
    .aof_fsync = KVS_AOF_FSYNC_ALWAYS,
    .is_sentinel = 0,
    .sentinel_master_name = "mymaster",
    .sentinel_monitor_host = "127.0.0.1",
    .sentinel_monitor_port = 5000,
    .sentinel_known_slaves = "",
    .sentinel_down_after_ms = 5000,
    .sentinel_failover_timeout_ms = 10000,
    .sentinel_quorum = 1,
};
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
size_t resp_build_cmd4(unsigned char *out, size_t cap, const char *cmd, const char *a1, const char *a2, const char *a3) { size_t pos=0; pos += (size_t)snprintf((char*)out+pos, cap-pos, "*4\r\n"); pos=append_bulk(out,pos,cap,cmd); pos=append_bulk(out,pos,cap,a1); pos=append_bulk(out,pos,cap,a2); pos=append_bulk(out,pos,cap,a3); return pos; }
size_t resp_build_cmd3(unsigned char *out, size_t cap, const char *cmd, const char *a1, const char *a2) { size_t pos=0; pos += (size_t)snprintf((char*)out+pos, cap-pos, "*3\r\n"); pos=append_bulk(out,pos,cap,cmd); pos=append_bulk(out,pos,cap,a1); pos=append_bulk(out,pos,cap,a2); return pos; }
size_t resp_build_cmd2(unsigned char *out, size_t cap, const char *cmd, const char *a1) { size_t pos=0; pos += (size_t)snprintf((char*)out+pos, cap-pos, "*2\r\n"); pos=append_bulk(out,pos,cap,cmd); pos=append_bulk(out,pos,cap,a1); return pos; }
size_t resp_build_cmd1(unsigned char *out, size_t cap, const char *cmd) { size_t pos=0; pos += (size_t)snprintf((char*)out+pos, cap-pos, "*1\r\n"); pos=append_bulk(out,pos,cap,cmd); return pos; }

static int parse_autosnap_rules(const char *spec);
static int parse_config_file(const char *path);
static void trim_inplace(char *s);
static int parse_appendfsync_policy(const char *s, kvs_aof_fsync_policy_t *out) {
    if (!s || !out) return -1;
    if (!strcasecmp(s, "always")) {
        *out = KVS_AOF_FSYNC_ALWAYS;
        return 0;
    }
    if (!strcasecmp(s, "everysec")) {
        *out = KVS_AOF_FSYNC_EVERYSEC;
        return 0;
    }
    return -1;
}

static int parse_args(int argc, char **argv) {
    const char *config_path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            config_path = argv[i + 1];
            break;
        }
    }

    if (!config_path) {
        FILE *fp = fopen(KVS_DEFAULT_CONFIG_PATH, "r");
        if (fp) {
            fclose(fp);
            config_path = KVS_DEFAULT_CONFIG_PATH;
        }
    }

    if (config_path && parse_config_file(config_path) != 0) return -1;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            ++i;
        }
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) g_cfg.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--role") && i + 1 < argc) g_cfg.role = !strcmp(argv[++i], "slave") ? ROLE_SLAVE : ROLE_MASTER;
        else if (!strcmp(argv[i], "--master-host") && i + 1 < argc) snprintf(g_cfg.master_host, sizeof(g_cfg.master_host), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--master-port") && i + 1 < argc) g_cfg.master_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) snprintf(g_cfg.dump_path, sizeof(g_cfg.dump_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--aof") && i + 1 < argc) snprintf(g_cfg.aof_path, sizeof(g_cfg.aof_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--mem") && i + 1 < argc) snprintf(g_cfg.mem_backend, sizeof(g_cfg.mem_backend), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--net") && i + 1 < argc) {
            snprintf(g_cfg.net_backend, sizeof(g_cfg.net_backend), "%s", argv[++i]);
        }
        else if (!strcmp(argv[i], "--appendfsync") && i + 1 < argc) {
            kvs_aof_fsync_policy_t policy;
            if (parse_appendfsync_policy(argv[++i], &policy) != 0) return -1;
            g_cfg.aof_fsync = policy;
        }
        else if (!strcmp(argv[i], "--autosnap") && i + 1 < argc) {
            persist_clear_autosnap_rules();
            if (parse_autosnap_rules(argv[++i]) != 0) return -1;
        }
        else if (!strcmp(argv[i], "--sentinel")) {
            g_cfg.is_sentinel = 1;
        }
        else if (!strcmp(argv[i], "--sentinel-master-name") && i + 1 < argc) {
            snprintf(g_cfg.sentinel_master_name, sizeof(g_cfg.sentinel_master_name), "%s", argv[++i]);
        }
        else if (!strcmp(argv[i], "--sentinel-monitor-host") && i + 1 < argc) {
            snprintf(g_cfg.sentinel_monitor_host, sizeof(g_cfg.sentinel_monitor_host), "%s", argv[++i]);
        }
        else if (!strcmp(argv[i], "--sentinel-monitor-port") && i + 1 < argc) {
            g_cfg.sentinel_monitor_port = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--sentinel-known-slaves") && i + 1 < argc) {
            snprintf(g_cfg.sentinel_known_slaves, sizeof(g_cfg.sentinel_known_slaves), "%s", argv[++i]);
        }
        else if (!strcmp(argv[i], "--sentinel-down-after") && i + 1 < argc) {
            g_cfg.sentinel_down_after_ms = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--sentinel-failover-timeout") && i + 1 < argc) {
            g_cfg.sentinel_failover_timeout_ms = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--sentinel-quorum") && i + 1 < argc) {
            g_cfg.sentinel_quorum = atoi(argv[++i]);
        }
        else return -1;
    }
    return 0;
}

static int parse_autosnap_rules(const char *spec) {
    if (!spec || !*spec) return 0;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", spec);
    char *saveptr = NULL;
    for (char *tok = strtok_r(tmp, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
        char *sep = strchr(tok, ':');
        if (!sep) return -1;
        *sep = '\0';
        long long sec = atoll(tok);
        long long changes = atoll(sep + 1);
        if (persist_register_autosnap_rule(sec, changes) != 0) return -1;
    }
    return 0;
}

static void trim_inplace(char *s) {
    char *start;
    char *end;
    size_t len;
    if (!s) return;
    start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    len = strlen(s);
    if (len == 0) return;
    end = s + len - 1;
    while (end >= s && isspace((unsigned char)*end)) {
        *end = '\0';
        --end;
    }
}

static int apply_config_kv(const char *key, const char *value) {
    kvs_aof_fsync_policy_t policy;
    if (!key || !value) return -1;
    if (!strcmp(key, "port")) g_cfg.port = atoi(value);
    else if (!strcmp(key, "role")) g_cfg.role = !strcasecmp(value, "slave") ? ROLE_SLAVE : ROLE_MASTER;
    else if (!strcmp(key, "master_host")) snprintf(g_cfg.master_host, sizeof(g_cfg.master_host), "%s", value);
    else if (!strcmp(key, "master_port")) g_cfg.master_port = atoi(value);
    else if (!strcmp(key, "dump_path")) snprintf(g_cfg.dump_path, sizeof(g_cfg.dump_path), "%s", value);
    else if (!strcmp(key, "aof_path")) snprintf(g_cfg.aof_path, sizeof(g_cfg.aof_path), "%s", value);
    else if (!strcmp(key, "mem_backend")) snprintf(g_cfg.mem_backend, sizeof(g_cfg.mem_backend), "%s", value);
    else if (!strcmp(key, "net_backend")) snprintf(g_cfg.net_backend, sizeof(g_cfg.net_backend), "%s", value);
    else if (!strcmp(key, "appendfsync")) {
        if (parse_appendfsync_policy(value, &policy) != 0) return -1;
        g_cfg.aof_fsync = policy;
    }
    else if (!strcmp(key, "autosnap")) {
        persist_clear_autosnap_rules();
        if (parse_autosnap_rules(value) != 0) return -1;
    }
    else if (!strcmp(key, "sentinel")) g_cfg.is_sentinel = (!strcasecmp(value, "1") || !strcasecmp(value, "true") || !strcasecmp(value, "yes"));
    else if (!strcmp(key, "sentinel_master_name")) snprintf(g_cfg.sentinel_master_name, sizeof(g_cfg.sentinel_master_name), "%s", value);
    else if (!strcmp(key, "sentinel_monitor_host")) snprintf(g_cfg.sentinel_monitor_host, sizeof(g_cfg.sentinel_monitor_host), "%s", value);
    else if (!strcmp(key, "sentinel_monitor_port")) g_cfg.sentinel_monitor_port = atoi(value);
    else if (!strcmp(key, "sentinel_known_slaves")) snprintf(g_cfg.sentinel_known_slaves, sizeof(g_cfg.sentinel_known_slaves), "%s", value);
    else if (!strcmp(key, "sentinel_down_after_ms")) g_cfg.sentinel_down_after_ms = atoi(value);
    else if (!strcmp(key, "sentinel_failover_timeout_ms")) g_cfg.sentinel_failover_timeout_ms = atoi(value);
    else if (!strcmp(key, "sentinel_quorum")) g_cfg.sentinel_quorum = atoi(value);
    else return -1;
    return 0;
}

static int parse_config_file(const char *path) {
    FILE *fp;
    char line[1024];
    int lineno = 0;
    if (!path || !*path) return -1;
    fp = fopen(path, "r");
    if (!fp) return -1;
    while (fgets(line, sizeof(line), fp)) {
        char *eq;
        char *key;
        char *value;
        lineno++;
        trim_inplace(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        eq = strchr(line, '=');
        if (!eq) {
            fclose(fp);
            return -1;
        }
        *eq = '\0';
        key = line;
        value = eq + 1;
        trim_inplace(key);
        trim_inplace(value);
        if (apply_config_kv(key, value) != 0) {
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
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
    return strcmp(cmd, "GET") && strcmp(cmd, "MGET") && strcmp(cmd, "TTL") && strcmp(cmd, "EXIST") && strcmp(cmd, "OWNER")
        && strcmp(cmd, "RGET") && strcmp(cmd, "RMGET") && strcmp(cmd, "RTTL") && strcmp(cmd, "REXIST")
        && strcmp(cmd, "HGET") && strcmp(cmd, "HMGET") && strcmp(cmd, "HTTL") && strcmp(cmd, "HEXIST")
        && strcmp(cmd, "XGET") && strcmp(cmd, "XMGET") && strcmp(cmd, "XTTL") && strcmp(cmd, "XEXIST")
        && strcmp(cmd, "INFO") && strcmp(cmd, "MEMSTAT")
        && strcmp(cmd, "SLAVEOF") && strcmp(cmd, "ROLE")
        && strcmp(cmd, "DOCGET") && strcmp(cmd, "DOCGETALL") && strcmp(cmd, "DOCEXIST") && strcmp(cmd, "DOCCOUNT");
}

static int is_write_cmd(const char *cmd) {
    const char *writes[] = {
        "SET","MSET","MOD","DEL","EXPIRE","PERSIST",
        "RSET","RMSET","RMOD","RDEL","REXPIRE","RPERSIST",
        "HSET","HMSET","HMOD","HDEL","HEXPIRE","HPERSIST",
        "XSET","XMSET","XMOD","XDEL","XEXPIRE","XPERSIST",
        "LOCK","UNLOCK","RENEW",
        "DOCSET","DOCDEL","DOCDROP",
        NULL
    };
    for (int i = 0; writes[i]; ++i) if (!strcmp(cmd, writes[i])) return 1;
    return 0;
}

static int cmd_engine(const char *cmd) {
    if (cmd[0] == 'R') return KVS_ENGINE_RBTREE;
    if (cmd[0] == 'H') return KVS_ENGINE_HASH;
    if (cmd[0] == 'X') return KVS_ENGINE_SKIPTABLE;
    return KVS_ENGINE_ARRAY;
}

static const char *strip_prefix(const char *cmd) {
    if (cmd[0] == 'R' || cmd[0] == 'H' || cmd[0] == 'X')
        return cmd + 1;
    return cmd;
}

static int engine_exist(int engine, char *key) {
    switch (engine) {
        case KVS_ENGINE_ARRAY: return kvs_array_exist(&global_array, key);
        case KVS_ENGINE_RBTREE: return kvs_rbtree_exist(&global_rbtree, key);
        case KVS_ENGINE_HASH: return kvs_hash_exist(&global_hash, key);
        case KVS_ENGINE_SKIPTABLE: return kvs_skiptable_exist(&global_skiptable, key);
        default: return 1;
    }
}
static char *engine_get(int engine, char *key) {
    switch (engine) {
        case KVS_ENGINE_ARRAY: return kvs_array_get(&global_array, key);
        case KVS_ENGINE_RBTREE: return kvs_rbtree_get(&global_rbtree, key);
        case KVS_ENGINE_HASH: return kvs_hash_get(&global_hash, key);
        case KVS_ENGINE_SKIPTABLE: return kvs_skiptable_get(&global_skiptable, key);
        default: return NULL;
    }
}
static int engine_set(int engine, char *key, char *value) {
    switch (engine) {
        case KVS_ENGINE_ARRAY: return kvs_array_set(&global_array, key, value);
        case KVS_ENGINE_RBTREE: return kvs_rbtree_set(&global_rbtree, key, value);
        case KVS_ENGINE_HASH: return kvs_hash_set(&global_hash, key, value);
        case KVS_ENGINE_SKIPTABLE: return kvs_skiptable_set(&global_skiptable, key, value);
        default: return -1;
    }
}
static int engine_mod(int engine, char *key, char *value) {
    switch (engine) {
        case KVS_ENGINE_ARRAY: return kvs_array_mod(&global_array, key, value);
        case KVS_ENGINE_RBTREE: return kvs_rbtree_mod(&global_rbtree, key, value);
        case KVS_ENGINE_HASH: return kvs_hash_mod(&global_hash, key, value);
        case KVS_ENGINE_SKIPTABLE: return kvs_skiptable_mod(&global_skiptable, key, value);
        default: return -1;
    }
}
static int engine_del(int engine, char *key) {
    switch (engine) {
        case KVS_ENGINE_ARRAY: return kvs_array_del(&global_array, key);
        case KVS_ENGINE_RBTREE: return kvs_rbtree_del(&global_rbtree, key);
        case KVS_ENGINE_HASH: return kvs_hash_del(&global_hash, key);
        case KVS_ENGINE_SKIPTABLE: return kvs_skiptable_del(&global_skiptable, key);
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

static int resp_array_from_values(char *out, size_t cap, int count, char **values);

static int engine_upsert(int engine, char *key, char *value) {
    int exists;
    if (!key || !value) return -1;
    try_expire(engine, key);
    exists = engine_exist(engine, key) == 0;
    if (exists) {
        if (engine_mod(engine, key, value) != 0) return -1;
        kvs_expire_persist(&global_expire, engine, key);
        return 0;
    }
    return engine_set(engine, key, value);
}

static int handle_multi_set(int engine, int argc, char **argv) {
    if (argc < 3 || (argc % 2) == 0) return -1;
    for (int i = 1; i < argc; i += 2) {
        if (!argv[i] || !argv[i + 1]) return -1;
    }
    for (int i = 1; i < argc; i += 2) {
        if (engine_upsert(engine, argv[i], argv[i + 1]) != 0) return -1;
    }
    return 0;
}

static int handle_multi_get(int engine, int argc, char **argv, char *resp, size_t cap) {
    char *values[32] = {0};
    if (argc < 2 || argc > 32) return -1;
    for (int i = 1; i < argc; ++i) {
        try_expire(engine, argv[i]);
        values[i - 1] = engine_get(engine, argv[i]);
    }
    return resp_array_from_values(resp, cap, argc - 1, values);
}

static int lock_acquire(int engine, char *key, char *token, long long ttl_ms) {
    if (!key || !token || ttl_ms <= 0) return -1;
    try_expire(engine, key);
    if (engine_exist(engine, key) == 0) return 1;
    if (engine_set(engine, key, token) != 0) return -1;
    if (kvs_expire_set(&global_expire, engine, key, ttl_ms) != 0) {
        engine_del(engine, key);
        kvs_expire_del(&global_expire, engine, key);
        return -1;
    }
    return 0;
}

static int lock_release(int engine, char *key, char *token) {
    char *cur;
    if (!key || !token) return -1;
    try_expire(engine, key);
    cur = engine_get(engine, key);
    if (!cur) return 1;
    if (strcmp(cur, token) != 0) return 1;
    if (engine_del(engine, key) != 0) return -1;
    kvs_expire_del(&global_expire, engine, key);
    return 0;
}

static int lock_renew(int engine, char *key, char *token, long long ttl_ms) {
    char *cur;
    if (!key || !token || ttl_ms <= 0) return -1;
    try_expire(engine, key);
    cur = engine_get(engine, key);
    if (!cur) return 1;
    if (strcmp(cur, token) != 0) return 1;
    return kvs_expire_set(&global_expire, engine, key, ttl_ms);
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

static int resp_array_append_bulk_or_null(char *out, size_t cap, size_t *pos, const char *s) {
    int n;
    if (!out || !pos || *pos >= cap) return -1;
    if (s) n = resp_bulk(out + *pos, cap - *pos, s, strlen(s));
    else n = resp_null_bulk(out + *pos, cap - *pos);
    if (n < 0 || (size_t)n > cap - *pos) return -1;
    *pos += (size_t)n;
    return 0;
}

static int resp_array_from_values(char *out, size_t cap, int count, char **values) {
    size_t pos = 0;
    int n = resp_array_header(out + pos, cap - pos, count);
    if (n < 0 || (size_t)n >= cap - pos) return -1;
    pos += (size_t)n;
    for (int i = 0; i < count; ++i) {
        if (resp_array_append_bulk_or_null(out, cap, &pos, values[i]) != 0) return -1;
    }
    return (int)pos;
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

    int rc_ret = 0;
    int n = 0;
    char *resp = (char *)kvs_malloc(BUFFER_CAP);
    if (!resp) return -1;

    (void)argl;
    if (argc <= 0) {
        rc_ret = -1;
        goto out;
    }

    kvs_ascii_upper(argv[0]);
    const char *cmd = argv[0];

    if (g_cfg.role == ROLE_SLAVE && !from_replication && is_readonly_slave_blocked(cmd)) {
        n = resp_error(resp, BUFFER_CAP, "read only slave");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "PING")) {
        if (argc >= 2) n = resp_bulk(resp, BUFFER_CAP, argv[1], strlen(argv[1]));
        else n = resp_simple_string(resp, BUFFER_CAP, "PONG");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "ECHO") && argc == 2) {
        n = resp_bulk(resp, BUFFER_CAP, argv[1], strlen(argv[1]));
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "QUIT")) {
        n = resp_simple_string(resp, BUFFER_CAP, "OK");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "COMMAND")) {
        n = resp_empty_array(resp, BUFFER_CAP);
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "CLIENT")) {
        if (argc >= 2 && !strcmp(argv[1], "SETINFO")) {
            n = resp_simple_string(resp, BUFFER_CAP, "OK");
        } else {
            n = resp_array_two_bulk(resp, BUFFER_CAP, "id", "1");
            if (n < 0) n = resp_error(resp, BUFFER_CAP, "client subcommand failed");
        }
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "HELLO")) {
        n = resp_error(resp, BUFFER_CAP, "NOPROTO unsupported RESP version");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "REPLSYNC")) { repl_add_slave(c); queue_snapshot(c); return 0; }
    if (!strcmp(cmd, "REPLDONE")) return 0;
    if (!strcmp(cmd, "INFO")) {
        char info[2048];

        int replicas = 0;
        pthread_mutex_lock(&g_repl_lock);
        for (conn_t *rc = g_replicas; rc; rc = rc->next_replica) replicas++;
        pthread_mutex_unlock(&g_repl_lock);

        snprintf(info, sizeof(info),
            "role:%s\n"
            "mem:%s\n"
            "dirty:%llu\n"
            "last_snapshot_ms:%lld\n"
            "autosnap_rules:%d\n"
            "bgsave:%s\n"
            "bgsave_pid:%ld\n"
            "aof_fsync:%s\n"
            "aof_rewrite:%s\n"
            "aof_rewrite_pid:%ld\n"
            "master_host:%s\n"
            "master_port:%d\n"
            "master_link:%s\n"
            "replicas:%d",
            g_cfg.role == ROLE_MASTER ? "master" : "slave",
            kvs_mem_backend_name(),
            (unsigned long long)persist_dirty_count(),
            persist_last_snapshot_ms(),
            g_cfg.autosnap_rule_count,
            persist_bgsave_state_name(),
            (long)g_bgsave_pid,
            persist_aof_policy_name(),
            persist_bgrewriteaof_state_name(),
            (long)(persist_bgrewriteaof_in_progress() ? 1 : -1),
            g_cfg.master_host[0] ? g_cfg.master_host : "",
            g_cfg.master_port,
            repl_master_link_state_name(),
            replicas);

        n = resp_bulk(resp, BUFFER_CAP, info, strlen(info));
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "MEMSTAT")) {
        char *info = (char *)kvs_malloc(BUFFER_CAP);
        n = build_memstat_text(info, BUFFER_CAP);
        if (n < 0) n = resp_error(resp, BUFFER_CAP, "memstat build failed");
        else n = resp_bulk(resp, BUFFER_CAP, info, (size_t)n);
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "SAVE")) {
        n = (persist_save_dump() == 0) ? resp_simple_string(resp, BUFFER_CAP, "OK") : resp_error(resp, BUFFER_CAP, "save failed");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "BGSAVE")) {
        int brc = persist_bgsave_start();
        if (brc == 0) n = resp_simple_string(resp, BUFFER_CAP, "Background saving started");
        else if (brc == 1) n = resp_error(resp, BUFFER_CAP, "background saving already in progress");
        else n = resp_error(resp, BUFFER_CAP, "bgsave failed");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "BGREWRITEAOF")) {
        int rrc = persist_bgrewriteaof_start();
        if (rrc == 0) n = resp_simple_string(resp, BUFFER_CAP, "Background append only file rewriting started");
        else if (rrc == 1) n = resp_error(resp, BUFFER_CAP, "aof rewrite already in progress");
        else n = resp_error(resp, BUFFER_CAP, "bgrewriteaof failed");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "APPENDFSYNC") && argc == 2) {
        kvs_aof_fsync_policy_t policy;
        if (parse_appendfsync_policy(argv[1], &policy) != 0 || persist_set_aof_policy(policy) != 0)
            n = resp_error(resp, BUFFER_CAP, "invalid fsync policy");
        else
            n = resp_simple_string(resp, BUFFER_CAP, "OK");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "CONFIG") && argc == 3) {
        if (!strcasecmp(argv[1], "APPENDFSYNC")) {
            kvs_aof_fsync_policy_t policy;
            if (parse_appendfsync_policy(argv[2], &policy) != 0 || persist_set_aof_policy(policy) != 0)
                n = resp_error(resp, BUFFER_CAP, "invalid fsync policy");
            else
                n = resp_simple_string(resp, BUFFER_CAP, "OK");
        } else {
            n = resp_error(resp, BUFFER_CAP, "unsupported config option");
        }
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "SNAPRULE") && argc == 3) {
        int arc = persist_register_autosnap_rule(atoll(argv[1]), atoll(argv[2]));
        n = (arc == 0) ? resp_simple_string(resp, BUFFER_CAP, "OK") : resp_error(resp, BUFFER_CAP, "snaprule failed");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "SNAPRULES")) {
        char *info = (char *)kvs_malloc(BUFFER_CAP);
        n = persist_build_autosnap_text(info, BUFFER_CAP);
        if (n < 0) n = resp_error(resp, BUFFER_CAP, "snaprules build failed");
        else n = resp_bulk(resp, BUFFER_CAP, info, (size_t)n);
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "SNAPRULECLEAR")) {
        persist_clear_autosnap_rules();
        n = resp_simple_string(resp, BUFFER_CAP, "OK");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }

    if (!strcmp(cmd, "LOCK") && argc == 4) {
        int lrc = lock_acquire(KVS_ENGINE_ARRAY, argv[1], argv[2], atoll(argv[3]));
        if (lrc == 0) {
            n = resp_integer(resp, BUFFER_CAP, 1);
            if (!from_replication) {
                persist_note_write();
                persist_append_raw(raw, rawlen);
                if (g_cfg.role == ROLE_MASTER) repl_broadcast(raw, rawlen);
            }
        } else if (lrc == 1) {
            n = resp_integer(resp, BUFFER_CAP, 0);
        } else {
            n = resp_error(resp, BUFFER_CAP, "lock failed");
        }
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "UNLOCK") && argc == 3) {
        int lrc = lock_release(KVS_ENGINE_ARRAY, argv[1], argv[2]);
        if (lrc == 0) {
            n = resp_integer(resp, BUFFER_CAP, 1);
            if (!from_replication) {
                persist_note_write();
                persist_append_raw(raw, rawlen);
                if (g_cfg.role == ROLE_MASTER) repl_broadcast(raw, rawlen);
            }
        } else if (lrc == 1) {
            n = resp_integer(resp, BUFFER_CAP, 0);
        } else {
            n = resp_error(resp, BUFFER_CAP, "unlock failed");
        }
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "RENEW") && argc == 4) {
        int lrc = lock_renew(KVS_ENGINE_ARRAY, argv[1], argv[2], atoll(argv[3]));
        if (lrc == 0) {
            n = resp_integer(resp, BUFFER_CAP, 1);
            if (!from_replication) {
                persist_note_write();
                persist_append_raw(raw, rawlen);
                if (g_cfg.role == ROLE_MASTER) repl_broadcast(raw, rawlen);
            }
        } else if (lrc == 1) {
            n = resp_integer(resp, BUFFER_CAP, 0);
        } else {
            n = resp_error(resp, BUFFER_CAP, "renew failed");
        }
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "OWNER") && argc == 2) {
        try_expire(KVS_ENGINE_ARRAY, argv[1]);
        char *v = engine_get(KVS_ENGINE_ARRAY, argv[1]);
        n = v ? resp_bulk(resp, BUFFER_CAP, v, strlen(v)) : resp_null_bulk(resp, BUFFER_CAP);
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "SLAVEOF")) {
        if (argc == 3 && !strcasecmp(argv[1], "NO") && !strcasecmp(argv[2], "ONE")) {
            if (repl_slaveof_noone() != 0)
                n = resp_error(resp, BUFFER_CAP, "slaveof no one failed");
            else
                n = resp_simple_string(resp, BUFFER_CAP, "OK");
            if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
            return 0;
        }

        if (argc == 3) {
            int port = atoi(argv[2]);
            if (port <= 0 || repl_slaveof(argv[1], port) != 0)
                n = resp_error(resp, BUFFER_CAP, "slaveof failed");
            else
                n = resp_simple_string(resp, BUFFER_CAP, "OK");
            if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
            return 0;
        }

        n = resp_error(resp, BUFFER_CAP, "wrong args for SLAVEOF");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }
    if (!strcmp(cmd, "ROLE")) {
        if (g_cfg.role == ROLE_MASTER) {
            n = resp_array_two_bulk(resp, BUFFER_CAP, "master", "0");
        } else {
            char mp[32];
            snprintf(mp, sizeof(mp), "%d", g_cfg.master_port);
            n = snprintf(resp, BUFFER_CAP,
                "*3\r\n$5\r\nslave\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                strlen(g_cfg.master_host), g_cfg.master_host,
                strlen(mp), mp);
        }
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }

    if (!strcmp(cmd, "DOCSET") && argc == 4) {
        int drc = kvs_doc_set(&global_doc, argv[1], argv[2], argv[3]);
        if (drc == 0) {
            n = resp_simple_string(resp, BUFFER_CAP, "OK");
            if (!from_replication) {
                persist_note_write();
                persist_append_raw(raw, rawlen);
                if (g_cfg.role == ROLE_MASTER) repl_broadcast(raw, rawlen);
            }
        } else {
            n = resp_error(resp, BUFFER_CAP, "docset failed");
        }
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
    }
    if (!strcmp(cmd, "DOCGET") && argc == 3) {
        char *v = kvs_doc_get(&global_doc, argv[1], argv[2]);
        n = v ? resp_bulk(resp, BUFFER_CAP, v, strlen(v)) : resp_null_bulk(resp, BUFFER_CAP);
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
    }
    if (!strcmp(cmd, "DOCDEL") && argc == 3) {
        int drc = kvs_doc_del_field(&global_doc, argv[1], argv[2]);
        if (drc == 0) {
            n = resp_simple_string(resp, BUFFER_CAP, "OK");
            if (!from_replication) {
                persist_note_write();
                persist_append_raw(raw, rawlen);
                if (g_cfg.role == ROLE_MASTER) repl_broadcast(raw, rawlen);
            }
        } else if (drc == 1) {
            n = resp_error(resp, BUFFER_CAP, "doc or field not found");
        } else {
            n = resp_error(resp, BUFFER_CAP, "docdel failed");
        }
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
    }
    if (!strcmp(cmd, "DOCDROP") && argc == 2) {
        int drc = kvs_doc_del(&global_doc, argv[1]);
        if (drc == 0) {
            n = resp_simple_string(resp, BUFFER_CAP, "OK");
            if (!from_replication) {
                persist_note_write();
                persist_append_raw(raw, rawlen);
                if (g_cfg.role == ROLE_MASTER) repl_broadcast(raw, rawlen);
            }
        } else if (drc == 1) {
            n = resp_error(resp, BUFFER_CAP, "doc not found");
        } else {
            n = resp_error(resp, BUFFER_CAP, "docdrop failed");
        }
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
    }
    if (!strcmp(cmd, "DOCEXIST") && argc == 2) {
        n = resp_integer(resp, BUFFER_CAP, kvs_doc_exist(&global_doc, argv[1]) == 0 ? 1 : 0);
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
    }
    if (!strcmp(cmd, "DOCCOUNT") && argc == 2) {
        int cnt = kvs_doc_field_count(&global_doc, argv[1]);
        n = resp_integer(resp, BUFFER_CAP, cnt);
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
    }
    if (!strcmp(cmd, "DOCGETALL") && argc == 2) {
        int cnt = kvs_doc_field_count(&global_doc, argv[1]);
        if (cnt <= 0) {
            n = resp_empty_array(resp, BUFFER_CAP);
            if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
            goto out;
        }
        size_t pos = 0;
        int hn = resp_array_header(resp, BUFFER_CAP, cnt * 2);
        if (hn < 0) { n = resp_error(resp, BUFFER_CAP, "docgetall failed"); if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n); goto out; }
        pos += (size_t)hn;
        kvs_doc_t *d = NULL;
        if (global_doc.buckets) {
            unsigned int didx;
            for (didx = 0; didx < (unsigned int)global_doc.size; ++didx) {
                for (kvs_doc_t *dd = global_doc.buckets[didx]; dd; dd = dd->next) {
                    if (strcmp(dd->key, argv[1]) == 0) { d = dd; break; }
                }
                if (d) break;
            }
        }
        if (d) {
            for (int bi = 0; bi < d->bucket_count; ++bi) {
                for (kvs_doc_field_t *f = d->fields[bi]; f; f = f->next) {
                    if (resp_array_append_bulk_or_null(resp, BUFFER_CAP, &pos, f->name) != 0) break;
                    if (resp_array_append_bulk_or_null(resp, BUFFER_CAP, &pos, f->value) != 0) break;
                }
            }
        }
        n = (int)pos;
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
    }

    int engine = cmd_engine(cmd);
    const char *op = strip_prefix(cmd);
    int rc = -1;
    int should_reply_ok = 0;
    int should_reply_missing = 0;

    if (!strcmp(op, "SET") && argc == 3) {
        try_expire(engine, argv[1]);
        rc = engine_set(engine, argv[1], argv[2]);
        should_reply_ok = 1;
        should_reply_missing = 1;
    }
    else if (!strcmp(op, "MSET")) {
        try_expire(engine, argv[1]);
        rc = handle_multi_set(engine, argc, argv);
        should_reply_ok = 1;
    }
    else if (!strcmp(op, "MOD") && argc == 3) {
        try_expire(engine, argv[1]);
        rc = engine_mod(engine, argv[1], argv[2]);
        should_reply_ok = 1;
        should_reply_missing = 1;
    }
    else if (!strcmp(op, "DEL") && argc == 2) {
        try_expire(engine, argv[1]);
        rc = engine_del(engine, argv[1]);
        kvs_expire_del(&global_expire, engine, argv[1]);
        should_reply_ok = 1;
        should_reply_missing = 1;
    }
    else if (!strcmp(op, "GET") && argc == 2) {
        try_expire(engine, argv[1]);
        char *v = engine_get(engine, argv[1]);
        n = v ? resp_bulk(resp, BUFFER_CAP, v, strlen(v)) : resp_null_bulk(resp, BUFFER_CAP);
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    } else if (!strcmp(op, "MGET")) {
        n = handle_multi_get(engine, argc, argv, resp, BUFFER_CAP);
        if (n < 0) n = resp_error(resp, BUFFER_CAP, "mget failed");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    } else if (!strcmp(op, "EXIST") && argc == 2) {
        try_expire(engine, argv[1]);
        n = resp_integer(resp, BUFFER_CAP, engine_exist(engine, argv[1]) == 0 ? 1 : 0);
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    } else if (!strcmp(op, "EXPIRE") && argc == 3) {
        try_expire(engine, argv[1]);
        if (engine_exist(engine, argv[1]) != 0) rc = 1;
        else rc = kvs_expire_set(&global_expire, engine, argv[1], atoll(argv[2]) * 1000);
        should_reply_ok = 1;
        should_reply_missing = 1;
    } else if (!strcmp(op, "TTL") && argc == 2) {
        try_expire(engine, argv[1]);
        if (engine_exist(engine, argv[1]) != 0) n = resp_integer(resp, BUFFER_CAP, -2);
        else {
            long long ttl = kvs_expire_ttl(&global_expire, engine, argv[1]);
            n = resp_integer(resp, BUFFER_CAP, ttl);
        }
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    } else if (!strcmp(op, "PERSIST") && argc == 2) {
        try_expire(engine, argv[1]);
        if (engine_exist(engine, argv[1]) != 0) rc = 1;
        else rc = kvs_expire_persist(&global_expire, engine, argv[1]);
        should_reply_ok = 1;
        should_reply_missing = 1;
    } else {
        int expected_argc = 0;
        if (!strcmp(op, "SET") || !strcmp(op, "MOD") || !strcmp(op, "EXPIRE")) expected_argc = 3;
        else if (!strcmp(op, "GET") || !strcmp(op, "DEL") || !strcmp(op, "EXIST") || !strcmp(op, "TTL") || !strcmp(op, "PERSIST")) expected_argc = 2;
        if (expected_argc > 0 && argc != expected_argc)
            n = resp_error(resp, BUFFER_CAP, "wrong number of arguments");
        else
            n = resp_error(resp, BUFFER_CAP, "unknown command or wrong args");
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    }

    if (rc == 0) {
        n = resp_simple_string(resp, BUFFER_CAP, "OK");
        if (!from_replication && is_write_cmd(cmd)) {
            persist_note_write();
            persist_append_raw(raw, rawlen);
            if (g_cfg.role == ROLE_MASTER) repl_broadcast(raw, rawlen);
        }
    } else if (rc == 1 && should_reply_missing) {
        if (!strcmp(op, "SET")) n = resp_simple_string(resp, BUFFER_CAP, "OK");
        else n = resp_error(resp, BUFFER_CAP, "not found or exists");
    } else {
        n = resp_error(resp, BUFFER_CAP, "operation failed");
    }
    if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
    out:
    kvs_free(resp);
    return rc_ret;
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
        int incomplete = 0;
        int malformed = 0;

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
        for (int i = 0; i < argc; ++i) {
            if (p >= *len) {
                incomplete = 1;
                break;
            }
            if (buf[p] != '$') {
                malformed = 1;
                break;
            }
            size_t lp = p + 1;
            while (lp + 1 < *len && !(buf[lp] == '\r' && buf[lp + 1] == '\n')) lp++;
            if (lp + 1 >= *len) {
                incomplete = 1;
                break;
            }
            char lbuf[32] = {0};
            memcpy(lbuf, buf + p + 1, lp - (p + 1));
            char *endp = NULL;
            long blen = strtol(lbuf, &endp, 10);
            if (!endp || *endp != '\0' || blen < 0) {
                malformed = 1;
                if (c) {
                    char r[128];
                    int n = resp_error(r, sizeof(r), "invalid bulk length");
                    queue_bytes(c, (unsigned char *)r, (size_t)n);
                }
                break;
            }
            p = lp + 2;
            if (p + (size_t)blen + 2 > *len) {
                incomplete = 1;
                break;
            }
            argv[i] = (char *)kvs_malloc((size_t)blen + 1);
            if (!argv[i]) {
                malformed = 1;
                break;
            }
            memcpy(argv[i], buf + p, (size_t)blen);
            argv[i][blen] = 0;
            argl[i] = (size_t)blen;
            p += (size_t)blen;
            if (!(buf[p] == '\r' && buf[p + 1] == '\n')) {
                malformed = 1;
                break;
            }
            p += 2;
        }
        if (incomplete) {
            for (int i = 0; i < argc; ++i) kvs_free(argv[i]);
            break;
        }
        if (malformed) {
            for (int i = 0; i < argc; ++i) kvs_free(argv[i]);
            if (p > start) pos = p;
            else break;
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

static int emit_cmd4_fp(FILE *fp, const char *cmd, const char *a1, const char *a2, const char *a3) {
    unsigned char buf[BUFFER_CAP];
    size_t n = resp_build_cmd4(buf, sizeof(buf), cmd, a1, a2, a3);
    return fwrite(buf, 1, n, fp) == n ? 0 : -1;
}

static int maybe_emit_expire(FILE *fp, int engine, const char *key) {
    long long ttl = kvs_expire_ttl(&global_expire, engine, key);
    if (ttl < 0) return 0;
    char sec[32]; snprintf(sec, sizeof(sec), "%lld", ttl);
    const char *cmd =
        engine == KVS_ENGINE_ARRAY ? "EXPIRE" :
        engine == KVS_ENGINE_RBTREE ? "REXPIRE" :
        engine == KVS_ENGINE_HASH ? "HEXPIRE" : "XEXPIRE";
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

static int snapshot_skiptable_cb(const char *key, const char *value, void *arg) {
    FILE *fp = (FILE *)arg;
    if (emit_cmd3_fp(fp, "XSET", key, value) != 0) return -1;
    if (maybe_emit_expire(fp, KVS_ENGINE_SKIPTABLE, key) != 0) return -1;
    return 0;
}

static int snapshot_skiptable(FILE *fp) {
    return kvs_skiptable_foreach(&global_skiptable, snapshot_skiptable_cb, fp);
}

static int snapshot_doc_field_cb(const char *name, const char *value, void *arg) {
    void **ctx = (void **)arg;
    FILE *fp = (FILE *)ctx[0];
    const char *key = (const char *)ctx[1];
    return emit_cmd4_fp(fp, "DOCSET", key, name, value);
}

static int snapshot_doc_cb(const char *key, kvs_doc_t *doc, void *arg) {
    FILE *fp = (FILE *)arg;
    (void)doc;
    void *ctx[2] = { fp, (void *)key };
    return kvs_doc_foreach_field(&global_doc, key, snapshot_doc_field_cb, ctx);
}

static int snapshot_doc(FILE *fp) {
    return kvs_doc_foreach(&global_doc, snapshot_doc_cb, fp);
}

int kvs_snapshot_to_fp(FILE *fp) {
    if (!fp) return -1;
    if (snapshot_array(fp) != 0) return -1;
    if (snapshot_rbtree_node(fp, global_rbtree.root, global_rbtree.nil) != 0) return -1;
    if (snapshot_hash(fp) != 0) return -1;
    if (snapshot_skiptable(fp) != 0) return -1;
    if (snapshot_doc(fp) != 0) return -1;
    return 0;
}

int main(int argc, char **argv) {
    if (parse_args(argc, argv) != 0) {
        fprintf(stderr, "Usage: %s [--config kvstore.conf] [--port 5000] [--role master|slave] [--master-host 127.0.0.1 --master-port 5000] [--mem libc|jemalloc|custom] [--net reactor|proactor|ntyco] [--appendfsync always|everysec] [--autosnap 60:1000,300:10]\n", argv[0]);
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
    kvs_skiptable_create(&global_skiptable);
    kvs_expire_create(&global_expire);
    kvs_doc_create(&global_doc);
   
    if (g_cfg.is_sentinel) {
        return sentinel_start();
    }
    if (persist_init() != 0) { perror("persist_init"); return 1; }
    persist_recover();

    if (!strcmp(g_cfg.net_backend, "reactor")) {
        return reactor_start();
    } else if (!strcmp(g_cfg.net_backend, "proactor")) {
        return proactor_start((unsigned short)g_cfg.port);
    } else if (!strcmp(g_cfg.net_backend, "ntyco")) {
        return ntyco_start((unsigned short)g_cfg.port);
    } else {
        fprintf(stderr, "unknown net backend: %s\n", g_cfg.net_backend);
        return 1;
    }
}
