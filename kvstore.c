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

int kvs_blob_dup(kvs_blob_t *dst, const unsigned char *data, size_t len) {
    dst->data = NULL;
    dst->len = 0;
    if (len == 0) return 0;
    dst->data = (unsigned char *)malloc(len);
    if (!dst->data) return -1;
    memcpy(dst->data, data, len);
    dst->len = len;
    return 0;
}

void kvs_blob_free(kvs_blob_t *b) {
    if (b && b->data) free(b->data);
    if (b) {
        b->data = NULL;
        b->len = 0;
    }
}

int kvs_blob_equal_view(const kvs_blob_t *a, const kvs_blob_view_t *b) {
    return a && b && a->len == b->len && (a->len == 0 || memcmp(a->data, b->data, a->len) == 0);
}

int kvs_blob_compare_view(const kvs_blob_t *a, const kvs_blob_view_t *b) {
    size_t min = a->len < b->len ? a->len : b->len;
    int cmp = min ? memcmp(a->data, b->data, min) : 0;
    if (cmp != 0) return cmp;
    if (a->len < b->len) return -1;
    if (a->len > b->len) return 1;
    return 0;
}

static void req_free(kvs_resp_request_t *req) {
    for (int i = 0; i < req->argc; ++i) kvs_blob_free(&req->argv[i]);
    memset(req, 0, sizeof(*req));
}

static void kvs_stream_clear_output(kvs_stream_t *s) {
    while (s->out_head) {
        kvs_out_node_t *node = s->out_head;
        s->out_head = node->next;
        free(node->data);
        free(node);
    }
    s->out_tail = NULL;
    s->out_queued_bytes = 0;
}

void kvs_stream_init(kvs_stream_t *s) {
    memset(s, 0, sizeof(*s));
    s->state = KVS_RESP_STATE_ARRAY;
    s->bulk_len = -1;
}

void kvs_stream_destroy(kvs_stream_t *s) {
    req_free(&s->req);
    kvs_stream_clear_output(s);
    memset(s, 0, sizeof(*s));
}

void kvs_stream_reset_request(kvs_stream_t *s) {
    req_free(&s->req);
    s->state = KVS_RESP_STATE_ARRAY;
    s->argc_expected = 0;
    s->argc_read = 0;
    s->bulk_len = -1;
}

void kvs_stream_compact_input(kvs_stream_t *s) {
    if (s->parse_pos == 0) return;
    if (s->parse_pos < s->in_len) {
        memmove(s->inbuf, s->inbuf + s->parse_pos, s->in_len - s->parse_pos);
    }
    s->in_len -= s->parse_pos;
    s->parse_pos = 0;
}

static int parse_crlf_number(const unsigned char *buf, size_t len, size_t *consumed, long long *value) {
    size_t i = 0;
    int neg = 0;
    long long v = 0;

    if (len == 0) return 0;
    if (buf[i] == '-') {
        neg = 1;
        i++;
    }
    if (i >= len) return 0;
    for (; i < len; ++i) {
        if (buf[i] == '\r') {
            if (i + 1 >= len) return 0;
            if (buf[i + 1] != '\n') return -1;
            *consumed = i + 2;
            *value = neg ? -v : v;
            return 1;
        }
        if (!isdigit(buf[i])) return -1;
        v = v * 10 + (buf[i] - '0');
    }
    return 0;
}

static int kvs_stream_enqueue_bytes(kvs_stream_t *s, const void *buf, size_t len) {
    kvs_out_node_t *node;
    if (len == 0) return 0;
    node = (kvs_out_node_t *)calloc(1, sizeof(*node));
    if (!node) return -1;
    node->data = (unsigned char *)malloc(len);
    if (!node->data) {
        free(node);
        return -1;
    }
    memcpy(node->data, buf, len);
    node->len = len;
    if (s->out_tail) {
        s->out_tail->next = node;
    } else {
        s->out_head = node;
    }
    s->out_tail = node;
    s->out_queued_bytes += len;
    return 0;
}

static int kvs_stream_enqueue_format(kvs_stream_t *s, const char *fmt, long long v) {
    char tmp[64];
    int n = snprintf(tmp, sizeof(tmp), fmt, v);
    return n > 0 ? kvs_stream_enqueue_bytes(s, tmp, (size_t)n) : -1;
}

static int reply_simple(kvs_stream_t *s, const char *msg) {
    return kvs_stream_enqueue_bytes(s, "+", 1) ||
           kvs_stream_enqueue_bytes(s, msg, strlen(msg)) ||
           kvs_stream_enqueue_bytes(s, "\r\n", 2);
}

static int reply_error(kvs_stream_t *s, const char *msg) {
    return kvs_stream_enqueue_bytes(s, "-ERR ", 5) ||
           kvs_stream_enqueue_bytes(s, msg, strlen(msg)) ||
           kvs_stream_enqueue_bytes(s, "\r\n", 2);
}

static int reply_integer(kvs_stream_t *s, long long v) {
    return kvs_stream_enqueue_format(s, ":%lld\r\n", v);
}

static int reply_null(kvs_stream_t *s) {
    return kvs_stream_enqueue_bytes(s, "$-1\r\n", 5);
}

static int reply_bulk(kvs_stream_t *s, const kvs_blob_t *b) {
    return kvs_stream_enqueue_format(s, "$%lld\r\n", (long long)b->len) ||
           kvs_stream_enqueue_bytes(s, b->data, b->len) ||
           kvs_stream_enqueue_bytes(s, "\r\n", 2);
}

static int view_eq_cstr(const kvs_blob_t *b, const char *s) {
    size_t n = strlen(s);
    return b->len == n && memcmp(b->data, s, n) == 0;
}

static int dispatch_request(kvs_stream_t *s, const kvs_resp_request_t *req) {
    if (req->argc < 2) return reply_error(s, "wrong number of arguments");
    const kvs_blob_t *cmd = &req->argv[0];
    kvs_blob_view_t key = { req->argv[1].data, req->argv[1].len };
    kvs_blob_view_t value = { NULL, 0 };
    if (req->argc >= 3) {
        value.data = req->argv[2].data;
        value.len = req->argv[2].len;
    }

#if ENABLE_ARRAY
    if (view_eq_cstr(cmd, "SET")) return req->argc == 3 ? (kvs_array_set(&global_array, &key, &value) == 0 ? reply_simple(s, "OK") : reply_error(s, "set failed")) : reply_error(s, "wrong argc for SET");
    if (view_eq_cstr(cmd, "GET")) { kvs_blob_t *v = kvs_array_get(&global_array, &key); return v ? reply_bulk(s, v) : reply_null(s); }
    if (view_eq_cstr(cmd, "DEL")) return reply_integer(s, kvs_array_del(&global_array, &key) == 0 ? 1 : 0);
    if (view_eq_cstr(cmd, "MOD")) return req->argc == 3 ? reply_integer(s, kvs_array_mod(&global_array, &key, &value) == 0 ? 1 : 0) : reply_error(s, "wrong argc for MOD");
    if (view_eq_cstr(cmd, "EXIST")) return reply_integer(s, kvs_array_exist(&global_array, &key) == 0 ? 1 : 0);
#endif
#if ENABLE_RBTREE
    if (view_eq_cstr(cmd, "RSET")) return req->argc == 3 ? (kvs_rbtree_set(&global_rbtree, &key, &value) == 0 ? reply_simple(s, "OK") : reply_error(s, "rset failed")) : reply_error(s, "wrong argc for RSET");
    if (view_eq_cstr(cmd, "RGET")) { kvs_blob_t *v = kvs_rbtree_get(&global_rbtree, &key); return v ? reply_bulk(s, v) : reply_null(s); }
    if (view_eq_cstr(cmd, "RDEL")) return reply_integer(s, kvs_rbtree_del(&global_rbtree, &key) == 0 ? 1 : 0);
    if (view_eq_cstr(cmd, "RMOD")) return req->argc == 3 ? reply_integer(s, kvs_rbtree_mod(&global_rbtree, &key, &value) == 0 ? 1 : 0) : reply_error(s, "wrong argc for RMOD");
    if (view_eq_cstr(cmd, "REXIST")) return reply_integer(s, kvs_rbtree_exist(&global_rbtree, &key) == 0 ? 1 : 0);
#endif
#if ENABLE_HASH
    if (view_eq_cstr(cmd, "HSET")) return req->argc == 3 ? (kvs_hash_set(&global_hash, &key, &value) == 0 ? reply_simple(s, "OK") : reply_error(s, "hset failed")) : reply_error(s, "wrong argc for HSET");
    if (view_eq_cstr(cmd, "HGET")) { kvs_blob_t *v = kvs_hash_get(&global_hash, &key); return v ? reply_bulk(s, v) : reply_null(s); }
    if (view_eq_cstr(cmd, "HDEL")) return reply_integer(s, kvs_hash_del(&global_hash, &key) == 0 ? 1 : 0);
    if (view_eq_cstr(cmd, "HMOD")) return req->argc == 3 ? reply_integer(s, kvs_hash_mod(&global_hash, &key, &value) == 0 ? 1 : 0) : reply_error(s, "wrong argc for HMOD");
    if (view_eq_cstr(cmd, "HEXIST")) return reply_integer(s, kvs_hash_exist(&global_hash, &key) == 0 ? 1 : 0);
#endif
    return reply_error(s, "unknown command");
}

static void kvs_stream_resync_input(kvs_stream_t *s, size_t start) {
    size_t i;
    if (start >= s->in_len) {
        s->in_len = 0;
        s->parse_pos = 0;
        return;
    }
    for (i = start; i < s->in_len; ++i) {
        if (s->inbuf[i] == '*') {
            if (i > 0 && s->inbuf[i - 1] != '\n') continue;
            memmove(s->inbuf, s->inbuf + i, s->in_len - i);
            s->in_len -= i;
            s->parse_pos = 0;
            return;
        }
    }
    s->in_len = 0;
    s->parse_pos = 0;
}

static int kvs_stream_protocol_error(kvs_stream_t *s, const char *msg) {
    size_t resume_from = s->parse_pos + 1;
    if (reply_error(s, msg) != 0) return -1;
    kvs_stream_reset_request(s);
    kvs_stream_resync_input(s, resume_from);
    return 0;
}

int kvs_stream_feed(kvs_stream_t *s, const unsigned char *data, size_t len) {
    if (len > 0) {
        if (s->in_len + len > KVS_STREAM_IN_CAP) {
            return kvs_stream_protocol_error(s, "input buffer overflow");
        }
        memcpy(s->inbuf + s->in_len, data, len);
        s->in_len += len;
    }

    while (1) {
        if (s->state == KVS_RESP_STATE_ARRAY) {
            size_t consumed = 0;
            long long argc = 0;
            int pr;

            if (s->parse_pos >= s->in_len) break;
            if (s->inbuf[s->parse_pos] != '*') {
                if (kvs_stream_protocol_error(s, "expected array") != 0) return -1;
                continue;
            }
            pr = parse_crlf_number(s->inbuf + s->parse_pos + 1, s->in_len - s->parse_pos - 1, &consumed, &argc);
            if (pr == 0) break;
            if (pr < 0) {
                if (kvs_stream_protocol_error(s, "invalid array length") != 0) return -1;
                continue;
            }
            if (argc <= 0 || argc > KVS_MAX_ARGC) {
                if (kvs_stream_protocol_error(s, "invalid argc") != 0) return -1;
                continue;
            }
            s->parse_pos += 1 + consumed;
            s->argc_expected = (int)argc;
            s->argc_read = 0;
            s->req.argc = (int)argc;
            s->state = KVS_RESP_STATE_BULK_LEN;
        } else if (s->state == KVS_RESP_STATE_BULK_LEN) {
            size_t consumed = 0;
            long long bulk = 0;
            int pr;

            if (s->parse_pos >= s->in_len) break;
            if (s->inbuf[s->parse_pos] != '$') {
                if (kvs_stream_protocol_error(s, "expected bulk string") != 0) return -1;
                continue;
            }
            pr = parse_crlf_number(s->inbuf + s->parse_pos + 1, s->in_len - s->parse_pos - 1, &consumed, &bulk);
            if (pr == 0) break;
            if (pr < 0) {
                if (kvs_stream_protocol_error(s, "invalid bulk length") != 0) return -1;
                continue;
            }
            if (bulk < 0) {
                if (kvs_stream_protocol_error(s, "negative bulk not supported") != 0) return -1;
                continue;
            }
            s->parse_pos += 1 + consumed;
            s->bulk_len = (ssize_t)bulk;
            s->state = KVS_RESP_STATE_BULK_DATA;
        } else {
            if (s->in_len - s->parse_pos < (size_t)s->bulk_len + 2) break;
            if (kvs_blob_dup(&s->req.argv[s->argc_read], s->inbuf + s->parse_pos, (size_t)s->bulk_len) != 0) return -1;
            s->parse_pos += (size_t)s->bulk_len;
            if (s->inbuf[s->parse_pos] != '\r' || s->inbuf[s->parse_pos + 1] != '\n') {
                if (kvs_stream_protocol_error(s, "bulk missing CRLF") != 0) return -1;
                continue;
            }
            s->parse_pos += 2;
            s->argc_read++;
            if (s->argc_read == s->argc_expected) {
                if (dispatch_request(s, &s->req) != 0) return -1;
                kvs_stream_reset_request(s);
                kvs_stream_compact_input(s);
            } else {
                s->state = KVS_RESP_STATE_BULK_LEN;
            }
        }
    }

    if (s->parse_pos > 0 && s->parse_pos == s->in_len) {
        s->in_len = 0;
        s->parse_pos = 0;
    }
    return 0;
}

int kvs_stream_has_output(const kvs_stream_t *s) {
    return s->out_head != NULL;
}

const unsigned char *kvs_stream_output_ptr(const kvs_stream_t *s) {
    return s->out_head ? s->out_head->data + s->out_head->sent : NULL;
}

size_t kvs_stream_output_len(const kvs_stream_t *s) {
    return s->out_head ? (s->out_head->len - s->out_head->sent) : 0;
}

void kvs_stream_consume_output(kvs_stream_t *s, size_t n) {
    while (n > 0 && s->out_head) {
        kvs_out_node_t *node = s->out_head;
        size_t left = node->len - node->sent;
        if (n < left) {
            node->sent += n;
            s->out_queued_bytes -= n;
            return;
        }
        n -= left;
        s->out_queued_bytes -= left;
        s->out_head = node->next;
        if (!s->out_head) s->out_tail = NULL;
        free(node->data);
        free(node);
    }
}

int init_kvengine(void) {
#if ENABLE_ARRAY
    memset(&global_array, 0, sizeof(global_array));
    if (kvs_array_create(&global_array) != 0) return -1;
#endif
#if ENABLE_RBTREE
    memset(&global_rbtree, 0, sizeof(global_rbtree));
    if (kvs_rbtree_create(&global_rbtree) != 0) return -1;
#endif
#if ENABLE_HASH
    memset(&global_hash, 0, sizeof(global_hash));
    if (kvs_hash_create(&global_hash) != 0) return -1;
#endif
    return 0;
}

void dest_kvengine(void) {
#if ENABLE_ARRAY
    kvs_array_destory(&global_array);
#endif
#if ENABLE_RBTREE
    kvs_rbtree_destory(&global_rbtree);
#endif
#if ENABLE_HASH
    kvs_hash_destory(&global_hash);
#endif
}

int main(int argc, char *argv[]) {
    int port;
    if (argc != 2) return -1;
    port = atoi(argv[1]);
    if (init_kvengine() != 0) return -1;
#if (NETWORK_SELECT == NETWORK_REACTOR)
    reactor_start((unsigned short)port, NULL);
#elif (NETWORK_SELECT == NETWORK_PROACTOR)
    proactor_start((unsigned short)port, NULL);
#else
    ntyco_start((unsigned short)port, NULL);
#endif
    dest_kvengine();
    return 0;
}
