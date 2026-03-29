#include "kvstore.h"

#if ENABLE_ARRAY
extern kvs_array_t global_array;
#endif
#if ENABLE_RBTREE
extern kvs_rbtree_t global_rbtree;
#endif
#if ENABLE_HASH
extern kvs_hash_t global_hash;
#endif

void *kvs_malloc(size_t size) { return malloc(size); }
void kvs_free(void *ptr) { free(ptr); }

static int str_eq_nocase(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a >= 'a' && *a <= 'z' ? *a - 32 : *a;
        char cb = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
        if (ca != cb) return 0;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

static char *dup_arg(const kvs_resp_arg_t *a) {
    char *s = kvs_malloc(a->len + 1);
    if (!s) return NULL;
    memcpy(s, a->data, a->len);
    s[a->len] = 0;
    return s;
}

void kvs_resp_request_reset(kvs_resp_request_t *req) {
    if (!req) return;
    for (int i = 0; i < req->argc; ++i) kvs_free(req->argv[i].data);
    memset(req, 0, sizeof(*req));
}

static int parse_line(const unsigned char *buf, size_t len, size_t start, size_t *line_end) {
    for (size_t i = start; i + 1 < len; ++i) {
        if (buf[i] == '\r' && buf[i+1] == '\n') { *line_end = i; return 1; }
    }
    return 0;
}

int kvs_resp_parse_one(const unsigned char *buf, size_t len, size_t *consumed, kvs_resp_request_t *req, char *errbuf, size_t errcap) {
    *consumed = 0;
    memset(req, 0, sizeof(*req));
    if (len == 0) return 0;
    if (buf[0] != '*') {
        snprintf(errbuf, errcap, "invalid resp type");
        return -1;
    }
    size_t line_end = 0;
    if (!parse_line(buf, len, 1, &line_end)) return 0;
    char tmp[32];
    size_t nlen = line_end - 1;
    if (nlen >= sizeof(tmp)) { snprintf(errbuf, errcap, "invalid array len"); return -1; }
    memcpy(tmp, buf + 1, nlen); tmp[nlen] = 0;
    int argc = atoi(tmp);
    if (argc <= 0 || argc > KVS_MAX_ARGS) { snprintf(errbuf, errcap, "invalid array len"); return -1; }
    req->argc = argc;
    size_t pos = line_end + 2;
    for (int i = 0; i < argc; ++i) {
        if (pos >= len) return 0;
        if (buf[pos] != '$') { snprintf(errbuf, errcap, "invalid bulk type"); kvs_resp_request_reset(req); return -1; }
        if (!parse_line(buf, len, pos + 1, &line_end)) { kvs_resp_request_reset(req); return 0; }
        nlen = line_end - (pos + 1);
        if (nlen >= sizeof(tmp)) { snprintf(errbuf, errcap, "invalid bulk length"); kvs_resp_request_reset(req); return -1; }
        memcpy(tmp, buf + pos + 1, nlen); tmp[nlen] = 0;
        long blen = strtol(tmp, NULL, 10);
        if (blen < 0 || blen > 1024 * 1024) { snprintf(errbuf, errcap, "invalid bulk length"); kvs_resp_request_reset(req); return -1; }
        pos = line_end + 2;
        if (pos + (size_t)blen + 2 > len) { kvs_resp_request_reset(req); return 0; }
        req->argv[i].data = kvs_malloc((size_t)blen + 1);
        memcpy(req->argv[i].data, buf + pos, (size_t)blen);
        req->argv[i].data[blen] = 0;
        req->argv[i].len = (size_t)blen;
        if (buf[pos + blen] != '\r' || buf[pos + blen + 1] != '\n') { snprintf(errbuf, errcap, "invalid bulk tail"); kvs_resp_request_reset(req); return -1; }
        pos += (size_t)blen + 2;
    }
    *consumed = pos;
    return 1;
}

static size_t encode_simple(unsigned char *out, size_t cap, const char *s) { return (size_t)snprintf((char*)out, cap, "+%s\r\n", s); }
static size_t encode_error(unsigned char *out, size_t cap, const char *s) { return (size_t)snprintf((char*)out, cap, "-ERR %s\r\n", s); }
static size_t encode_integer(unsigned char *out, size_t cap, long long v) { return (size_t)snprintf((char*)out, cap, ":%lld\r\n", v); }
static size_t encode_null(unsigned char *out, size_t cap) { return (size_t)snprintf((char*)out, cap, "$-1\r\n"); }
static size_t encode_bulk(unsigned char *out, size_t cap, const char *s) {
    if (!s) return encode_null(out, cap);
    size_t slen = strlen(s);
    int n = snprintf((char*)out, cap, "$%zu\r\n", slen);
    if (n < 0 || (size_t)n + slen + 2 > cap) return 0;
    memcpy(out + n, s, slen);
    memcpy(out + n + slen, "\r\n", 2);
    return (size_t)n + slen + 2;
}

static int engine_exist(int engine, char *key) {
    switch (engine) {
#if ENABLE_ARRAY
        case KVS_ENGINE_ARRAY: return kvs_array_exist(&global_array, key);
#endif
#if ENABLE_RBTREE
        case KVS_ENGINE_RBTREE: return kvs_rbtree_exist(&global_rbtree, key);
#endif
#if ENABLE_HASH
        case KVS_ENGINE_HASH: return kvs_hash_exist(&global_hash, key);
#endif
        default: return 1;
    }
}

static char *engine_get(int engine, char *key) {
    switch (engine) {
#if ENABLE_ARRAY
        case KVS_ENGINE_ARRAY: return kvs_array_get(&global_array, key);
#endif
#if ENABLE_RBTREE
        case KVS_ENGINE_RBTREE: return kvs_rbtree_get(&global_rbtree, key);
#endif
#if ENABLE_HASH
        case KVS_ENGINE_HASH: return kvs_hash_get(&global_hash, key);
#endif
        default: return NULL;
    }
}

static int engine_set(int engine, char *key, char *val) {
    switch (engine) {
#if ENABLE_ARRAY
        case KVS_ENGINE_ARRAY: return kvs_array_set(&global_array, key, val);
#endif
#if ENABLE_RBTREE
        case KVS_ENGINE_RBTREE: return kvs_rbtree_set(&global_rbtree, key, val);
#endif
#if ENABLE_HASH
        case KVS_ENGINE_HASH: return kvs_hash_set(&global_hash, key, val);
#endif
        default: return -1;
    }
}

static int engine_mod(int engine, char *key, char *val) {
    switch (engine) {
#if ENABLE_ARRAY
        case KVS_ENGINE_ARRAY: return kvs_array_mod(&global_array, key, val);
#endif
#if ENABLE_RBTREE
        case KVS_ENGINE_RBTREE: return kvs_rbtree_mod(&global_rbtree, key, val);
#endif
#if ENABLE_HASH
        case KVS_ENGINE_HASH: return kvs_hash_mod(&global_hash, key, val);
#endif
        default: return -1;
    }
}

static int engine_del(int engine, char *key) {
    switch (engine) {
#if ENABLE_ARRAY
        case KVS_ENGINE_ARRAY: return kvs_array_del(&global_array, key);
#endif
#if ENABLE_RBTREE
        case KVS_ENGINE_RBTREE: return kvs_rbtree_del(&global_rbtree, key);
#endif
#if ENABLE_HASH
        case KVS_ENGINE_HASH: return kvs_hash_del(&global_hash, key);
#endif
        default: return -1;
    }
}

static int try_expire_key(int engine, char *key) {
    if (kvs_expire_is_expired(&global_expire, engine, key)) {
        engine_del(engine, key);
        kvs_expire_del(&global_expire, engine, key);
        kvs_dataset_del(engine, key);
        return 1;
    }
    return 0;
}

static int cmd_to_engine(const char *cmd, const char **base) {
    if (cmd[0] == 'R') { *base = cmd + 1; return KVS_ENGINE_RBTREE; }
    if (cmd[0] == 'H') { *base = cmd + 1; return KVS_ENGINE_HASH; }
    *base = cmd; return KVS_ENGINE_ARRAY;
}

static int is_write_command(const char *base) {
    return str_eq_nocase(base, "SET") || str_eq_nocase(base, "DEL") || str_eq_nocase(base, "MOD") ||
           str_eq_nocase(base, "EXPIRE") || str_eq_nocase(base, "PERSIST") || str_eq_nocase(base, "SAVE");
}

static int handle_set_like(int engine, char *key, char *value, unsigned char *out, size_t outcap, size_t *outlen, int mode) {
    int ret = mode == 0 ? engine_set(engine, key, value) : engine_mod(engine, key, value);
    if (ret < 0) { *outlen = encode_error(out, outcap, "write failed"); return 0; }
    if (mode == 0 && ret > 0) { *outlen = encode_error(out, outcap, "key exists"); return 0; }
    if (mode == 1 && ret > 0) { *outlen = encode_null(out, outcap); return 0; }
    kvs_dataset_set(engine, key, value);
    *outlen = encode_simple(out, outcap, "OK");
    return 0;
}

static int execute_request(const kvs_resp_request_t *req, unsigned char *out, size_t outcap, size_t *outlen, int internal_replay) {
    if (req->argc <= 0) { *outlen = encode_error(out, outcap, "invalid request"); return -1; }
    char *cmd = dup_arg(&req->argv[0]);
    if (!cmd) { *outlen = encode_error(out, outcap, "oom"); return -1; }
    const char *base = NULL;
    int engine = cmd_to_engine(cmd, &base);

    char *key = req->argc > 1 ? dup_arg(&req->argv[1]) : NULL;
    char *value = req->argc > 2 ? dup_arg(&req->argv[2]) : NULL;
    long long ttl = value ? atoll(value) : 0;

    if (str_eq_nocase(base, "SET")) {
        if (req->argc != 3) *outlen = encode_error(out, outcap, "wrong number of arguments");
        else handle_set_like(engine, key, value, out, outcap, outlen, 0);
    } else if (str_eq_nocase(base, "GET")) {
        if (req->argc != 2) *outlen = encode_error(out, outcap, "wrong number of arguments");
        else { try_expire_key(engine, key); *outlen = encode_bulk(out, outcap, engine_get(engine, key)); }
    } else if (str_eq_nocase(base, "DEL")) {
        if (req->argc != 2) *outlen = encode_error(out, outcap, "wrong number of arguments");
        else {
            try_expire_key(engine, key);
            int ret = engine_del(engine, key);
            kvs_expire_del(&global_expire, engine, key);
            kvs_dataset_del(engine, key);
            *outlen = ret == 0 ? encode_simple(out, outcap, "OK") : encode_null(out, outcap);
        }
    } else if (str_eq_nocase(base, "MOD")) {
        if (req->argc != 3) *outlen = encode_error(out, outcap, "wrong number of arguments");
        else { try_expire_key(engine, key); handle_set_like(engine, key, value, out, outcap, outlen, 1); }
    } else if (str_eq_nocase(base, "EXIST")) {
        if (req->argc != 2) *outlen = encode_error(out, outcap, "wrong number of arguments");
        else { try_expire_key(engine, key); *outlen = encode_integer(out, outcap, engine_exist(engine, key) == 0 ? 1 : 0); }
    } else if (str_eq_nocase(base, "EXPIRE")) {
        if (req->argc != 3) *outlen = encode_error(out, outcap, "wrong number of arguments");
        else {
            try_expire_key(engine, key);
            if (engine_exist(engine, key) != 0) *outlen = encode_integer(out, outcap, 0);
            else {
                kvs_expire_set(&global_expire, engine, key, ttl * 1000);
                kvs_dataset_expire(engine, key, kvs_now_ms() + ttl * 1000);
                *outlen = encode_simple(out, outcap, "OK");
            }
        }
    } else if (str_eq_nocase(base, "TTL")) {
        if (req->argc != 2) *outlen = encode_error(out, outcap, "wrong number of arguments");
        else {
            try_expire_key(engine, key);
            if (engine_exist(engine, key) != 0) *outlen = encode_integer(out, outcap, -2);
            else *outlen = encode_integer(out, outcap, kvs_expire_ttl(&global_expire, engine, key));
        }
    } else if (str_eq_nocase(base, "PERSIST")) {
        if (req->argc != 2) *outlen = encode_error(out, outcap, "wrong number of arguments");
        else {
            try_expire_key(engine, key);
            if (engine_exist(engine, key) != 0) *outlen = encode_integer(out, outcap, 0);
            else {
                kvs_expire_persist(&global_expire, engine, key);
                kvs_dataset_persist(engine, key);
                *outlen = encode_simple(out, outcap, "OK");
            }
        }
    } else if (str_eq_nocase(base, "SAVE")) {
        if (req->argc != 1) *outlen = encode_error(out, outcap, "wrong number of arguments");
        else *outlen = kvs_persist_save_snapshot() == 0 ? encode_simple(out, outcap, "OK") : encode_error(out, outcap, "save failed");
    } else {
        *outlen = encode_error(out, outcap, "unknown command");
    }

    if (!internal_replay && req->argc > 0 && is_write_command(base)) kvs_persist_append_command(req);
    kvs_free(cmd); kvs_free(key); kvs_free(value);
    return 0;
}

int kvs_dispatch_request(const kvs_resp_request_t *req, unsigned char *out, size_t outcap, size_t *outlen, int internal_replay) {
    return execute_request(req, out, outcap, outlen, internal_replay);
}

void kvs_stream_init(kvs_stream_t *s) { memset(s, 0, sizeof(*s)); }

void kvs_stream_free(kvs_stream_t *s) {
    kvs_out_node_t *n = s->out_head;
    while (n) { kvs_out_node_t *next = n->next; kvs_free(n->data); kvs_free(n); n = next; }
    memset(s, 0, sizeof(*s));
}

int kvs_stream_enqueue(kvs_stream_t *s, const unsigned char *data, size_t len) {
    kvs_out_node_t *n = kvs_malloc(sizeof(*n));
    if (!n) return -1;
    memset(n, 0, sizeof(*n));
    n->data = kvs_malloc(len);
    if (!n->data) { kvs_free(n); return -1; }
    memcpy(n->data, data, len); n->len = len;
    if (!s->out_tail) s->out_head = s->out_tail = n;
    else { s->out_tail->next = n; s->out_tail = n; }
    s->out_queued_bytes += len;
    return 0;
}

static size_t find_next_star(const unsigned char *buf, size_t len, size_t start) {
    for (size_t i = start; i < len; ++i) if (buf[i] == '*') return i;
    return len;
}

int kvs_stream_feed(kvs_stream_t *s, const unsigned char *data, size_t len) {
    if (s->in_len + len > sizeof(s->inbuf)) return -1;
    memcpy(s->inbuf + s->in_len, data, len);
    s->in_len += len;
    while (s->in_len > 0) {
        kvs_resp_request_t req; char err[128] = {0}; size_t used = 0;
        int rc = kvs_resp_parse_one(s->inbuf, s->in_len, &used, &req, err, sizeof(err));
        if (rc == 0) break;
        if (rc < 0) {
            unsigned char out[256];
            size_t outlen = encode_error(out, sizeof(out), err[0] ? err : "protocol error");
            kvs_stream_enqueue(s, out, outlen);
            size_t next = find_next_star(s->inbuf, s->in_len, 1);
            if (next >= s->in_len) s->in_len = 0;
            else { memmove(s->inbuf, s->inbuf + next, s->in_len - next); s->in_len -= next; }
            continue;
        }
        unsigned char out[BUFFER_LENGTH]; size_t outlen = 0;
        kvs_dispatch_request(&req, out, sizeof(out), &outlen, 0);
        kvs_stream_enqueue(s, out, outlen);
        kvs_resp_request_reset(&req);
        memmove(s->inbuf, s->inbuf + used, s->in_len - used);
        s->in_len -= used;
    }
    return 0;
}

int kvs_active_expire_cycle(int budget) {
    int removed = 0;
    long long now = kvs_now_ms();
    for (int i = 0; i < global_expire.size && removed < budget; ++i) {
        kvs_expire_item_t *cur = global_expire.buckets[i], *prev = NULL;
        while (cur && removed < budget) {
            if (now >= cur->expire_at_ms) {
                engine_del(cur->engine, cur->key);
                kvs_dataset_del(cur->engine, cur->key);
                if (prev) prev->next = cur->next; else global_expire.buckets[i] = cur->next;
                kvs_expire_item_t *dead = cur;
                cur = cur->next;
                kvs_free(dead->key); kvs_free(dead);
                removed++;
                continue;
            }
            prev = cur; cur = cur->next;
        }
    }
    return removed;
}

static int init_kvengine(void) {
#if ENABLE_ARRAY
    extern kvs_array_t global_array;
    memset(&global_array, 0, sizeof(global_array));
    kvs_array_create(&global_array);
#endif
#if ENABLE_RBTREE
    extern kvs_rbtree_t global_rbtree;
    memset(&global_rbtree, 0, sizeof(global_rbtree));
    kvs_rbtree_create(&global_rbtree);
#endif
#if ENABLE_HASH
    extern kvs_hash_t global_hash;
    memset(&global_hash, 0, sizeof(global_hash));
    kvs_hash_create(&global_hash);
#endif
    kvs_expire_create(&global_expire);
    kvs_persist_init("kvstore.dump", "kvstore.aof");
    kvs_persist_load_all();
    return 0;
}

static void dest_kvengine(void) {
    kvs_persist_shutdown();
    kvs_expire_destroy(&global_expire);
#if ENABLE_ARRAY
    extern kvs_array_t global_array;
    kvs_array_destory(&global_array);
#endif
#if ENABLE_RBTREE
    extern kvs_rbtree_t global_rbtree;
    kvs_rbtree_destory(&global_rbtree);
#endif
#if ENABLE_HASH
    extern kvs_hash_t global_hash;
    kvs_hash_destory(&global_hash);
#endif
}

int proactor_start(unsigned short port, msg_handler handler) { (void)port; (void)handler; return -1; }
int ntyco_start(unsigned short port, msg_handler handler) { (void)port; (void)handler; return -1; }

int kvs_protocol(char *msg, int length, char *response) { (void)msg; (void)length; (void)response; return -1; }

int main(int argc, char *argv[]) {
    if (argc != 2) return -1;
    int port = atoi(argv[1]);
    init_kvengine();
    int rc = reactor_start((unsigned short)port, kvs_protocol);
    dest_kvengine();
    return rc;
}
