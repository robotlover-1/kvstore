
#include "kvstore/kvstore.h"
#include "kvstore/replication/repl_kprobe.h"
#include <signal.h>
#include <ctype.h>
#include <strings.h>

/* kvs_repl.c 中的 static 变量，KPROBEMR 响应需要 */
extern int g_slave_fd;

#define KVS_DEFAULT_CONFIG_PATH "kvstore.conf"

/* 仅当 RDMA 被实际配置为传输层时才打印 RDMA 调试日志 */
#if KVS_ENABLE_RDMA
#define repl_rdma_log(fmt, ...) do { \
    if (!strcasecmp(g_cfg.repl_fullsync_transport, "rdma") || \
        !strcasecmp(g_cfg.repl_transport_backend, "rdma")) { \
        fprintf(stderr, "repl rdma: " fmt "\n", ##__VA_ARGS__); \
    } \
} while(0)
#else
#define repl_rdma_log(fmt, ...) ((void)0)
#endif

kv_config_t g_cfg = {
    .role = ROLE_MASTER,
    .port = 5160,
    .master_host = "192.168.233.128",
    .master_port = 5160,
    .dump_path = "kvstore.dump",
    .aof_path = "kvstore.aof",
    .mem_backend = "libc",
    .net_backend = "reactor",
    .repl_transport_backend = "tcp",
    .repl_fullsync_transport = "rdma",
    .repl_realtime_transport = "tcp",
    .ebpf_obj_path = "build/replication/bpf/repl_sockmap.bpf.o",
    .ebpf_pin_path = "/sys/fs/bpf/kvstore_repl_sockmap",
    .ebpf_enabled = 0,
    .ebpf_redirect = 0,
    .ebpf_redirect_key = 0,
    .ebpf_forward = 0,
    .rdma_dev = "siw0",
    .rdma_ib_port = 1,
    .rdma_gid_idx = 1,
    .rdma_port = 0,
    .rdma_recv_slots = 64,
    .rdma_chunk_size = BUFFER_CAP * 4,
    .rdma_qp_wr_depth = 64,
    .aof_fsync = KVS_AOF_FSYNC_ALWAYS,
    .log_mode = "info",
    .is_sentinel = 0,
    .sentinel_master_name = "mymaster",
    .sentinel_monitor_host = "127.0.0.1",
    .sentinel_monitor_port = 5000,
    .sentinel_known_slaves = "",
    .sentinel_down_after_ms = 5000,
    .sentinel_failover_timeout_ms = 10000,
    .sentinel_quorum = 1,
    .kprobe_enabled = 1,
    .repl_kprobe_obj_path = "build/replication/bpf/repl_kprobe.bpf.o",
};
conn_t *g_replicas = NULL;
pthread_mutex_t g_repl_lock = PTHREAD_MUTEX_INITIALIZER;

/* 全量同步进行中标志 — 同步期间跳过实时广播，启停 RDMA */
volatile int g_repl_fullsync_in_progress = 0;

/* eBPF+tcp 新增量路径标志 — 启用时 client_capture 直接转发到 slave，repl_broadcast 跳过 */
volatile int g_repl_client_capture_active = 0;

/* Master 侧 slave 的 TCP 连接 fd（供 client_capture 转发引擎使用） */
int g_repl_capture_slave_fd = -1;

/* kprobe 转发接管增量同步时，压制 repl_broadcast */
volatile int g_repl_broadcast_suppressed = 0;

static pthread_mutex_t g_repl_last_send_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_repl_last_send_stage[64] = "none";
static unsigned long long g_repl_last_send_len = 0;
static unsigned long long g_repl_last_send_offset = 0;
static char g_repl_last_send_preview[160] = "";

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

static void repl_format_send_preview(char *out, size_t out_cap, const unsigned char *buf, size_t len) {
    size_t in_cap;
    size_t i;
    size_t pos = 0;
    if (!out || out_cap == 0) return;
    out[0] = '\0';
    if (!buf || len == 0) return;
    in_cap = len < 64 ? len : 64;
    for (i = 0; i < in_cap && pos + 2 < out_cap; ++i) {
        unsigned char ch = buf[i];
        if (ch == '\r') {
            if (pos + 2 >= out_cap) break;
            out[pos++] = '\\';
            out[pos++] = 'r';
        } else if (ch == '\n') {
            if (pos + 2 >= out_cap) break;
            out[pos++] = '\\';
            out[pos++] = 'n';
        } else if (isprint(ch)) {
            out[pos++] = (char)ch;
        } else {
            out[pos++] = '.';
        }
    }
    if (in_cap < len && pos + 4 < out_cap) {
        out[pos++] = '.';
        out[pos++] = '.';
        out[pos++] = '.';
    }
    out[pos] = '\0';
}

void repl_note_send_context(const char *stage, size_t len, unsigned long long offset, const unsigned char *buf) {
    pthread_mutex_lock(&g_repl_last_send_lock);
    snprintf(g_repl_last_send_stage, sizeof(g_repl_last_send_stage), "%s", (stage && *stage) ? stage : "unknown");
    g_repl_last_send_len = (unsigned long long)len;
    g_repl_last_send_offset = offset;
    repl_format_send_preview(g_repl_last_send_preview, sizeof(g_repl_last_send_preview), buf, len);
    pthread_mutex_unlock(&g_repl_last_send_lock);
}

void repl_get_last_send_context(char *stage, size_t stage_cap, unsigned long long *len, unsigned long long *offset, char *preview, size_t preview_cap) {
    pthread_mutex_lock(&g_repl_last_send_lock);
    if (stage && stage_cap > 0) snprintf(stage, stage_cap, "%s", g_repl_last_send_stage);
    if (len) *len = g_repl_last_send_len;
    if (offset) *offset = g_repl_last_send_offset;
    if (preview && preview_cap > 0) snprintf(preview, preview_cap, "%s", g_repl_last_send_preview);
    pthread_mutex_unlock(&g_repl_last_send_lock);
}

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

static int parse_repl_transport_backend(const char *s) {
    if (!s) return -1;
    if (!strcasecmp(s, "tcp")) return 0;
    if (!strcasecmp(s, "rdma")) return 0;
    if (!strcasecmp(s, "ebpf")) return 0;
    if (!strcasecmp(s, "sockmap")) return 0;
    if (!strcasecmp(s, "kprobe-rdma")) return 0;
    if (!strcasecmp(s, "ebpf+tcp")) return 0;
    return -1;
}

static int parse_args(int argc, char **argv) {
    const char *config_path = NULL;
    /* First pass: find config file path */
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            config_path = argv[i + 1];
            break;
        }
        if (i == 1 && argv[i][0] != '-') {
            /* First positional argument: treat as config file */
            config_path = argv[i];
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
        else if (!strcmp(argv[i], "--aof-disable")) persist_aof_disable();
        else if (!strcmp(argv[i], "--mem") && i + 1 < argc) snprintf(g_cfg.mem_backend, sizeof(g_cfg.mem_backend), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--net") && i + 1 < argc) {
            snprintf(g_cfg.net_backend, sizeof(g_cfg.net_backend), "%s", argv[++i]);
        }
        else if (!strcmp(argv[i], "--repl-transport") && i + 1 < argc) {
            if (parse_repl_transport_backend(argv[i + 1]) != 0) return -1;
            snprintf(g_cfg.repl_transport_backend, sizeof(g_cfg.repl_transport_backend), "%s", argv[++i]);
        }
        else if (!strcmp(argv[i], "--repl-fullsync-transport") && i + 1 < argc) {
            if (parse_repl_transport_backend(argv[i + 1]) != 0) return -1;
            snprintf(g_cfg.repl_fullsync_transport, sizeof(g_cfg.repl_fullsync_transport), "%s", argv[++i]);
        }
        else if (!strcmp(argv[i], "--repl-realtime-transport") && i + 1 < argc) {
            if (parse_repl_transport_backend(argv[i + 1]) != 0) return -1;
            snprintf(g_cfg.repl_realtime_transport, sizeof(g_cfg.repl_realtime_transport), "%s", argv[++i]);
        }
        else if (!strcmp(argv[i], "--ebpf-obj") && i + 1 < argc) {
            snprintf(g_cfg.ebpf_obj_path, sizeof(g_cfg.ebpf_obj_path), "%s", argv[++i]);
        }
        else if (!strcmp(argv[i], "--ebpf-pin") && i + 1 < argc) {
            snprintf(g_cfg.ebpf_pin_path, sizeof(g_cfg.ebpf_pin_path), "%s", argv[++i]);
        }
        else if (!strcmp(argv[i], "--ebpf-pin-path") && i + 1 < argc) {
            snprintf(g_cfg.ebpf_pin_path, sizeof(g_cfg.ebpf_pin_path), "%s", argv[++i]);
        }
        else if (!strcmp(argv[i], "--ebpf-redirect")) {
            g_cfg.ebpf_redirect = 1;
        }
        else if (!strcmp(argv[i], "--ebpf-redirect-key") && i + 1 < argc) {
            g_cfg.ebpf_redirect_key = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--ebpf-forward")) {
            g_cfg.ebpf_forward = 1;
        }
        else if (!strcmp(argv[i], "--rdma-dev") && i + 1 < argc) {
            snprintf(g_cfg.rdma_dev, sizeof(g_cfg.rdma_dev), "%s", argv[++i]);
        }
        else if (!strcmp(argv[i], "--rdma-ib-port") && i + 1 < argc) {
            g_cfg.rdma_ib_port = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--rdma-gid-idx") && i + 1 < argc) {
            g_cfg.rdma_gid_idx = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--rdma-port") && i + 1 < argc) {
            g_cfg.rdma_port = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--rdma-recv-slots") && i + 1 < argc) {
            g_cfg.rdma_recv_slots = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--rdma-chunk-size") && i + 1 < argc) {
            g_cfg.rdma_chunk_size = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--rdma-qp-wr-depth") && i + 1 < argc) {
            g_cfg.rdma_qp_wr_depth = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--appendfsync") && i + 1 < argc) {
            kvs_aof_fsync_policy_t policy;
            if (parse_appendfsync_policy(argv[++i], &policy) != 0) return -1;
            g_cfg.aof_fsync = policy;
        }
        else if (!strcmp(argv[i], "--log-mode") && i + 1 < argc) {
            snprintf(g_cfg.log_mode, sizeof(g_cfg.log_mode), "%s", argv[++i]);
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
        else if (!strcmp(argv[i], "--kprobe-enabled")) {
            g_cfg.kprobe_enabled = 1;
        }
        else if (!strcmp(argv[i], "--repl-kprobe-obj-path") && i + 1 < argc) {
            snprintf(g_cfg.repl_kprobe_obj_path, sizeof(g_cfg.repl_kprobe_obj_path), "%s", argv[++i]);
        }
        else if (argv[i][0] != '-') {
            /* Bare argument: treat as config file path */
            if (parse_config_file(argv[i]) != 0) return -1;
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
    else if (!strcmp(key, "repl_transport_backend")) {
        if (parse_repl_transport_backend(value) != 0) return -1;
        snprintf(g_cfg.repl_transport_backend, sizeof(g_cfg.repl_transport_backend), "%s", value);
    }
    else if (!strcmp(key, "repl_fullsync_transport")) {
        if (parse_repl_transport_backend(value) != 0) return -1;
        snprintf(g_cfg.repl_fullsync_transport, sizeof(g_cfg.repl_fullsync_transport), "%s", value);
    }
    else if (!strcmp(key, "repl_realtime_transport")) {
        if (parse_repl_transport_backend(value) != 0) return -1;
        snprintf(g_cfg.repl_realtime_transport, sizeof(g_cfg.repl_realtime_transport), "%s", value);
    }
    else if (!strcmp(key, "ebpf_enabled")) g_cfg.ebpf_enabled = (!strcasecmp(value, "1") || !strcasecmp(value, "true") || !strcasecmp(value, "yes"));
    else if (!strcmp(key, "ebpf_obj_path")) snprintf(g_cfg.ebpf_obj_path, sizeof(g_cfg.ebpf_obj_path), "%s", value);
    else if (!strcmp(key, "ebpf_pin_path")) snprintf(g_cfg.ebpf_pin_path, sizeof(g_cfg.ebpf_pin_path), "%s", value);
    else if (!strcmp(key, "ebpf_redirect")) g_cfg.ebpf_redirect = (!strcasecmp(value, "1") || !strcasecmp(value, "true") || !strcasecmp(value, "yes"));
    else if (!strcmp(key, "ebpf_redirect_key")) g_cfg.ebpf_redirect_key = atoi(value);
    else if (!strcmp(key, "ebpf_forward")) g_cfg.ebpf_forward = (!strcasecmp(value, "1") || !strcasecmp(value, "true") || !strcasecmp(value, "yes"));
    else if (!strcmp(key, "rdma_dev")) snprintf(g_cfg.rdma_dev, sizeof(g_cfg.rdma_dev), "%s", value);
    else if (!strcmp(key, "rdma_ib_port")) g_cfg.rdma_ib_port = atoi(value);
    else if (!strcmp(key, "rdma_gid_idx")) g_cfg.rdma_gid_idx = atoi(value);
    else if (!strcmp(key, "rdma_port")) g_cfg.rdma_port = atoi(value);
    else if (!strcmp(key, "rdma_recv_slots")) g_cfg.rdma_recv_slots = atoi(value);
    else if (!strcmp(key, "rdma_chunk_size")) g_cfg.rdma_chunk_size = atoi(value);
    else if (!strcmp(key, "rdma_qp_wr_depth")) g_cfg.rdma_qp_wr_depth = atoi(value);
    else if (!strcmp(key, "appendfsync")) {
        if (parse_appendfsync_policy(value, &policy) != 0) return -1;
        g_cfg.aof_fsync = policy;
    }
    else if (!strcmp(key, "log_mode")) snprintf(g_cfg.log_mode, sizeof(g_cfg.log_mode), "%s", value);
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
    else if (!strcmp(key, "kprobe_enabled")) g_cfg.kprobe_enabled = (!strcasecmp(value, "1") || !strcasecmp(value, "true") || !strcasecmp(value, "yes"));
    else if (!strcmp(key, "repl_kprobe_obj_path")) snprintf(g_cfg.repl_kprobe_obj_path, sizeof(g_cfg.repl_kprobe_obj_path), "%s", value);
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
    if (!c) return;  /* safety: slave 端 parse_resp_stream(NULL, ...) 可能误触发 */
    pthread_mutex_lock(&g_repl_lock);
    for (conn_t *it = g_replicas; it; it = it->next_replica) {
        if (it == c) {
            c->is_replica = 1;
            c->repl_draining = 0;
            c->repl_fullsync_pending = 0;
            pthread_mutex_unlock(&g_repl_lock);
            return;
        }
    }
    c->is_replica = 1;
    c->repl_draining = 0;
    c->repl_fullsync_pending = 0;
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
    c->next_replica = NULL;
    c->is_replica = 0;
    c->repl_draining = 0;
    c->repl_fullsync_pending = 0;
    pthread_mutex_unlock(&g_repl_lock);
}

void repl_broadcast(const unsigned char *raw, size_t rawlen) {
    /* kprobe 转发接管时跳过 repl_broadcast（保底路径静默） */
    if (g_repl_broadcast_suppressed) return;

    repl_note_send_context("broadcast", rawlen, repl_master_offset(), raw);
    repl_backlog_feed(raw, rawlen);
    repl_note_broadcast(rawlen);
    pthread_mutex_lock(&g_repl_lock);
    conn_t **pp = &g_replicas;
    while (*pp) {
        conn_t *c = *pp;
        if (c->repl_draining) {
            *pp = c->next_replica;
            c->next_replica = NULL;
            c->is_replica = 0;
            continue;
        }
        if (c->repl_fullsync_pending) {
            pp = &c->next_replica;
            continue;
        }
        /* 全量同步期间跳过实时广播（数据通过 backlog + eBPF 缓存处理） */
        if (g_repl_fullsync_in_progress) {
            pp = &c->next_replica;
            continue;
        }
        /* eBPF 转发路径同时运行（best effort），repl_broadcast 作为可靠保底 */
        if (repl_realtime_send(c, raw, rawlen) != 0) {
            if (repl_handle_replica_send_failure(c, pp)) continue;
            pp = &c->next_replica;
            continue;
        }
        c->repl_offset_sent = repl_master_offset();
        c->repl_last_send_ms = kvs_now_ms();
        pp = &c->next_replica;
    }
    pthread_mutex_unlock(&g_repl_lock);
}

int repl_send_chunked(conn_t *c, const unsigned char *buf, size_t len) {
    return repl_send_chunked_ctx(c, buf, len, KVS_REPL_SEND_FULLSYNC);
}

int repl_send_chunked_ctx(conn_t *c, const unsigned char *buf, size_t len, int send_ctx) {
    size_t off = 0;
    size_t chunk_cap = !strcasecmp(repl_fullsync_transport_name(), "rdma") ? (g_cfg.rdma_chunk_size > 0 ? (size_t)g_cfg.rdma_chunk_size : (BUFFER_CAP * 4)) : len;
    if (!buf || len == 0) return 0;
    repl_note_send_context("chunked", len, repl_master_offset(), buf);
    if (chunk_cap < 1024) chunk_cap = 1024;
    if (chunk_cap > BUFFER_CAP * 4) chunk_cap = BUFFER_CAP * 4;
    if (chunk_cap == 0) chunk_cap = len;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > chunk_cap) chunk = chunk_cap;
        if (send_ctx == KVS_REPL_SEND_REALTIME) {
            if (repl_realtime_send(c, buf + off, chunk) != 0) return -1;
        } else {
            if (repl_fullsync_send(c, buf + off, chunk) != 0) return -1;
        }
        off += chunk;
    }
    return 0;
}

static int queue_snapshot(conn_t *c) {
    char hdr[128];
    char tmp_path[512];
    FILE *fp;
    size_t buf_size = BUFFER_CAP;
    {
        size_t eff = (size_t)repl_rdma_effective_chunk_size();
        if (eff > buf_size) buf_size = eff;
    }
    unsigned char *buf = (unsigned char *)kvs_malloc(buf_size);
    if (!buf) return -1;
    size_t r;
    size_t total = 0;
    size_t total_bytes = 0;
    int ret = -1;
    int rdma_ok = 0;

    repl_rdma_log("queue_snapshot - begin replid=%s offset=%llu", repl_master_id(), repl_master_offset());
    /* 记录全量同步启动时的 offset，用于后续回放 gap */
    unsigned long long snap_base_offset = repl_master_offset();

    /* NEW: 进入全量同步模式 — 设置标志，通知 eBPF 开始缓存客户端数据 */
    g_repl_fullsync_in_progress = 1;
    repl_client_capture_set_fullsync(1);

    /* 尝试启动 RDMA */
    if (!strcasecmp(g_cfg.repl_fullsync_transport, "rdma")) {
        if (repl_rdma_start_fullsync(c) == 0) {
            rdma_ok = 1;
            repl_rdma_log("queue_snapshot - RDMA started for fullsync");
        } else {
            repl_rdma_log("queue_snapshot - RDMA start failed, falling back to TCP");
        }
    }

    /* Use a temporary path so we don't overwrite the binary dump file */
    snprintf(tmp_path, sizeof(tmp_path), "%s.fullsync.tmp.%ld", g_cfg.dump_path, (long)getpid());
    fp = fopen(tmp_path, "wb");
    if (!fp) goto out;
    if (kvs_snapshot_to_fp(fp) != 0) { fclose(fp); unlink(tmp_path); goto out; }
    fclose(fp);
    fp = fopen(tmp_path, "rb");
    if (!fp) { unlink(tmp_path); goto out; }
    while ((r = fread(buf, 1, buf_size, fp)) > 0) total_bytes += r;
    fclose(fp);
    fp = NULL;

    int n = snprintf(hdr, sizeof(hdr), "+FULLRESYNC %s %llu %zu\r\n", repl_master_id(), snap_base_offset, total_bytes);

    repl_note_send_context("fullsync-header", (size_t)n, snap_base_offset, (unsigned char *)hdr);
    if (repl_send_chunked(c, (unsigned char *)hdr, (size_t)n) != 0) {
        repl_rdma_log("queue_snapshot - header send failed");
        goto out;
    }
    fp = fopen(tmp_path, "rb");
    if (!fp) { unlink(tmp_path); goto out; }
    while ((r = fread(buf, 1, buf_size, fp)) > 0) {
        total += r;
        repl_note_send_context("fullsync-snapshot", r, repl_master_offset(), buf);
        if (repl_send_chunked(c, buf, r) != 0) {
            repl_rdma_log("queue_snapshot - snapshot chunk send failed total=%zu chunk=%zu", total, r);
            fclose(fp);
            unlink(tmp_path);
            goto out;
        }
    }
    fclose(fp);
    fp = NULL;
    unlink(tmp_path);
    repl_note_fullsync(total);
    c->repl_offset_sent = repl_master_offset();
    c->repl_last_send_ms = kvs_now_ms();
    {
        size_t done = resp_build_cmd1(buf, buf_size, "REPLDONE");
        repl_note_send_context("fullsync-done", done, repl_master_offset(), buf);
        if (repl_send_chunked(c, buf, done) != 0) {
            repl_rdma_log("queue_snapshot - REPLDONE send failed");
            goto out;
        }
        /* 通知 client_capture: REPLDONE 已发送（用户态路径，eBPF+tcp 不经过 kprobe） */
        repl_client_capture_note_repldone();
    }
    c->repl_fullsync_pending = 0;

    /* NEW: 全量同步数据发送完成 — 关闭 RDMA，清除标志 */
    g_repl_fullsync_in_progress = 0;
    repl_client_capture_set_fullsync(0);

    /* 启用 kprobe 异步转发探索模式（双路径并行，验证通过后压制 repl_broadcast） */
    extern volatile int g_repl_broadcast_suppressed;
    extern volatile time_t g_fwd_last_active;
    extern volatile int g_fwd_healthy;
    /* 建立 kprobe 转发独立连接（不与 repl_broadcast 共用 fd） */
    repl_kprobe_fwd_connect_from_replica(c, g_cfg.port);
    /* 不立即压制 repl_broadcast — 等健康检查确认 kprobe 转发正常后再压制 */
    g_repl_broadcast_suppressed = 0;
    g_fwd_last_active = time(NULL);
    g_fwd_healthy = 1;
    fprintf(stderr, "kprobe fwd: probe mode (dual-path until proven healthy)\n");

    if (rdma_ok) {
        repl_rdma_stop_fullsync();
        repl_rdma_log("queue_snapshot - RDMA stopped after fullsync");
    }

    /* NEW: Flush eBPF 缓存的客户端数据（通过 TCP 发送到 slave，RDMA 已关闭）
     * 返回 > 0 表示有缓存数据被刷新，此时跳过 backlog gap replay 避免重复 */
    int cache_flushed = repl_client_capture_flush_to_slave(c);

    /* 回放全量同步期间累积的 gap 数据（从快照基址到当前 offset）
     * 仅当 eBPF 缓存未启用/无数据时才回放 backlog，避免数据重复 */
    if (cache_flushed == 0 && repl_master_offset() > snap_base_offset) {
        unsigned long long gap = repl_master_offset() - snap_base_offset;
        repl_rdma_log("queue_snapshot - replaying gap offset=%llu bytes=%llu",
            snap_base_offset, gap);
        if (repl_backlog_write_range(c, snap_base_offset) != 0) {
            repl_rdma_log("queue_snapshot - gap replay FAILED");
        } else {
            repl_rdma_log("queue_snapshot - gap replay OK");
        }
    } else if (cache_flushed > 0) {
        repl_rdma_log("queue_snapshot - backlog gap skipped (cache flushed %d items)", cache_flushed);
    }

    ret = 0;
    repl_rdma_log("queue_snapshot - complete snapshot_bytes=%zu repl_offset=%llu", total, repl_master_offset());
out:
    if (fp) fclose(fp);
    kvs_free(buf);
    return ret;
}

static int is_readonly_slave_blocked(const char *cmd) {
    return strcmp(cmd, "GET") && strcmp(cmd, "MGET") && strcmp(cmd, "TTL") && strcmp(cmd, "EXIST") && strcmp(cmd, "OWNER")
        && strcmp(cmd, "RGET") && strcmp(cmd, "RMGET") && strcmp(cmd, "RTTL") && strcmp(cmd, "REXIST")
        && strcmp(cmd, "HGET") && strcmp(cmd, "HMGET") && strcmp(cmd, "HTTL") && strcmp(cmd, "HEXIST")
        && strcmp(cmd, "XGET") && strcmp(cmd, "XMGET") && strcmp(cmd, "XTTL") && strcmp(cmd, "XEXIST")
        && strcmp(cmd, "INFO") && strcmp(cmd, "MEMSTAT")
        && strcmp(cmd, "PING") && strcmp(cmd, "ECHO") && strcmp(cmd, "QUIT")
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

static void repl_log_applied_command(const char *cmd, int argc, char **argv, size_t rawlen) {
    const char *key = (argc >= 2 && argv && argv[1]) ? argv[1] : "";
    unsigned long long before = repl_slave_offset();
    unsigned long long after = before + (unsigned long long)rawlen;
    repl_rdma_log("slave_apply_cmd - cmd=%s key=%s rawlen=%zu offset_before=%llu offset_after=%llu",
        cmd ? cmd : "?", key, rawlen, before, after);
    (void)argc;
}

static int repl_is_soak_key(const char *key) {
    return key && strstr(key, "rdma:soak:key:") == key;
}

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

static void repl_collect_replica_ack_stats(unsigned long long *max_applied, unsigned long long *max_durable, long long *min_ack_age_ms, int *replicas) {
    unsigned long long applied = 0;
    unsigned long long durable = 0;
    long long min_age = -1;
    int count = 0;
    pthread_mutex_lock(&g_repl_lock);
    for (conn_t *rc = g_replicas; rc; rc = rc->next_replica) {
        long long ack_age = (rc->repl_last_ack_ms > 0) ? (kvs_now_ms() - rc->repl_last_ack_ms) : -1;
        count++;
        if (rc->repl_applied_offset_ack > applied) applied = rc->repl_applied_offset_ack;
        if (rc->repl_durable_offset_ack > durable) durable = rc->repl_durable_offset_ack;
        if (ack_age >= 0 && (min_age < 0 || ack_age < min_age)) min_age = ack_age;
    }
    pthread_mutex_unlock(&g_repl_lock);
    if (max_applied) *max_applied = applied;
    if (max_durable) *max_durable = durable;
    if (min_ack_age_ms) *min_ack_age_ms = min_age;
    if (replicas) *replicas = count;
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
    char *resp = (char *)kvs_malloc(4096);  /* 4KB covers 99.9% responses, 16x smaller than 64KB */
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
    if (!strcmp(cmd, "REPLSYNC")) {
        /* slave 端通过 parse_resp_stream(NULL, ..., 1) 处理复制数据时，
         * 不应处理 REPLSYNC（缓存回放可能包含被 BPF 误捕获的 REPLSYNC） */
        if (from_replication && !c) goto out;
        const char *req_replid = argc >= 2 ? argv[1] : "?";
        unsigned long long req_offset = argc >= 3 ? (unsigned long long)strtoull(argv[2], NULL, 10) : 0;
        unsigned long long req_durable = argc >= 4 ? (unsigned long long)strtoull(argv[3], NULL, 10) : req_offset;
        /* Use req_offset for backlog check (data the slave needs).
         * req_durable may lag behind req_offset due to lazy AOF fsync,
         * but that doesn't prevent partial resync since the master
         * can replay from req_offset onward. */
        int can_continue = (argc >= 3 && repl_backlog_can_continue(req_replid, req_offset));
        repl_rdma_log("master_replsync - req_replid=%s req_offset=%llu req_durable=%llu backlog_start=%llu backlog_end=%llu can_continue=%d",
            req_replid, req_offset, req_durable, repl_backlog_start_offset(), repl_backlog_end_offset(), can_continue ? 1 : 0);
        if (c && (!strcasecmp(g_cfg.repl_realtime_transport, "ebpf") || !strcasecmp(g_cfg.repl_realtime_transport, "sockmap")
                || !strcasecmp(g_cfg.repl_transport_backend, "ebpf") || !strcasecmp(g_cfg.repl_transport_backend, "sockmap"))) {
            c->repl_transport_kind = KVS_REPL_TRANSPORT_EBPF;
            if (repl_ebpf_register_fd(c->fd, 1) != 0) {
                fprintf(stderr, "repl ebpf: fd registration failed on master replica link, using tcp-compatible path\n");
            }
        }
        if (c && (!strcasecmp(g_cfg.repl_realtime_transport, "kprobe-rdma")
                || !strcasecmp(g_cfg.repl_transport_backend, "kprobe-rdma"))) {
            c->repl_transport_kind = KVS_REPL_TRANSPORT_KPROBE_RDMA;
            /* kprobe 透明拦截，fd 注册在主进程初始化时已完成 */
            fprintf(stderr, "repl: replica transport set to kprobe-rdma\n");
        }
        if (c && (!strcasecmp(g_cfg.repl_realtime_transport, "ebpf+tcp")
                || !strcasecmp(g_cfg.repl_realtime_transport, "tcp"))) {
            c->repl_transport_kind = KVS_REPL_TRANSPORT_EBPF_TCP;
            fprintf(stderr, "repl: replica transport set to ebpf+tcp\n");
        }
        repl_add_slave(c);
        repl_replica_update_ack(c, req_offset, req_durable);
        c->repl_fullsync_pending = can_continue ? 0 : 1;

        /* eBPF+tcp 路径: 记录 slave fd 供转发引擎使用 */
        if (g_repl_client_capture_active) {
            g_repl_capture_slave_fd = c->fd;
            fprintf(stderr, "client_capture: capture slave fd=%d\n", c->fd);
        }
        if (can_continue) {
            repl_note_partialsync_result(1);
            repl_backlog_send_continue(c, req_offset);
        } else {
            repl_note_partialsync_result(argc >= 3 ? 0 : 1);
            if (queue_snapshot(c) != 0) {
                repl_rdma_log("master_replsync - queue_snapshot failed");
                c->repl_draining = 1;
                c->repl_fullsync_pending = 0;
            }
        }
        return 0;
    }
    if (!strcmp(cmd, "REPLACK")) {
        /* slave 端通过 parse_resp_stream(NULL, ..., 1) 处理复制数据时不应处理 REPLACK */
        if (from_replication && !c) return 0;
        unsigned long long applied_offset = argc >= 2 ? (unsigned long long)strtoull(argv[1], NULL, 10) : 0;
        unsigned long long durable_offset = argc >= 3 ? (unsigned long long)strtoull(argv[2], NULL, 10) : applied_offset;
        repl_replica_update_ack(c, applied_offset, durable_offset);
        return 0;
    }
    if (!strcmp(cmd, "REPLDONE")) {
        repl_rdma_log("slave_parse - REPLDONE");
        repl_slave_finish_fullsync();
        return 0;
    }
    if (!strcmp(cmd, "KPROBEMR")) {
        /* Slave 侧收到 KPROBEMR 请求：返回 MR 信息 */
        fprintf(stderr, "kprobe rdma: KPROBEMR received, sending MR info...\n");
        char resp[384];
        int rn = repl_kprobe_rdma_get_mr_text(resp, sizeof(resp));
        if (rn > 0) {
            if (c) {
                queue_bytes(c, (unsigned char *)resp, (size_t)rn);
            } else if (g_slave_fd >= 0) {
                send(g_slave_fd, resp, (size_t)rn, MSG_NOSIGNAL);
            }
            fprintf(stderr, "kprobe rdma: KPROBEMR response sent (%d bytes): %s",
                rn, resp);
        }
        return 0;
    }
    if (!strcmp(cmd, "INFO")) {
        char info[12288];
        char recover[1024] = {0};
        kvs_repl_ebpf_stats_t ebpf_stats;
        kvs_repl_kprobe_stats_t kprobe_stats;
        unsigned long long cc_hits = 0, cc_cached = 0;
        int cc_active = 0, cc_repld = 0, cc_l1 = 0, cc_l2 = 0;
        int recover_n = persist_build_recover_text(recover, sizeof(recover));
        repl_ebpf_get_stats(&ebpf_stats);
        repl_kprobe_rdma_get_stats(&kprobe_stats);
        cc_active = repl_client_capture_get_stats(
            &cc_hits, &cc_cached, &cc_repld, &cc_l1, &cc_l2);

        unsigned long long max_replica_applied = 0;
        unsigned long long max_replica_durable = 0;
        long long min_replica_ack_age_ms = -1;
        int replicas = 0;
        repl_collect_replica_ack_stats(&max_replica_applied, &max_replica_durable, &min_replica_ack_age_ms, &replicas);

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
            "repl_transport:%s\n"
            "repl_transport_configured:%s\n"
            "repl_transport_active:%s\n"
            "repl_transport_fallback_reason:%s\n"
            "repl_transport_fallback_count:%llu\n"
            "repl_transport_fallback_until_ms:%lld\n"
            "replicas:%d\n"
            "master_replid:%s\n"
            "master_repl_offset:%llu\n"
            "connected_slaves:%llu\n"
            "repl_fullsync_count:%llu\n"
            "repl_partialsync_ok_count:%llu\n"
            "repl_partialsync_err_count:%llu\n"
            "repl_broadcast_bytes:%llu\n"
            "repl_snapshot_bytes:%llu\n"
            "repl_backlog_size:%llu\n"
            "repl_backlog_histlen:%llu\n"
            "repl_backlog_start_offset:%llu\n"
            "repl_backlog_end_offset:%llu\n"
            "slave_master_replid:%s\n"
            "slave_repl_offset:%llu\n"
            "slave_repl_applied_offset:%llu\n"
            "slave_repl_durable_offset:%llu\n"
            "slave_fullsync_loading:%d\n"
            "replica_max_applied_offset_ack:%llu\n"
            "replica_max_durable_offset_ack:%llu\n"
            "replica_min_ack_age_ms:%lld\n"
            "rdma_recv_slots:%d\n"
            "rdma_chunk_size:%d\n"
            "rdma_qp_wr_depth:%d\n"
            "rdma_connected:%d\n"
            "rdma_disconnect_count:%llu\n"
            "rdma_reject_count:%llu\n"
            "rdma_send_cq_error_count:%llu\n"
            "rdma_recv_cq_error_count:%llu\n"
            "ebpf_compiled:%llu\n"
            "ebpf_initialized:%llu\n"
            "ebpf_register_attempts:%llu\n"
            "ebpf_register_failures:%llu\n"
            "ebpf_last_errno:%d\n"
            "ebpf_last_error:%s\n"
            "ebpf_sk_msg_count:%llu\n"
            "ebpf_sk_msg_bytes:%llu\n"
            "ebpf_sk_msg_pass:%llu\n"
            "ebpf_sk_msg_drop:%llu\n"
            "ebpf_redirect_enabled:%llu\n"
            "ebpf_forward_enabled:%llu\n"
            "ebpf_redirect_attempts:%llu\n"
            "ebpf_redirect_success:%llu\n"
            "ebpf_redirect_failures:%llu\n"
            "ebpf_role_unknown:%llu\n"
            "ebpf_role_master:%llu\n"
            "ebpf_role_slave:%llu\n"
            "kprobe_initialized:%d\n"
            "kprobe_rdma_connected:%d\n"
            "kprobe_hits:%llu\n"
            "kprobe_bytes:%llu\n"
            "kprobe_ringbuf_events:%llu\n"
            "kprobe_ringbuf_bytes:%llu\n"
            "kprobe_rdma_writes:%llu\n"
            "kprobe_rdma_errors:%llu\n"
            "client_capture_active:%d\n"
            "client_capture_hits:%llu\n"
            "client_capture_cached:%llu\n"
            "client_capture_repldone_detect:%d\n"
            "client_cache_l1_flushed:%d\n"
            "client_cache_l2_flushed:%d\n"
            "%s",
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
            repl_transport_name(),
            repl_transport_configured_name(),
            repl_transport_active_name(),
            repl_transport_fallback_reason()[0] ? repl_transport_fallback_reason() : "none",
            repl_transport_fallback_count(),
            repl_transport_fallback_until_ms(),
            replicas,
            repl_master_id(),
            repl_master_offset(),
            repl_connected_slaves(),
            repl_fullsync_count(),
            repl_partialsync_ok_count(),
            repl_partialsync_err_count(),
            repl_broadcast_bytes(),
            repl_snapshot_bytes(),
            repl_backlog_size(),
            repl_backlog_histlen(),
            repl_backlog_start_offset(),
            repl_backlog_end_offset(),
            repl_slave_master_id(),
            repl_slave_offset(),
            repl_slave_applied_offset(),
            repl_slave_durable_offset(),
            repl_slave_loading_fullsync(),
            max_replica_applied,
            max_replica_durable,
            min_replica_ack_age_ms,
            repl_rdma_effective_recv_slots(),
            repl_rdma_effective_chunk_size(),
            repl_rdma_effective_qp_wr_depth(),
            repl_rdma_is_connected(),
            repl_rdma_disconnect_count(),
            repl_rdma_reject_count(),
            repl_rdma_send_cq_error_count(),
            repl_rdma_recv_cq_error_count(),
            ebpf_stats.compiled,
            ebpf_stats.initialized,
            ebpf_stats.register_attempts,
            ebpf_stats.register_failures,
            ebpf_stats.last_errno,
            ebpf_stats.last_error,
            ebpf_stats.sk_msg_count,
            ebpf_stats.sk_msg_bytes,
            ebpf_stats.sk_msg_pass,
            ebpf_stats.sk_msg_drop,
            ebpf_stats.redirect_enabled,
            ebpf_stats.forward_enabled,
            ebpf_stats.redirect_attempts,
            ebpf_stats.redirect_success,
            ebpf_stats.redirect_failures,
            ebpf_stats.role_unknown,
            ebpf_stats.role_master,
            ebpf_stats.role_slave,
            kprobe_stats.kprobe_initialized,
            kprobe_stats.rdma_connected,
            kprobe_stats.kprobe_hits,
            kprobe_stats.kprobe_bytes,
            kprobe_stats.total_events,
            kprobe_stats.total_bytes,
            kprobe_stats.rdma_writes,
            kprobe_stats.rdma_errors,
            cc_active, cc_hits, cc_cached,
            cc_repld, cc_l1, cc_l2,
            recover_n >= 0 ? recover : "");

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
                if (persist_append_raw(raw, rawlen) != 0) {
                    n = resp_error(resp, BUFFER_CAP, "AOF write failed");
                    if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
                    goto out;
                }
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
                if (persist_append_raw(raw, rawlen) != 0) {
                    n = resp_error(resp, BUFFER_CAP, "AOF write failed");
                    if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
                    goto out;
                }
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
                if (persist_append_raw(raw, rawlen) != 0) {
                    n = resp_error(resp, BUFFER_CAP, "AOF write failed");
                    if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
                    goto out;
                }
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
        unsigned long long role_max_replica_applied = 0;
        unsigned long long role_max_replica_durable = 0;
        long long role_min_replica_ack_age_ms = -1;
        int role_replicas = 0;
        repl_collect_replica_ack_stats(&role_max_replica_applied, &role_max_replica_durable, &role_min_replica_ack_age_ms, &role_replicas);
        if (g_cfg.role == ROLE_MASTER) {
            char durable[32];
            snprintf(durable, sizeof(durable), "%llu", role_max_replica_durable);
            n = snprintf(resp, BUFFER_CAP,
                "*3\r\n$6\r\nmaster\r\n$1\r\n0\r\n$%zu\r\n%s\r\n",
                strlen(durable), durable);
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
                if (persist_append_raw(raw, rawlen) != 0) {
                    n = resp_error(resp, BUFFER_CAP, "AOF write failed");
                    if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
                    goto out;
                }
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
                if (persist_append_raw(raw, rawlen) != 0) {
                    n = resp_error(resp, BUFFER_CAP, "AOF write failed");
                    if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
                    goto out;
                }
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
                if (persist_append_raw(raw, rawlen) != 0) {
                    n = resp_error(resp, BUFFER_CAP, "AOF write failed");
                    if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
                    goto out;
                }
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
    int should_reply_missing = 0;

    if (!strcmp(op, "SET") && argc == 3) {
        try_expire(engine, argv[1]);
        rc = engine_set(engine, argv[1], argv[2]);
        should_reply_missing = 1;
    }
    else if (!strcmp(op, "MSET")) {
        try_expire(engine, argv[1]);
        rc = handle_multi_set(engine, argc, argv);
    }
    else if (!strcmp(op, "MOD") && argc == 3) {
        try_expire(engine, argv[1]);
        rc = engine_mod(engine, argv[1], argv[2]);
        should_reply_missing = 1;
    }
    else if (!strcmp(op, "DEL") && argc == 2) {
        try_expire(engine, argv[1]);
        rc = engine_del(engine, argv[1]);
        kvs_expire_del(&global_expire, engine, argv[1]);
        should_reply_missing = 1;
    }
    else if (!strcmp(op, "GET") && argc == 2) {
        try_expire(engine, argv[1]);
        char *v = engine_get(engine, argv[1]);
#if KVS_ENABLE_RDMA
        if (g_cfg.role == ROLE_SLAVE && !from_replication && repl_is_soak_key(argv[1])) {
            repl_rdma_log("slave_get_inline - key=%s hit=%d value=%s slave_offset=%llu master_link=%s",
                argv[1], v ? 1 : 0, v ? v : "(null)", repl_slave_offset(), repl_master_link_state_name());
        }
#endif
        n = v ? resp_bulk(resp, BUFFER_CAP, v, strlen(v)) : resp_null_bulk(resp, BUFFER_CAP);
        if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
        goto out;
        return 0;
    } else if (!strcmp(op, "MGET")) {
        n = handle_multi_get(engine, argc, argv, resp, BUFFER_CAP);
#if KVS_ENABLE_RDMA
        if (g_cfg.role == ROLE_SLAVE && !from_replication) {
            for (int i = 1; i < argc; ++i) {
                if (repl_is_soak_key(argv[i])) {
                    char *mv = engine_get(engine, argv[i]);
                    repl_rdma_log("slave_mget_inline - key=%s hit=%d value=%s slave_offset=%llu master_link=%s",
                        argv[i], mv ? 1 : 0, mv ? mv : "(null)", repl_slave_offset(), repl_master_link_state_name());
                }
            }
        }
#endif
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
        if (!strcmp(op, "SET") || !strcmp(op, "MOD")) {
            kvs_expire_persist(&global_expire, engine, argv[1]);
        }
        if (from_replication && is_write_cmd(cmd)) {
            if (!persist_recover_in_progress()) {
                repl_log_applied_command(cmd, argc, argv, rawlen);
            }
            if (g_cfg.role == ROLE_SLAVE && !persist_recover_in_progress()
                && !repl_slave_loading_fullsync()) {
                /* 全量同步期间不写 AOF（数据保存在 dump 文件）
                 * 增量同步期间写入 AOF 用于持久化 */
                persist_note_write();
                if (persist_append_raw(raw, rawlen) == 0) {
                    repl_slave_note_durable(rawlen);
                }
                /* else: AOF write failed — durability not achieved,
                 * slave will need full resync on reconnect */
            }
            if (!persist_recover_in_progress()) {
                repl_slave_note_applied(rawlen);
            }
        }
        if (!from_replication && is_write_cmd(cmd)) {
            persist_note_write();
            if (persist_append_raw(raw, rawlen) != 0) {
                n = resp_error(resp, BUFFER_CAP, "AOF write failed");
                if (c) queue_bytes(c, (unsigned char *)resp, (size_t)n);
                goto out;
            }
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
    /* KVSD fullsync interception: during full sync, write raw KVSD bytes
     * to temp file instead of RESP parsing. This is the single choke point
     * for ALL receive paths (TCP, RDMA, kprobe). */
    extern int g_slave_fullsync_tmp_fd;
    extern int g_slave_loading_fullsync;
    extern unsigned long long g_slave_fullsync_target_bytes;
    extern unsigned long long g_slave_fullsync_loaded_bytes;

    if (from_replication && g_slave_loading_fullsync) {
        size_t remaining = g_slave_fullsync_target_bytes - g_slave_fullsync_loaded_bytes;
        size_t to_write = (*len < remaining) ? *len : remaining;

        if (g_slave_fullsync_tmp_fd >= 0 && to_write > 0) {
            ssize_t wr = write(g_slave_fullsync_tmp_fd, buf, to_write);
            if (wr < 0) {
                *len = 0;
                return -1;
            }
        }
        g_slave_fullsync_loaded_bytes += to_write;

        if (to_write < *len) {
            /* trailing bytes (e.g. REPLDONE) — keep in buf for normal parsing */
            memmove(buf, buf + to_write, *len - to_write);
            *len -= to_write;
            if (g_slave_fullsync_target_bytes > 0 &&
                g_slave_fullsync_loaded_bytes >= g_slave_fullsync_target_bytes) {
                repl_slave_finish_fullsync();
            }
            return 0;
        }

        *len = 0;
        if (g_slave_fullsync_target_bytes > 0 &&
            g_slave_fullsync_loaded_bytes >= g_slave_fullsync_target_bytes) {
            repl_slave_finish_fullsync();
        }
        return 0;
    }

#define PARSE_SCRATCH 4096
    char scratch[PARSE_SCRATCH];
    size_t scratch_off = 0;
    size_t pos = 0;
    while (pos < *len) {
        if (buf[pos] == '+') {
            size_t line_start = pos + 1;
            while (pos + 1 < *len && !(buf[pos] == '\r' && buf[pos + 1] == '\n')) pos++;
            if (pos + 1 >= *len) break;
            if (pos > line_start) {
                size_t line_len = pos - line_start;
                char *line = (char *)kvs_malloc(line_len + 1);
                if (line) {
                    memcpy(line, buf + line_start, line_len);
                    line[line_len] = '\0';
                    char *argv[8] = {0};
                    int argc = split_inline_argv(line, argv, 8);
                    /* +KPROBERDMA 在 master 侧也需要处理 */
                    if (argc >= 6 && !strcmp(argv[0], "KPROBERDMA")) {
                        unsigned long rkey = (unsigned long)strtoull(argv[1], NULL, 10);
                        unsigned long addr = (unsigned long)strtoull(argv[2], NULL, 10);
                        fprintf(stderr, "kprobe rdma: +KPROBERDMA received (role=%s) rkey=%lu addr=0x%lx\n",
                            g_cfg.role == ROLE_MASTER ? "master" : "slave", rkey, addr);
                        if (g_cfg.role == ROLE_MASTER) {
                            repl_kprobe_rdma_parse_mr_info_direct(rkey, addr,
                                (size_t)strtoull(argv[3], NULL, 10),
                                (size_t)strtoull(argv[4], NULL, 10),
                                (size_t)strtoull(argv[5], NULL, 10));
                        }
                    } else if (from_replication && argc >= 3 && !strcmp(argv[0], "FULLRESYNC")) {
                        unsigned long long fullsync_target = argc >= 4 ? (unsigned long long)strtoull(argv[3], NULL, 10) : 0;
                        repl_slave_set_sync_state(argv[1], (unsigned long long)strtoull(argv[2], NULL, 10), (unsigned long long)strtoull(argv[2], NULL, 10), 1, fullsync_target);
                        repl_rdma_log("slave_parse - FULLRESYNC replid=%s offset=%s target=%s", argv[1], argv[2], argc >= 4 ? argv[3] : "0");
                    } else if (from_replication && argc >= 3 && !strcmp(argv[0], "CONTINUE")) {
                        unsigned long long continue_end = (unsigned long long)strtoull(argv[2], NULL, 10);
                        unsigned long long continue_start = repl_slave_offset();
                        unsigned long long durable_start = repl_slave_durable_offset();
                        if (continue_end < continue_start) continue_end = continue_start;
                        repl_slave_set_sync_state(argv[1], continue_start, durable_start, 0, 0);
                        repl_slave_send_ack();
                        repl_rdma_log("slave_parse - CONTINUE replid=%s start_offset=%llu durable_offset=%llu end_offset=%llu", argv[1], continue_start, durable_start, continue_end);
                    }
                    kvs_free(line);
                }
            }
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
        if (p - (pos + 1) >= 32) { pos = p + 2; continue; }
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
            if (lp - (p + 1) >= 32) { malformed = 1; break; }
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
            /* try scratch buffer first (amortizes malloc across pipeline batch) */
            if (scratch_off + (size_t)blen + 1 <= PARSE_SCRATCH) {
                argv[i] = scratch + scratch_off;
                scratch_off += (size_t)blen + 1;
            } else {
                argv[i] = (char *)kvs_malloc((size_t)blen + 1);
                if (!argv[i]) { malformed = 1; break; }
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
            for (int i = 0; i < argc; ++i)
                if (argv[i] < scratch || argv[i] >= scratch + PARSE_SCRATCH) kvs_free(argv[i]);
            break;
        }
        if (malformed) {
            for (int i = 0; i < argc; ++i)
                if (argv[i] < scratch || argv[i] >= scratch + PARSE_SCRATCH) kvs_free(argv[i]);
            scratch_off = 0;
            if (p > start) pos = p;
            else break;
            continue;
        }
        handle_parsed_command(c, argc, argv, argl, buf + start, p - start, from_replication);
        for (int i = 0; i < argc; ++i)
            if (argv[i] < scratch || argv[i] >= scratch + PARSE_SCRATCH) kvs_free(argv[i]);
        scratch_off = 0;
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

typedef int (*snapshot_emit_fn)(void *ctx, const unsigned char *buf, size_t len);

typedef struct snapshot_sink_s {
    snapshot_emit_fn emit;
    void *ctx;
} snapshot_sink_t;

static int emit_cmd3_sink(snapshot_sink_t *sink, const char *cmd, const char *a1, const char *a2) {
    unsigned char buf[BUFFER_CAP];
    size_t n = resp_build_cmd3(buf, sizeof(buf), cmd, a1, a2);
    return sink->emit(sink->ctx, buf, n);
}

static int emit_cmd4_sink(snapshot_sink_t *sink, const char *cmd, const char *a1, const char *a2, const char *a3) {
    unsigned char buf[BUFFER_CAP];
    size_t n = resp_build_cmd4(buf, sizeof(buf), cmd, a1, a2, a3);
    return sink->emit(sink->ctx, buf, n);
}

static int snapshot_emit_fp(void *ctx, const unsigned char *buf, size_t len) {
    FILE *fp = (FILE *)ctx;
    return fwrite(buf, 1, len, fp) == len ? 0 : -1;
}

static int snapshot_emit_fd(void *ctx, const unsigned char *buf, size_t len) {
    long long *state = (long long *)ctx;
    int fd = (int)state[0];
    long long off = state[1];
    if (persist_write_raw_fd(fd, buf, len, &off) != 0) return -1;
    state[1] = off;
    return 0;
}

static int maybe_emit_expire_sink(snapshot_sink_t *sink, int engine, const char *key) {
    long long ttl = kvs_expire_ttl(&global_expire, engine, key);
    if (ttl < 0) return 0;
    char sec[32]; snprintf(sec, sizeof(sec), "%lld", ttl);
    const char *cmd =
        engine == KVS_ENGINE_ARRAY ? "EXPIRE" :
        engine == KVS_ENGINE_RBTREE ? "REXPIRE" :
        engine == KVS_ENGINE_HASH ? "HEXPIRE" : "XEXPIRE";
    return emit_cmd3_sink(sink, cmd, key, sec);
}

static int snapshot_array_sink(snapshot_sink_t *sink) {
    for (int i = 0; i < KVS_ARRAY_SIZE; ++i) {
        if (global_array.table && global_array.table[i].key) {
            if (emit_cmd3_sink(sink, "SET", global_array.table[i].key, global_array.table[i].value) != 0) return -1;
            if (maybe_emit_expire_sink(sink, KVS_ENGINE_ARRAY, global_array.table[i].key) != 0) return -1;
        }
    }
    return 0;
}

static int snapshot_hash_sink(snapshot_sink_t *sink) {
    for (int t = 0; t < 2; t++) {
        if (!global_hash.ht[t].nodes) continue;
        for (int i = 0; i < global_hash.ht[t].max_slots; ++i) {
            for (hashnode_t *node = global_hash.ht[t].nodes[i]; node; node = node->next) {
                if (emit_cmd3_sink(sink, "HSET", node->key, node->value) != 0) return -1;
                if (maybe_emit_expire_sink(sink, KVS_ENGINE_HASH, node->key) != 0) return -1;
            }
        }
    }
    return 0;
}

static int snapshot_rbtree_node_sink(snapshot_sink_t *sink, rbtree_node *node, rbtree_node *nil) {
    if (node == nil) return 0;
    if (snapshot_rbtree_node_sink(sink, node->left, nil) != 0) return -1;
    if (emit_cmd3_sink(sink, "RSET", node->key, (char *)node->value) != 0) return -1;
    if (maybe_emit_expire_sink(sink, KVS_ENGINE_RBTREE, node->key) != 0) return -1;
    if (snapshot_rbtree_node_sink(sink, node->right, nil) != 0) return -1;
    return 0;
}

static int snapshot_skiptable_cb_sink(const char *key, const char *value, void *arg) {
    snapshot_sink_t *sink = (snapshot_sink_t *)arg;
    if (emit_cmd3_sink(sink, "XSET", key, value) != 0) return -1;
    if (maybe_emit_expire_sink(sink, KVS_ENGINE_SKIPTABLE, key) != 0) return -1;
    return 0;
}

static int snapshot_skiptable_sink(snapshot_sink_t *sink) {
    return kvs_skiptable_foreach(&global_skiptable, snapshot_skiptable_cb_sink, sink);
}

static int snapshot_doc_field_cb_sink(const char *name, const char *value, void *arg) {
    void **ctx = (void **)arg;
    snapshot_sink_t *sink = (snapshot_sink_t *)ctx[0];
    const char *key = (const char *)ctx[1];
    return emit_cmd4_sink(sink, "DOCSET", key, name, value);
}

static int snapshot_doc_cb_sink(const char *key, kvs_doc_t *doc, void *arg) {
    snapshot_sink_t *sink = (snapshot_sink_t *)arg;
    (void)doc;
    void *ctx[2] = { sink, (void *)key };
    return kvs_doc_foreach_field(&global_doc, key, snapshot_doc_field_cb_sink, ctx);
}

static int snapshot_doc_sink(snapshot_sink_t *sink) {
    return kvs_doc_foreach(&global_doc, snapshot_doc_cb_sink, sink);
}

static int snapshot_all_sink(snapshot_sink_t *sink) {
    if (!sink || !sink->emit) return -1;
    if (snapshot_array_sink(sink) != 0) return -1;
    if (snapshot_rbtree_node_sink(sink, global_rbtree.root, global_rbtree.nil) != 0) return -1;
    if (snapshot_hash_sink(sink) != 0) return -1;
    if (snapshot_skiptable_sink(sink) != 0) return -1;
    if (snapshot_doc_sink(sink) != 0) return -1;
    return 0;
}

int kvs_snapshot_to_fp(FILE *fp) {
    snapshot_sink_t sink;
    if (!fp) return -1;
    sink.emit = snapshot_emit_fp;
    sink.ctx = fp;
    return snapshot_all_sink(&sink);
}

int kvs_snapshot_to_fd(int fd) {
    snapshot_sink_t sink;
    long long fd_state[2];
    if (fd < 0) return -1;
    fd_state[0] = fd;
    fd_state[1] = 0;
    sink.emit = snapshot_emit_fd;
    sink.ctx = fd_state;
    return snapshot_all_sink(&sink);
}

static int dump_skiptable_write_kv(const char *key, const char *value, void *arg) {
    int fd = *(int *)arg;
    uint8_t eng = (uint8_t)KVS_ENGINE_SKIPTABLE;
    uint8_t flags = 0;
    uint32_t klen = (uint32_t)strlen(key);
    uint32_t vlen = (uint32_t)strlen(value);
    long long ttl_sec = kvs_expire_ttl(&global_expire, KVS_ENGINE_SKIPTABLE, key);
    uint64_t exp = 0;
    if (ttl_sec >= 0) { flags = KVSD_FLAG_HAS_EXPIRE; exp = (uint64_t)(kvs_now_ms() + ttl_sec * 1000); }
    if (write(fd, &eng, sizeof(eng)) != sizeof(eng)) return -1;
    if (write(fd, &flags, sizeof(flags)) != sizeof(flags)) return -1;
    if (write(fd, &klen, sizeof(klen)) != sizeof(klen)) return -1;
    if (write(fd, key, klen) != (ssize_t)klen) return -1;
    if (write(fd, &vlen, sizeof(vlen)) != sizeof(vlen)) return -1;
    if (write(fd, value, vlen) != (ssize_t)vlen) return -1;
    if (flags & KVSD_FLAG_HAS_EXPIRE) {
        if (write(fd, &exp, sizeof(exp)) != sizeof(exp)) return -1;
    }
    return 0;
}

int kvs_dump_to_fd(int fd, unsigned long long aof_offset) {
    if (fd < 0) return -1;

    /* header: AOF file size at dump creation time */
    if (write(fd, &aof_offset, sizeof(aof_offset)) != sizeof(aof_offset)) return -1;

    /* dump helper: write [1B engine][1B flags][4B klen][key][4B vlen][value][optional 8B expire_ms] */
#define DUMP_WRITE_KV_EX(engine_id, key, value) do {                 \
    uint8_t  _eng = (uint8_t)(engine_id);                             \
    uint8_t  _flags = 0;                                              \
    uint32_t _klen = (uint32_t)strlen(key);                           \
    uint32_t _vlen = (uint32_t)strlen(value);                         \
    long long _ttl_sec = kvs_expire_ttl(&global_expire, _eng, key);   \
    uint64_t _exp = 0;                                                \
    if (_ttl_sec >= 0) { _flags = KVSD_FLAG_HAS_EXPIRE;               \
        _exp = (uint64_t)(kvs_now_ms() + _ttl_sec * 1000); }          \
    if (write(fd, &_eng, sizeof(_eng)) != sizeof(_eng)) return -1;   \
    if (write(fd, &_flags, sizeof(_flags)) != sizeof(_flags)) return -1; \
    if (write(fd, &_klen, sizeof(_klen)) != sizeof(_klen)) return -1; \
    if (write(fd, key, _klen) != (ssize_t)_klen) return -1;           \
    if (write(fd, &_vlen, sizeof(_vlen)) != sizeof(_vlen)) return -1; \
    if (write(fd, value, _vlen) != (ssize_t)_vlen) return -1;         \
    if (_flags & KVSD_FLAG_HAS_EXPIRE)                                 \
        if (write(fd, &_exp, sizeof(_exp)) != sizeof(_exp)) return -1; \
} while(0)

    /* iterate all hash entries */
    for (int t = 0; t < 2; t++) {
        if (!global_hash.ht[t].nodes) continue;
        for (int i = 0; i < global_hash.ht[t].max_slots; ++i) {
            for (hashnode_t *node = global_hash.ht[t].nodes[i]; node; node = node->next) {
                DUMP_WRITE_KV_EX(KVS_ENGINE_HASH, node->key, node->value);
            }
        }
    }

    /* iterate array entries */
    for (int i = 0; i < KVS_ARRAY_SIZE; ++i) {
        if (global_array.table && global_array.table[i].key) {
            DUMP_WRITE_KV_EX(KVS_ENGINE_ARRAY, global_array.table[i].key, global_array.table[i].value);
        }
    }

    /* iterate rbtree entries */
    {
        rbtree_node *nil = global_rbtree.nil;
        rbtree_node **stack = (rbtree_node **)kvs_malloc(sizeof(rbtree_node*) * 256);
        int top = 0;
        rbtree_node *cur = global_rbtree.root;
        if (stack) {
            while (cur != nil || top > 0) {
                while (cur != nil) {
                    stack[top++] = cur;
                    cur = cur->left;
                }
                cur = stack[--top];
                DUMP_WRITE_KV_EX(KVS_ENGINE_RBTREE, cur->key, (char*)cur->value);
                cur = cur->right;
            }
            kvs_free(stack);
        }
    }

    /* iterate skiptable entries */
    kvs_skiptable_foreach(&global_skiptable, dump_skiptable_write_kv, &fd);

    /* iterate doc entries (squash newlines to spaces) */
    {
        char doc_buf[BUFFER_CAP];
        for (int i = 0; i < global_doc.size; ++i) {
            for (kvs_doc_t *d = global_doc.buckets[i]; d; d = d->next) {
                int doc_pos = 0;
                for (int j = 0; j < d->bucket_count && doc_pos < (int)sizeof(doc_buf) - 4; ++j) {
                    for (kvs_doc_field_t *f = d->fields[j]; f; f = f->next) {
                        int n = snprintf(doc_buf + doc_pos, sizeof(doc_buf) - (size_t)doc_pos,
                            "%s=%s ", f->name, f->value);
                        if (n > 0) doc_pos += n;
                    }
                }
                if (doc_pos > 0 && doc_buf[doc_pos-1] == ' ') doc_pos--;
                doc_buf[doc_pos] = '\0';
                DUMP_WRITE_KV_EX(KVS_ENGINE_DOC, d->key, doc_buf);
            }
        }
    }

#undef DUMP_WRITE_KV_EX
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (parse_args(argc, argv) != 0) {
        fprintf(stderr, "Usage: %s [kvstore.conf] [--role master|slave] [options]\n"
                "  kvstore.conf 中的所有选项均可通过命令行覆盖:\n"
                "  --port PORT             监听端口 (默认 5160)\n"
                "  --role master|slave     角色 (默认 master)\n"
                "  --master-host HOST      主机地址 (默认 192.168.233.128)\n"
                "  --master-port PORT      主机端口 (默认 5160)\n"
                "  --dump PATH             dump 文件路径 (默认 kvstore.dump)\n"
                "  --aof PATH              AOF 文件路径 (默认 kvstore.aof)\n"
                "  --mem libc|jemalloc|custom  内存后端 (默认 libc)\n"
                "  --net reactor|proactor|ntyco  网络模型 (默认 reactor)\n"
                "  --repl-fullsync-transport tcp|rdma  全量同步传输 (默认 rdma)\n"
                "  --repl-realtime-transport tcp|kprobe-rdma|ebpf  增量同步传输 (默认 kprobe-rdma)\n"
                "  --kprobe-enabled        启用 kprobe+RDMA 增量同步\n"
                "  --rdma-dev DEV          RDMA 设备 (默认 siw0)\n"
                "  --rdma-port PORT        RDMA 监听端口 (默认 0 = main+1)\n"
                "  --rdma-ib-port PORT     RDMA IB 端口 (默认 1)\n"
                "  --rdma-gid-idx IDX      RDMA GID 索引 (默认 1)\n"
                "  --rdma-recv-slots N     接收槽位数 (默认 64)\n"
                "  --rdma-chunk-size SIZE  分块大小 (默认 262144)\n"
                "  --rdma-qp-wr-depth N    QP 队列深度 (默认 64)\n"
                "  --appendfsync always|everysec  AOF fsync 策略 (默认 always)\n"
                "  --ebpf-enabled          启用 eBPF sockmap\n"
                "  --ebpf-obj PATH         eBPF 对象文件路径\n"
                "  --ebpf-pin PATH         eBPF pin 路径\n"
                "  --ebpf-redirect         启用 eBPF 重定向\n"
                "  --ebpf-redirect-key N   eBPF 重定向 key\n"
                "  --ebpf-forward          启用 eBPF 转发\n"
                "  --aof-disable           禁用 AOF 持久化\n"
                "  --autosnap RULES        自动快照规则 (例如 60:1000,300:10)\n"
                "  --sentinel              启用哨兵模式\n"
                "  --sentinel-master-name NAME  哨兵主节点名\n"
                "  --sentinel-monitor-host HOST  哨兵监控主机\n"
                "  --sentinel-monitor-port PORT  哨兵监控端口\n"
                "  --sentinel-known-slaves LIST  已知从机列表\n"
                "  --sentinel-down-after MS     判定下线毫秒数\n"
                "  --sentinel-failover-timeout MS  故障转移超时\n"
                "  --sentinel-quorum N     哨兵法定人数\n"
                "  --log-mode MODE         日志模式 (默认 info)\n"
                "  --config PATH           配置文件路径 (默认 ./kvstore.conf)\n", argv[0]);
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
    if (g_cfg.role == ROLE_SLAVE) repl_slave_state_load();
    if (g_cfg.ebpf_enabled) {
        if (g_cfg.role == ROLE_MASTER && (!strcasecmp(g_cfg.repl_realtime_transport, "ebpf") || !strcasecmp(g_cfg.repl_realtime_transport, "sockmap")
                || !strcasecmp(g_cfg.repl_transport_backend, "ebpf") || !strcasecmp(g_cfg.repl_transport_backend, "sockmap"))) {
            if (repl_ebpf_init() != 0) {
                fprintf(stderr, "ebpf init failed, falling back to tcp replication transport\n");
                /* Non-fatal: continue with TCP fallback */
            }
        }
    }
    /* kprobe+RDMA 增量同步初始化 */
    if (g_cfg.kprobe_enabled &&
        !strcasecmp(g_cfg.repl_realtime_transport, "kprobe-rdma")) {
        if (g_cfg.role == ROLE_MASTER) {
            if (repl_kprobe_rdma_master_init() != 0) {
                fprintf(stderr, "kprobe rdma master init failed, disabling\n");
                g_cfg.kprobe_enabled = 0;
            }
        } else if (g_cfg.role == ROLE_SLAVE) {
            repl_kprobe_rdma_slave_init();
        }
    }

    /* Client capture 初始化 — 用于全量同步期间缓存客户端写入 */
    if (g_cfg.kprobe_enabled && g_cfg.role == ROLE_MASTER) {
        if (repl_client_capture_init() != 0) {
            fprintf(stderr, "client capture init failed, continuing without cache\n");
        }
    }

    /* kprobe 转发独立 TCP 监听（slave 侧，port+13） */
    if (g_cfg.kprobe_enabled && g_cfg.role == ROLE_SLAVE) {
        repl_kprobe_fwd_slave_init(g_cfg.port);
    }

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
