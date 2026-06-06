#include "kvstore/kvstore.h"
#include "kvstore/replication/repl_kprobe.h"
#include <poll.h>
#include <time.h>

#if KVS_ENABLE_RDMA
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#endif

#define KVS_REPL_BACKLOG_SIZE (1024 * 1024)

/* 传输层日志 */
static FILE *g_transport_log = NULL;
static pthread_mutex_t g_transport_log_lock = PTHREAD_MUTEX_INITIALIZER;

static void transport_log(const char *fmt, ...) {
    va_list ap;
    time_t now;
    struct tm *tm_info;
    char timestamp[64];

    pthread_mutex_lock(&g_transport_log_lock);
    if (!g_transport_log) {
        g_transport_log = fopen("kvstore_transport.log", "a");
        if (!g_transport_log) { pthread_mutex_unlock(&g_transport_log_lock); return; }
    }
    time(&now);
    tm_info = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    fprintf(g_transport_log, "[%s] ", timestamp);
    va_start(ap, fmt);
    vfprintf(g_transport_log, fmt, ap);
    va_end(ap);
    fprintf(g_transport_log, "\n");
    fflush(g_transport_log);
    pthread_mutex_unlock(&g_transport_log_lock);
}

typedef struct repl_backlog_s {
    unsigned char *buf;
    size_t cap;
    size_t histlen;
    size_t head;
    unsigned long long start_offset;
    unsigned long long end_offset;
} repl_backlog_t;

static pthread_mutex_t g_slave_conf_lock = PTHREAD_MUTEX_INITIALIZER;
#if KVS_ENABLE_RDMA
static pthread_mutex_t g_repl_rdma_send_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_repl_rdma_cq_lock = PTHREAD_MUTEX_INITIALIZER;
#endif
static char g_slave_host[128] = "";
static int g_slave_port = 0;
static int g_slave_conf_gen = 0;
static int g_master_link_up = 0;
static int g_slave_transport_kind = KVS_REPL_TRANSPORT_TCP;
/* 非 static，供 kvstore.c 中的 KPROBEMR handler 使用 */
int g_slave_fd = -1;
static long long g_master_last_io_ms = 0;
static int g_slave_thread_started = 0;
static int g_rdma_master_listener_started = 0;
static long long g_slave_last_ack_ms = 0;
static char g_master_replid[41] = {0};
static unsigned long long g_master_repl_offset = 0;
static unsigned long long g_repl_fullsync_count = 0;
static unsigned long long g_repl_partialsync_ok_count = 0;
static unsigned long long g_repl_partialsync_err_count = 0;
static unsigned long long g_repl_broadcast_bytes = 0;
static unsigned long long g_repl_snapshot_bytes = 0;
static unsigned long long g_rdma_disconnect_count = 0;
static unsigned long long g_rdma_reject_count = 0;
static unsigned long long g_rdma_send_cq_error_count = 0;
static unsigned long long g_rdma_recv_cq_error_count = 0;
static unsigned long long g_repl_transport_fallback_count = 0;
static char g_repl_transport_active[32] = "tcp";
static char g_repl_transport_fallback_reason[64] = "";
static long long g_repl_transport_fallback_until_ms = 0;
static repl_backlog_t g_repl_backlog = {0};
static char g_slave_master_replid[41] = "?";
static unsigned long long g_slave_repl_offset = 0;
static unsigned long long g_slave_repl_applied_offset = 0;
static unsigned long long g_slave_repl_durable_offset = 0;
static int g_slave_loading_fullsync = 0;
static unsigned long long g_slave_fullsync_target_bytes = 0;
static unsigned long long g_slave_fullsync_loaded_bytes = 0;
static char g_slave_state_path[512] = {0};
static conn_t g_rdma_master_replica_conn = {0};

#if KVS_ENABLE_RDMA
#define KVS_RDMA_RECV_SLOTS_MAX 64
#define KVS_RDMA_RECV_SLOTS_DEFAULT 32
#define KVS_RDMA_CHUNK_SIZE_DEFAULT (BUFFER_CAP * 4)
#define KVS_RDMA_QP_WR_DEPTH_DEFAULT 64

/* ---- Pipeline Constants ---- */
#define KVS_RDMA_PIPELINE_DEPTH      4   /* 多发送缓冲区深度 */
#define KVS_RDMA_CQ_BATCH            8   /* CQ 批量 poll 大小 */
#define KVS_RDMA_BATCH_MAX           8   /* 批量 send WR 上限 */
#define KVS_RDMA_PIPELINE_WR_ID_FLAG 0x80000000UL  /* wr_id 高位标记，区分 pipeline send vs recv */

typedef enum repl_rdma_state_e {
    REPL_RDMA_STATE_INIT = 0,
    REPL_RDMA_STATE_CONNECTING,
    REPL_RDMA_STATE_ESTABLISHED,
    REPL_RDMA_STATE_SYNCING,
    REPL_RDMA_STATE_STEADY,
    REPL_RDMA_STATE_BACKOFF,
    REPL_RDMA_STATE_FAILED,
    REPL_RDMA_STATE_FALLBACK_TCP,
} repl_rdma_state_t;

static void repl_rdma_reset_ctx(void);
static void repl_rdma_reset_conn_ctx(int preserve_listener);
static void repl_rdma_log(const char *stage, const char *detail);
static void repl_rdma_set_state(repl_rdma_state_t st, const char *reason);
static const char *repl_rdma_state_name(repl_rdma_state_t st);

typedef struct repl_rdma_recv_slot_s {
    struct ibv_mr *mr;
    unsigned char *buf;
    size_t cap;
    int posted;
} repl_rdma_recv_slot_t;

/* ---- Pipeline 发送缓冲区槽位 ---- */
typedef struct repl_rdma_send_slot_s {
    struct ibv_mr *mr;          /* 注册的内存区域 */
    unsigned char *buf;         /* 缓冲区 */
    size_t cap;                 /* 容量 */
    int in_flight;              /* 1 = 已 post 但未完成 */
    uint64_t wr_id;             /* 对应的 wr_id（含 PIPELINE_WR_ID_FLAG） */
} repl_rdma_send_slot_t;

typedef struct repl_rdma_ctx_s {
    struct rdma_event_channel *ec;
    struct rdma_cm_id *id;
    struct rdma_cm_id *listen_id;
    struct rdma_cm_id *accepted_id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_comp_channel *comp_chan;
    /* ---- 旧单 send_buf 保留作 fallback, send_pipeline 启用后不再使用 ---- */
    struct ibv_mr *send_mr;
    unsigned char *send_buf;
    size_t send_buf_cap;
    repl_rdma_recv_slot_t recv_slots[KVS_RDMA_RECV_SLOTS_MAX];
    size_t recv_buf_cap;
    int pending_recv_slots[KVS_RDMA_RECV_SLOTS_MAX];
    size_t pending_recv_lens[KVS_RDMA_RECV_SLOTS_MAX];
    int pending_recv_head;
    int pending_recv_tail;
    int pending_recv_count;
    int active_recv_slots;
    int active_qp_wr_depth;
    size_t active_chunk_size;
    int addr_resolved;
    int route_resolved;
    int qp_ready;
    int connected;
    repl_rdma_state_t state;
    /* ---- Pipeline 发送缓冲区 ---- */
    repl_rdma_send_slot_t send_slots[KVS_RDMA_PIPELINE_DEPTH];
    int send_pipeline_head;          /* 下一个可用的空闲 slot 索引 */
    int send_slots_in_flight;        /* 当前 outstanding send WR 数 */
    int send_pipeline_depth;         /* 当前生效的 pipeline 深度（≤ KVS_RDMA_PIPELINE_DEPTH） */
    int send_pipeline_enabled;       /* 是否启用 pipeline 模式 */

    /* ---- CQ 轮询线程 ---- */
    pthread_t cq_poll_thread;        /* CQ 轮询线程 ID */
    int cq_poll_thread_running;      /* CQ 轮询线程是否运行 */
} repl_rdma_ctx_t;

static repl_rdma_ctx_t g_repl_rdma_ctx = {0};
#endif

typedef struct repl_transport_ops_s {
    const char *name;
    int supported;
    int (*send)(conn_t *c, const unsigned char *buf, size_t len);
    int (*connect_slave)(const char *host, int port);
    void (*disconnect_slave)(int fd);
} repl_transport_ops_t;

static int repl_transport_tcp_send(conn_t *c, const unsigned char *buf, size_t len) {
    if (!c || !buf || len == 0) return 0;
    return queue_bytes(c, buf, len);
}

static int repl_transport_ebpf_send(conn_t *c, const unsigned char *buf, size_t len) {
    /* eBPF 增量传输：
     *
     * 数据路径: queue_bytes → reactor on_write → send(c->fd) 
     *          → 内核触发 sk_msg BPF (因 fd 已注册到 sockmap)
     *          → BPF 执行 bpf_msg_redirect_map() 
     *          → sock_map[redirect_key] 的 TCP → 远端 slave
     *
     * c->fd 已通过 register_fd() 注册到 sock_map 和 role_map，
     * 因此 send() 系统调用会被 BPF 程序拦截并重定向。
     * 传输层为 TCP（跨机器数据必须走网络协议），
     * 但数据路由由 eBPF 程序在内核态决策，而非直接走内核 TCP 栈。 */
    return queue_bytes(c, buf, len);
}

static int repl_transport_tcp_connect_slave(const char *host, int port) {
    int fd;
    struct timeval tv;
    struct sockaddr_in addr;
    if (!host || host[0] == '\0' || port <= 0) return -1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

#if KVS_ENABLE_RDMA
static void repl_rdma_log(const char *stage, const char *detail) {
    fprintf(stderr, "repl rdma: %s%s%s\n", stage ? stage : "?", detail ? " - " : "", detail ? detail : "");
}

static int repl_rdma_cfg_recv_slots(void) {
    int v = g_cfg.rdma_recv_slots;
    if (v <= 0) v = KVS_RDMA_RECV_SLOTS_DEFAULT;
    if (v > KVS_RDMA_RECV_SLOTS_MAX) v = KVS_RDMA_RECV_SLOTS_MAX;
    return v;
}

static int repl_rdma_cfg_qp_wr_depth(void) {
    int v = g_cfg.rdma_qp_wr_depth;
    int min_depth = repl_rdma_cfg_recv_slots() * 2;
    if (v <= 0) v = KVS_RDMA_QP_WR_DEPTH_DEFAULT;
    if (v < min_depth) v = min_depth;
    return v;
}

static size_t repl_rdma_cfg_chunk_size(void) {
    size_t v = (g_cfg.rdma_chunk_size > 0) ? (size_t)g_cfg.rdma_chunk_size : (size_t)KVS_RDMA_CHUNK_SIZE_DEFAULT;
    if (v < 1024) v = 1024;
    if (v > BUFFER_CAP * 4) v = BUFFER_CAP * 4;
    return v;
}

static void repl_rdma_refresh_runtime_cfg(void) {
    g_repl_rdma_ctx.active_recv_slots = repl_rdma_cfg_recv_slots();
    g_repl_rdma_ctx.active_qp_wr_depth = repl_rdma_cfg_qp_wr_depth();
    g_repl_rdma_ctx.active_chunk_size = repl_rdma_cfg_chunk_size();
}

/* 前向声明 */
static int repl_rdma_pending_recv_push(int slot, size_t len);
static int repl_rdma_pending_recv_pop(int *slot_out, size_t *len_out);
static void repl_rdma_stop_cq_poll_thread(void);
static volatile int g_cq_poll_thread_exited = 0;
static int repl_rdma_start_cq_poll_thread(void);

/* ---- Pipeline: 获取一个空闲的 send slot ----
 * 返回 slot 索引，-1 表示所有 slot 均在飞行中。
 * 如果 timeout_ms > 0，会忙等待直到有 slot 可用或超时。
 */
static int repl_rdma_acquire_send_slot(int timeout_ms) {
    int slot;
    long long deadline = timeout_ms > 0 ? kvs_now_ms() + timeout_ms : 0;
    for (;;) {
        /* 连接已断开，立即返回 */
        if (!g_repl_rdma_ctx.connected) break;
        /* 线性扫描取第一个空闲 slot */
        for (int i = 0; i < g_repl_rdma_ctx.send_pipeline_depth; ++i) {
            slot = (g_repl_rdma_ctx.send_pipeline_head + i) % g_repl_rdma_ctx.send_pipeline_depth;
            if (!g_repl_rdma_ctx.send_slots[slot].in_flight) {
                g_repl_rdma_ctx.send_pipeline_head = (slot + 1) % g_repl_rdma_ctx.send_pipeline_depth;
                return slot;
            }
        }
        /* 所有 slot 均在飞行中 */
        if (g_repl_rdma_ctx.cq_poll_thread_running) {
            /* CQ 轮询线程在后台运行，不直接 poll CQ，等它释放 slot */
            if (timeout_ms <= 0) break;
            if (kvs_now_ms() >= deadline) break;
            usleep(500);
            continue;
        }
        /* CQ 轮询线程未运行：直接 poll CQ 回收 completion */
        if (g_repl_rdma_ctx.connected && g_repl_rdma_ctx.cq) {
            struct ibv_wc wc;
            if (ibv_poll_cq(g_repl_rdma_ctx.cq, 1, &wc) > 0) {
                if (wc.status == IBV_WC_SUCCESS &&
                    (wc.opcode == IBV_WC_SEND) &&
                    (wc.wr_id & KVS_RDMA_PIPELINE_WR_ID_FLAG)) {
                    int done_slot = (int)(wc.wr_id & ~KVS_RDMA_PIPELINE_WR_ID_FLAG);
                    if (done_slot >= 0 && done_slot < g_repl_rdma_ctx.send_pipeline_depth) {
                        g_repl_rdma_ctx.send_slots[done_slot].in_flight = 0;
                        g_repl_rdma_ctx.send_slots[done_slot].wr_id = 0;
                        g_repl_rdma_ctx.send_slots_in_flight--;
                        continue; /* 重试 */
                    }
                } else if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_RECV) {
                    /* 顺便处理 recv completion */
                    int recv_slot = (wc.wr_id > 0 && wc.wr_id <= (uint64_t)g_repl_rdma_ctx.active_recv_slots)
                        ? (int)(wc.wr_id - 1) : -1;
                    if (recv_slot >= 0 && recv_slot < g_repl_rdma_ctx.active_recv_slots) {
                        g_repl_rdma_ctx.recv_slots[recv_slot].posted = 0;
                        repl_rdma_pending_recv_push(recv_slot, (size_t)wc.byte_len);
                    }
                    continue;
                } else {
                    fprintf(stderr, "repl rdma: acquire_send_slot unexpected wc status=%d opcode=%d\n",
                        wc.status, wc.opcode);
                }
            }
        }
        if (timeout_ms <= 0) break;
        if (kvs_now_ms() >= deadline) break;
        usleep(500);
    }
    return -1;
}

/* ---- Pipeline: 释放一个 send slot（由 CQ completion 回调调用）---- */
static void repl_rdma_release_send_slot(int slot) {
    if (slot >= 0 && slot < g_repl_rdma_ctx.send_pipeline_depth) {
        g_repl_rdma_ctx.send_slots[slot].in_flight = 0;
        g_repl_rdma_ctx.send_slots[slot].wr_id = 0;
        g_repl_rdma_ctx.send_slots_in_flight--;
    }
}

static int repl_rdma_pending_recv_push(int slot, size_t len) {
    if (slot < 0 || slot >= KVS_RDMA_RECV_SLOTS_MAX) return -1;
    if (g_repl_rdma_ctx.pending_recv_count >= KVS_RDMA_RECV_SLOTS_MAX) return -1;
    g_repl_rdma_ctx.pending_recv_slots[g_repl_rdma_ctx.pending_recv_tail] = slot;
    g_repl_rdma_ctx.pending_recv_lens[g_repl_rdma_ctx.pending_recv_tail] = len;
    g_repl_rdma_ctx.pending_recv_tail = (g_repl_rdma_ctx.pending_recv_tail + 1) % KVS_RDMA_RECV_SLOTS_MAX;
    g_repl_rdma_ctx.pending_recv_count++;
    return 0;
}

static int repl_rdma_pending_recv_pop(int *slot_out, size_t *len_out) {
    int slot;
    size_t len;
    if (g_repl_rdma_ctx.pending_recv_count <= 0) return -1;
    slot = g_repl_rdma_ctx.pending_recv_slots[g_repl_rdma_ctx.pending_recv_head];
    len = g_repl_rdma_ctx.pending_recv_lens[g_repl_rdma_ctx.pending_recv_head];
    g_repl_rdma_ctx.pending_recv_head = (g_repl_rdma_ctx.pending_recv_head + 1) % KVS_RDMA_RECV_SLOTS_MAX;
    g_repl_rdma_ctx.pending_recv_count--;
    if (slot_out) *slot_out = slot;
    if (len_out) *len_out = len;
    return 0;
}

static void repl_rdma_reset_conn_ctx(int preserve_listener) {
    int i;
    /* 先停止 CQ 轮询线程（后续销毁 comp_chan/cq 时会唤醒线程） */
    repl_rdma_stop_cq_poll_thread();
    for (i = 0; i < KVS_RDMA_RECV_SLOTS_MAX; ++i) {
        if (g_repl_rdma_ctx.recv_slots[i].mr) {
            ibv_dereg_mr(g_repl_rdma_ctx.recv_slots[i].mr);
            g_repl_rdma_ctx.recv_slots[i].mr = NULL;
        }
        if (g_repl_rdma_ctx.recv_slots[i].buf) {
            kvs_free(g_repl_rdma_ctx.recv_slots[i].buf);
            g_repl_rdma_ctx.recv_slots[i].buf = NULL;
        }
        g_repl_rdma_ctx.recv_slots[i].cap = 0;
        g_repl_rdma_ctx.recv_slots[i].posted = 0;
    }
    /* 清理旧单 send_buf（兼容） */
    if (g_repl_rdma_ctx.send_mr) {
        ibv_dereg_mr(g_repl_rdma_ctx.send_mr);
        g_repl_rdma_ctx.send_mr = NULL;
    }
    if (g_repl_rdma_ctx.send_buf) {
        kvs_free(g_repl_rdma_ctx.send_buf);
        g_repl_rdma_ctx.send_buf = NULL;
    }
    g_repl_rdma_ctx.send_buf_cap = 0;
    /* 清理 pipeline 多发送缓冲区 */
    for (i = 0; i < KVS_RDMA_PIPELINE_DEPTH; ++i) {
        if (g_repl_rdma_ctx.send_slots[i].mr) {
            ibv_dereg_mr(g_repl_rdma_ctx.send_slots[i].mr);
            g_repl_rdma_ctx.send_slots[i].mr = NULL;
        }
        if (g_repl_rdma_ctx.send_slots[i].buf) {
            kvs_free(g_repl_rdma_ctx.send_slots[i].buf);
            g_repl_rdma_ctx.send_slots[i].buf = NULL;
        }
        g_repl_rdma_ctx.send_slots[i].cap = 0;
        g_repl_rdma_ctx.send_slots[i].in_flight = 0;
        g_repl_rdma_ctx.send_slots[i].wr_id = 0;
    }
    g_repl_rdma_ctx.send_pipeline_head = 0;
    g_repl_rdma_ctx.send_slots_in_flight = 0;
    g_repl_rdma_ctx.send_pipeline_depth = KVS_RDMA_PIPELINE_DEPTH;
    g_repl_rdma_ctx.send_pipeline_enabled = 0;
    g_repl_rdma_ctx.recv_buf_cap = 0;
    memset(g_repl_rdma_ctx.pending_recv_slots, 0, sizeof(g_repl_rdma_ctx.pending_recv_slots));
    memset(g_repl_rdma_ctx.pending_recv_lens, 0, sizeof(g_repl_rdma_ctx.pending_recv_lens));
    g_repl_rdma_ctx.pending_recv_head = 0;
    g_repl_rdma_ctx.pending_recv_tail = 0;
    g_repl_rdma_ctx.pending_recv_count = 0;
    g_repl_rdma_ctx.active_recv_slots = 0;
    g_repl_rdma_ctx.active_qp_wr_depth = 0;
    g_repl_rdma_ctx.active_chunk_size = 0;
    if (g_repl_rdma_ctx.cq) {
        ibv_destroy_cq(g_repl_rdma_ctx.cq);
        g_repl_rdma_ctx.cq = NULL;
    }
    if (g_repl_rdma_ctx.comp_chan) {
        ibv_destroy_comp_channel(g_repl_rdma_ctx.comp_chan);
        g_repl_rdma_ctx.comp_chan = NULL;
    }
    if (g_repl_rdma_ctx.pd) {
        ibv_dealloc_pd(g_repl_rdma_ctx.pd);
        g_repl_rdma_ctx.pd = NULL;
    }
    if (g_repl_rdma_ctx.id) {
        struct rdma_cm_id *id = g_repl_rdma_ctx.id;
        g_repl_rdma_ctx.id = NULL;
        if (g_repl_rdma_ctx.accepted_id == id) g_repl_rdma_ctx.accepted_id = NULL;
        if (g_repl_rdma_ctx.listen_id == id) g_repl_rdma_ctx.listen_id = NULL;
        rdma_destroy_id(id);
    }
    if (g_repl_rdma_ctx.accepted_id) {
        struct rdma_cm_id *accepted_id = g_repl_rdma_ctx.accepted_id;
        g_repl_rdma_ctx.accepted_id = NULL;
        if (g_repl_rdma_ctx.listen_id == accepted_id) g_repl_rdma_ctx.listen_id = NULL;
        rdma_destroy_id(accepted_id);
    }
    if (!preserve_listener) {
        if (g_repl_rdma_ctx.listen_id) {
            rdma_destroy_id(g_repl_rdma_ctx.listen_id);
            g_repl_rdma_ctx.listen_id = NULL;
        }
        if (g_repl_rdma_ctx.ec) {
            rdma_destroy_event_channel(g_repl_rdma_ctx.ec);
            g_repl_rdma_ctx.ec = NULL;
        }
    }
    g_repl_rdma_ctx.addr_resolved = 0;
    g_repl_rdma_ctx.route_resolved = 0;
    g_repl_rdma_ctx.qp_ready = 0;
    g_repl_rdma_ctx.connected = 0;
    repl_rdma_set_state(preserve_listener ? REPL_RDMA_STATE_BACKOFF : REPL_RDMA_STATE_INIT, preserve_listener ? "reset_conn_ctx_preserve_listener" : "reset_ctx");
}

static void repl_rdma_reset_ctx(void) {
    char last_stage[64];
    char last_preview[160];
    unsigned long long last_len = 0;
    unsigned long long last_offset = 0;
    repl_get_last_send_context(last_stage, sizeof(last_stage), &last_len, &last_offset, last_preview, sizeof(last_preview));
    fprintf(stderr, "repl rdma: reset_ctx - last_send_stage=%s last_send_len=%llu last_send_offset=%llu last_send_preview=%s\n",
        last_stage, last_len, last_offset, last_preview);
    repl_rdma_reset_conn_ctx(0);
}

static int repl_rdma_wait_event(enum rdma_cm_event_type expect, int timeout_ms);
static void repl_rdma_drop_master_replica_from_list(const char *reason);

static int repl_rdma_wait_event(enum rdma_cm_event_type expect, int timeout_ms) {
    struct pollfd pfd;
    struct rdma_cm_event *event = NULL;
    if (!g_repl_rdma_ctx.ec) return -1;
    fprintf(stderr, "repl rdma: wait_event_begin - expect=%s timeout_ms=%d\n", rdma_event_str(expect), timeout_ms);
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = g_repl_rdma_ctx.ec->fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, timeout_ms) <= 0) {
        repl_rdma_log("wait_event", "poll timeout or error");
        return -1;
    }
    if (rdma_get_cm_event(g_repl_rdma_ctx.ec, &event) != 0) {
        repl_rdma_log("wait_event", "rdma_get_cm_event failed");
        return -1;
    }
    int ok = (event->event == expect) ? 0 : -1;
    if (ok == 0) repl_rdma_log("cm_event", rdma_event_str(event->event));
    else fprintf(stderr, "repl rdma: cm_event unexpected - got=%s expect=%s\n", rdma_event_str(event->event), rdma_event_str(expect));
    rdma_ack_cm_event(event);
    return ok;
}

static int repl_rdma_drain_cm_events_nonblock(void) {
    struct pollfd pfd;
    struct rdma_cm_event *event = NULL;
    int saw_disconnect = 0;
    if (!g_repl_rdma_ctx.ec) return 0;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = g_repl_rdma_ctx.ec->fd;
    pfd.events = POLLIN;
    while (poll(&pfd, 1, 0) > 0) {
        if (rdma_get_cm_event(g_repl_rdma_ctx.ec, &event) != 0) break;
        repl_rdma_log("cm_event_async", rdma_event_str(event->event));
        switch (event->event) {
            case RDMA_CM_EVENT_DISCONNECTED:
            case RDMA_CM_EVENT_REJECTED:
            case RDMA_CM_EVENT_ADDR_ERROR:
            case RDMA_CM_EVENT_ROUTE_ERROR:
            case RDMA_CM_EVENT_CONNECT_ERROR:
            case RDMA_CM_EVENT_UNREACHABLE:
            case RDMA_CM_EVENT_DEVICE_REMOVAL:
            case RDMA_CM_EVENT_TIMEWAIT_EXIT:
                if (event->event == RDMA_CM_EVENT_REJECTED) g_rdma_reject_count++;
                else g_rdma_disconnect_count++;
                saw_disconnect = 1;
                {
                    char last_stage[64];
                    char last_preview[160];
                    unsigned long long last_len = 0;
                    unsigned long long last_offset = 0;
                    repl_get_last_send_context(last_stage, sizeof(last_stage), &last_len, &last_offset, last_preview, sizeof(last_preview));
                    fprintf(stderr, "repl rdma: cm_event_async_context - last_send_stage=%s last_send_len=%llu last_send_offset=%llu last_send_preview=%s\n",
                        last_stage, last_len, last_offset, last_preview);
                }
                repl_rdma_log("cm_event_async", "marking transport disconnected");
                g_repl_rdma_ctx.connected = 0;
                repl_rdma_drop_master_replica_from_list("cm_event_async_disconnect");
                repl_rdma_set_state(REPL_RDMA_STATE_FAILED, rdma_event_str(event->event));
                break;
            default:
                break;
        }
        rdma_ack_cm_event(event);
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = g_repl_rdma_ctx.ec->fd;
        pfd.events = POLLIN;
    }
    return saw_disconnect ? -1 : 0;
}

static void repl_rdma_drop_master_replica_shallow(conn_t *c) {
    if (!c) return;
    c->next_replica = NULL;
    c->is_replica = 0;
    c->repl_draining = 0;
}

static void repl_rdma_drop_master_replica_from_list(const char *reason) {
    if (!g_rdma_master_replica_conn.is_replica && !g_rdma_master_replica_conn.next_replica && !g_rdma_master_replica_conn.repl_draining) return;
    if (reason && *reason) repl_rdma_log("replica_cleanup", reason);
    g_rdma_master_replica_conn.repl_draining = 1;
    repl_remove_slave(&g_rdma_master_replica_conn);
    repl_rdma_drop_master_replica_shallow(&g_rdma_master_replica_conn);
}

static int repl_rdma_pick_non_loopback_ipv4(struct in_addr *out) {
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa = NULL;
    int rc = -1;
    if (!out) return -1;
    if (getifaddrs(&ifaddr) != 0) return -1;
    for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        struct sockaddr_in *sin;
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if ((ifa->ifa_flags & IFF_UP) == 0) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        sin = (struct sockaddr_in *)ifa->ifa_addr;
        if (sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK) || sin->sin_addr.s_addr == 0) continue;
        *out = sin->sin_addr;
        rc = 0;
        break;
    }
    freeifaddrs(ifaddr);
    return rc;
}

static int repl_rdma_prepare_addr(const char *host, int port, struct sockaddr_in *addr) {
    if (!host || !addr || port <= 0) return -1;
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((uint16_t)port);
    if (!strcmp(host, "127.0.0.1") || !strcmp(host, "localhost")) {
        if (repl_rdma_pick_non_loopback_ipv4(&addr->sin_addr) == 0) {
            char ipbuf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &addr->sin_addr, ipbuf, sizeof(ipbuf));
            fprintf(stderr, "repl rdma: prepare_addr - remapped loopback host %s to %s\n", host, ipbuf[0] ? ipbuf : "?");
            return 0;
        }
    }
    if (inet_pton(AF_INET, host, &addr->sin_addr) <= 0) return -1;
    return 0;
}

static int repl_rdma_create_qp(void) {
    struct ibv_qp_init_attr attr;
    repl_rdma_refresh_runtime_cfg();
    if (!g_repl_rdma_ctx.id || !g_repl_rdma_ctx.pd || !g_repl_rdma_ctx.cq) return -1;
    memset(&attr, 0, sizeof(attr));
    attr.send_cq = g_repl_rdma_ctx.cq;
    attr.recv_cq = g_repl_rdma_ctx.cq;
    attr.qp_type = IBV_QPT_RC;
    attr.cap.max_send_wr = (uint32_t)g_repl_rdma_ctx.active_qp_wr_depth;
    attr.cap.max_recv_wr = (uint32_t)g_repl_rdma_ctx.active_qp_wr_depth;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;
    if (rdma_create_qp(g_repl_rdma_ctx.id, g_repl_rdma_ctx.pd, &attr) != 0) {
        repl_rdma_log("create_qp", "rdma_create_qp failed");
        return -1;
    }
    g_repl_rdma_ctx.qp_ready = 1;
    return 0;
}

static int repl_rdma_connect_handshake(void) {
    struct rdma_conn_param param;
    const int establish_timeout_ms = 12000;
    if (!g_repl_rdma_ctx.id || !g_repl_rdma_ctx.qp_ready) return -1;
    memset(&param, 0, sizeof(param));
    param.initiator_depth = 1;
    param.responder_resources = 1;
    param.retry_count = 7;
    param.rnr_retry_count = 7;
    repl_rdma_log("connect", "issuing rdma_connect");
    repl_rdma_set_state(REPL_RDMA_STATE_CONNECTING, "rdma_connect");
    if (rdma_connect(g_repl_rdma_ctx.id, &param) != 0) {
        repl_rdma_log("connect", "rdma_connect failed");
        return -1;
    }
    if (repl_rdma_wait_event(RDMA_CM_EVENT_ESTABLISHED, establish_timeout_ms) != 0) {
        repl_rdma_log("connect", "established wait timed out or failed");
        return -1;
    }
    g_repl_rdma_ctx.connected = 1;
    repl_rdma_set_state(REPL_RDMA_STATE_ESTABLISHED, "established");
    repl_rdma_log("connect", "established");
    return 0;
}

static int repl_rdma_prepare_buffers(void) {
    repl_rdma_refresh_runtime_cfg();
    size_t cap = repl_rdma_cfg_chunk_size();
    if (cap < BUFFER_CAP) cap = BUFFER_CAP;
    int i;
    if (!g_repl_rdma_ctx.pd) return -1;
    /* 分配 pipeline 多发送缓冲区 */
    g_repl_rdma_ctx.send_slots_in_flight = 0;
    g_repl_rdma_ctx.send_pipeline_head = 0;
    g_repl_rdma_ctx.send_pipeline_depth = KVS_RDMA_PIPELINE_DEPTH;
    g_repl_rdma_ctx.send_pipeline_enabled = 1;
    for (i = 0; i < KVS_RDMA_PIPELINE_DEPTH; ++i) {
        g_repl_rdma_ctx.send_slots[i].buf = (unsigned char *)kvs_malloc(cap);
        if (!g_repl_rdma_ctx.send_slots[i].buf) {
            repl_rdma_log("prepare_buffers", "pipeline send buffer alloc failed");
            return -1;
        }
        g_repl_rdma_ctx.send_slots[i].cap = cap;
        g_repl_rdma_ctx.send_slots[i].in_flight = 0;
        g_repl_rdma_ctx.send_slots[i].wr_id = 0;
        memset(g_repl_rdma_ctx.send_slots[i].buf, 0, cap);
        g_repl_rdma_ctx.send_slots[i].mr = ibv_reg_mr(g_repl_rdma_ctx.pd,
            g_repl_rdma_ctx.send_slots[i].buf, cap, IBV_ACCESS_LOCAL_WRITE);
        if (!g_repl_rdma_ctx.send_slots[i].mr) {
            repl_rdma_log("prepare_buffers", "pipeline send mr register failed");
            return -1;
        }
    }
    /* 旧单 send_buf 保留作兼容 */
    g_repl_rdma_ctx.send_buf = (unsigned char *)kvs_malloc(cap);
    if (!g_repl_rdma_ctx.send_buf) {
        repl_rdma_log("prepare_buffers", "legacy buffer alloc failed");
        return -1;
    }
    g_repl_rdma_ctx.send_buf_cap = cap;
    g_repl_rdma_ctx.recv_buf_cap = cap;
    memset(g_repl_rdma_ctx.send_buf, 0, cap);
    g_repl_rdma_ctx.send_mr = ibv_reg_mr(g_repl_rdma_ctx.pd, g_repl_rdma_ctx.send_buf, cap, IBV_ACCESS_LOCAL_WRITE);
    if (!g_repl_rdma_ctx.send_mr) {
        repl_rdma_log("prepare_buffers", "legacy send mr register failed");
        return -1;
    }
    for (i = 0; i < g_repl_rdma_ctx.active_recv_slots; ++i) {
        g_repl_rdma_ctx.recv_slots[i].buf = (unsigned char *)kvs_malloc(cap);
        if (!g_repl_rdma_ctx.recv_slots[i].buf) {
            repl_rdma_log("prepare_buffers", "recv buffer alloc failed");
            return -1;
        }
        g_repl_rdma_ctx.recv_slots[i].cap = cap;
        g_repl_rdma_ctx.recv_slots[i].posted = 0;
        memset(g_repl_rdma_ctx.recv_slots[i].buf, 0, cap);
        g_repl_rdma_ctx.recv_slots[i].mr = ibv_reg_mr(g_repl_rdma_ctx.pd, g_repl_rdma_ctx.recv_slots[i].buf, cap, IBV_ACCESS_LOCAL_WRITE);
        if (!g_repl_rdma_ctx.recv_slots[i].mr) {
            repl_rdma_log("prepare_buffers", "recv mr register failed");
            return -1;
        }
    }
    return 0;
}

static int repl_rdma_post_recv_slot(int slot) {
    struct ibv_sge sge;
    struct ibv_recv_wr wr;
    struct ibv_recv_wr *bad_wr = NULL;
    if (slot < 0 || slot >= g_repl_rdma_ctx.active_recv_slots) return -1;
    if (!g_repl_rdma_ctx.id || !g_repl_rdma_ctx.id->qp || !g_repl_rdma_ctx.recv_slots[slot].mr || !g_repl_rdma_ctx.recv_slots[slot].buf) return -1;
    memset(g_repl_rdma_ctx.recv_slots[slot].buf, 0, g_repl_rdma_ctx.recv_slots[slot].cap);
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)g_repl_rdma_ctx.recv_slots[slot].buf;
    sge.length = (uint32_t)g_repl_rdma_ctx.recv_slots[slot].cap;
    sge.lkey = g_repl_rdma_ctx.recv_slots[slot].mr->lkey;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = (uint64_t)(slot + 1);
    wr.sg_list = &sge;
    wr.num_sge = 1;
    if (ibv_post_recv(g_repl_rdma_ctx.id->qp, &wr, &bad_wr) != 0) {
        repl_rdma_log("post_recv", "ibv_post_recv failed");
        return -1;
    }
    g_repl_rdma_ctx.recv_slots[slot].posted = 1;
    return 0;
}

static int repl_rdma_post_initial_recv(void) {
    int slot;
    for (slot = 0; slot < g_repl_rdma_ctx.active_recv_slots; ++slot) {
        if (repl_rdma_post_recv_slot(slot) != 0) return -1;
    }
    return 0;
}

static int repl_rdma_repost_recv(int slot) {
    return repl_rdma_post_recv_slot(slot);
}

static unsigned char *repl_rdma_dup_recv_payload(int slot, size_t len) {
    unsigned char *copy;
    if (slot < 0 || slot >= g_repl_rdma_ctx.active_recv_slots) return NULL;
    if (len == 0 || !g_repl_rdma_ctx.recv_slots[slot].buf || len > g_repl_rdma_ctx.recv_slots[slot].cap) return NULL;
    copy = (unsigned char *)kvs_malloc(len);
    if (!copy) return NULL;
    memcpy(copy, g_repl_rdma_ctx.recv_slots[slot].buf, len);
    return copy;
}

static int repl_rdma_wait_cq_send_completion(int timeout_ms) {
    struct ibv_wc wc;
    long long deadline = kvs_now_ms() + timeout_ms;
    if (!g_repl_rdma_ctx.cq) return -1;
    for (;;) {
        int n;
        pthread_mutex_lock(&g_repl_rdma_cq_lock);
        n = ibv_poll_cq(g_repl_rdma_ctx.cq, 1, &wc);
        if (n > 0) {
            if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_SEND) {
                /* Pipeline send completion */
                if (wc.wr_id & KVS_RDMA_PIPELINE_WR_ID_FLAG) {
                    int slot = (int)(wc.wr_id & ~KVS_RDMA_PIPELINE_WR_ID_FLAG);
                    if (slot >= 0 && slot < g_repl_rdma_ctx.send_pipeline_depth) {
                        g_repl_rdma_ctx.send_slots[slot].in_flight = 0;
                        g_repl_rdma_ctx.send_slots[slot].wr_id = 0;
                        g_repl_rdma_ctx.send_slots_in_flight--;
                    }
                }
                pthread_mutex_unlock(&g_repl_rdma_cq_lock);
                return 0;
            }
            if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_RECV) {
                int slot = (wc.wr_id > 0 && wc.wr_id <= (uint64_t)g_repl_rdma_ctx.active_recv_slots) ? (int)(wc.wr_id - 1) : -1;
                if (slot >= 0 && slot < g_repl_rdma_ctx.active_recv_slots) g_repl_rdma_ctx.recv_slots[slot].posted = 0;
                if (slot >= 0 && repl_rdma_pending_recv_push(slot, (size_t)wc.byte_len) != 0) {
                    pthread_mutex_unlock(&g_repl_rdma_cq_lock);
                    repl_rdma_log("send_cq", "pending recv queue overflow");
                    g_repl_rdma_ctx.connected = 0;
                    return -1;
                }
                pthread_mutex_unlock(&g_repl_rdma_cq_lock);
                continue;
            }
            pthread_mutex_unlock(&g_repl_rdma_cq_lock);
            fprintf(stderr, "repl rdma: send_cq failed - status=%d opcode=%d\n", wc.status, wc.opcode);
            g_rdma_send_cq_error_count++;
            g_repl_rdma_ctx.connected = 0;
            return -1;
        }
        pthread_mutex_unlock(&g_repl_rdma_cq_lock);
        if (repl_rdma_drain_cm_events_nonblock() != 0 || !g_repl_rdma_ctx.connected) {
            repl_rdma_log("send_cq", "transport already disconnected");
            return -1;
        }
        if (kvs_now_ms() >= deadline) {
            repl_rdma_drain_cm_events_nonblock();
            g_repl_rdma_ctx.connected = 0;
            repl_rdma_log("send_cq", "poll timeout or error");
            return -1;
        }
        usleep(1000);
    }
}

static int repl_rdma_wait_cq_recv_completion(int timeout_ms, int *slot_out, size_t *recv_len) {
    struct ibv_wc wc;
    long long deadline = kvs_now_ms() + timeout_ms;
    if (slot_out) *slot_out = -1;
    if (recv_len) *recv_len = 0;

    /* 优先从 pending_recv 队列取（CQ 轮询线程异步填充） */
    for (;;) {
        pthread_mutex_lock(&g_repl_rdma_cq_lock);
        if (repl_rdma_pending_recv_pop(slot_out, recv_len) == 0) {
            pthread_mutex_unlock(&g_repl_rdma_cq_lock);
            return (slot_out && *slot_out >= 0) ? 0 : -1;
        }
        pthread_mutex_unlock(&g_repl_rdma_cq_lock);
        /* pending 队列为空 */
        break;
    }

    /* CQ 轮询线程运行时，不直接 poll CQ（避免竞争），只等 pending 队列 */
    if (g_repl_rdma_ctx.cq_poll_thread_running) {
        /* 忙等待 pending 队列直到超时 */
        while (kvs_now_ms() < deadline) {
            usleep(1000);
            pthread_mutex_lock(&g_repl_rdma_cq_lock);
            if (repl_rdma_pending_recv_pop(slot_out, recv_len) == 0) {
                pthread_mutex_unlock(&g_repl_rdma_cq_lock);
                return (slot_out && *slot_out >= 0) ? 0 : -1;
            }
            pthread_mutex_unlock(&g_repl_rdma_cq_lock);
            if (repl_rdma_drain_cm_events_nonblock() != 0 || !g_repl_rdma_ctx.connected) {
                repl_rdma_log("recv_cq", "transport already disconnected");
                return -1;
            }
        }
        return -1;
    }

    /* ---- CQ 轮询线程未运行：直接 poll CQ（向后兼容） ---- */
    if (!g_repl_rdma_ctx.cq) return -1;
    for (;;) {
        int n;
        pthread_mutex_lock(&g_repl_rdma_cq_lock);
        n = ibv_poll_cq(g_repl_rdma_ctx.cq, 1, &wc);
        if (n > 0) {
            if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_RECV) {
                int slot = (wc.wr_id > 0 && wc.wr_id <= (uint64_t)g_repl_rdma_ctx.active_recv_slots) ? (int)(wc.wr_id - 1) : -1;
                if (slot >= 0 && slot < g_repl_rdma_ctx.active_recv_slots) g_repl_rdma_ctx.recv_slots[slot].posted = 0;
                if (slot_out) *slot_out = slot;
                if (recv_len) *recv_len = (size_t)wc.byte_len;
                pthread_mutex_unlock(&g_repl_rdma_cq_lock);
                return (slot >= 0) ? 0 : -1;
            }
            if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_SEND) {
                /* Pipeline send completion — release slot */
                if (wc.wr_id & KVS_RDMA_PIPELINE_WR_ID_FLAG) {
                    int slot = (int)(wc.wr_id & ~KVS_RDMA_PIPELINE_WR_ID_FLAG);
                    if (slot >= 0 && slot < g_repl_rdma_ctx.send_pipeline_depth) {
                        g_repl_rdma_ctx.send_slots[slot].in_flight = 0;
                        g_repl_rdma_ctx.send_slots[slot].wr_id = 0;
                        g_repl_rdma_ctx.send_slots_in_flight--;
                    }
                }
                pthread_mutex_unlock(&g_repl_rdma_cq_lock);
                continue;
            }
            pthread_mutex_unlock(&g_repl_rdma_cq_lock);
            fprintf(stderr, "repl rdma: recv_cq failed - status=%d opcode=%d\n", wc.status, wc.opcode);
            g_rdma_recv_cq_error_count++;
            if (repl_rdma_drain_cm_events_nonblock() != 0 || !g_repl_rdma_ctx.connected) {
                repl_rdma_log("recv_cq", "transport already disconnected after recv failure");
            }
            return -1;
        }
        pthread_mutex_unlock(&g_repl_rdma_cq_lock);
        if (repl_rdma_drain_cm_events_nonblock() != 0 || !g_repl_rdma_ctx.connected) {
            repl_rdma_log("recv_cq", "transport already disconnected while waiting for recv");
            return -1;
        }
        if (kvs_now_ms() >= deadline) {
            repl_rdma_log("recv_cq", "poll timeout or error");
            return -1;
        }
        usleep(1000);
    }
}

static int repl_rdma_try_send(const unsigned char *buf, size_t len) {
    struct ibv_sge sge;
    struct ibv_send_wr wr;
    struct ibv_send_wr *bad_wr = NULL;
    int slot;
    if (!buf || len == 0) return 0;
    pthread_mutex_lock(&g_repl_rdma_send_lock);
    if (repl_rdma_drain_cm_events_nonblock() != 0) {
        pthread_mutex_unlock(&g_repl_rdma_send_lock);
        return -1;
    }
    if (!g_repl_rdma_ctx.connected || !g_repl_rdma_ctx.id || !g_repl_rdma_ctx.id->qp) {
        pthread_mutex_unlock(&g_repl_rdma_send_lock);
        return -1;
    }
    if (g_repl_rdma_ctx.send_pipeline_enabled) {
        /* ---- Pipeline 模式：非阻塞发送 ---- */
        if (len > g_repl_rdma_ctx.send_slots[0].cap) {
            repl_rdma_log("try_send", "pipeline payload too large");
            pthread_mutex_unlock(&g_repl_rdma_send_lock);
            return -1;
        }
        /* 获取空闲 send slot（等待不超过 5s） */
        slot = repl_rdma_acquire_send_slot(5000);
        if (slot < 0) {
            repl_rdma_log("try_send", "no available send slot");
            pthread_mutex_unlock(&g_repl_rdma_send_lock);
            return -1;
        }
        /* 拷贝数据到 slot buffer */
        memcpy(g_repl_rdma_ctx.send_slots[slot].buf, buf, len);
        memset(&sge, 0, sizeof(sge));
        sge.addr = (uintptr_t)g_repl_rdma_ctx.send_slots[slot].buf;
        sge.length = (uint32_t)len;
        sge.lkey = g_repl_rdma_ctx.send_slots[slot].mr->lkey;
        memset(&wr, 0, sizeof(wr));
        wr.wr_id = (uint64_t)slot | KVS_RDMA_PIPELINE_WR_ID_FLAG;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.opcode = IBV_WR_SEND;
        wr.send_flags = IBV_SEND_SIGNALED;
        if (ibv_post_send(g_repl_rdma_ctx.id->qp, &wr, &bad_wr) != 0) {
            repl_rdma_log("try_send", "pipeline ibv_post_send failed");
            repl_rdma_release_send_slot(slot);
            g_repl_rdma_ctx.connected = 0;
            pthread_mutex_unlock(&g_repl_rdma_send_lock);
            return -1;
        }
        /* 标记 in_flight，立即返回（不等待 CQ） */
        g_repl_rdma_ctx.send_slots[slot].in_flight = 1;
        g_repl_rdma_ctx.send_slots[slot].wr_id = wr.wr_id;
        g_repl_rdma_ctx.send_slots_in_flight++;
        pthread_mutex_unlock(&g_repl_rdma_send_lock);
        return 0;
    }
    /* ---- 兼容旧模式：同步发送 ---- */
    if (!g_repl_rdma_ctx.send_buf || !g_repl_rdma_ctx.send_mr) {
        pthread_mutex_unlock(&g_repl_rdma_send_lock);
        return -1;
    }
    if (len > g_repl_rdma_ctx.send_buf_cap) {
        repl_rdma_log("try_send", "payload too large");
        pthread_mutex_unlock(&g_repl_rdma_send_lock);
        return -1;
    }
    memcpy(g_repl_rdma_ctx.send_buf, buf, len);
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)g_repl_rdma_ctx.send_buf;
    sge.length = (uint32_t)len;
    sge.lkey = g_repl_rdma_ctx.send_mr->lkey;
    memset(&wr, 0, sizeof(wr));
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    if (ibv_post_send(g_repl_rdma_ctx.id->qp, &wr, &bad_wr) != 0) {
        repl_rdma_log("try_send", "ibv_post_send failed");
        g_repl_rdma_ctx.connected = 0;
        pthread_mutex_unlock(&g_repl_rdma_send_lock);
        return -1;
    }
    /* 旧模式仍同步等待 completion */
    int rc = repl_rdma_wait_cq_send_completion(5000);
    pthread_mutex_unlock(&g_repl_rdma_send_lock);
    return rc;
}
#endif

/* ---- 自适应 Pipeline 深度调节 ----
 * 根据当前 in_flight 数量动态调整 send_pipeline_depth。
 * 目标：保持 pipeline 深度与传输速率匹配，避免过度 in_flight 导致 OOO。
 * 在 CQ 轮询线程中定期调用。
 */
#define KVS_RDMA_PIPELINE_DEPTH_MIN 2

static void repl_rdma_adjust_pipeline_depth(void) {
    int in_flight = g_repl_rdma_ctx.send_slots_in_flight;
    int depth = g_repl_rdma_ctx.send_pipeline_depth;

    if (in_flight <= depth / 2 && depth < KVS_RDMA_PIPELINE_DEPTH) {
        /* pipeline 利用率低 → 增加深度 */
        g_repl_rdma_ctx.send_pipeline_depth = depth + 1;
#if KVS_REPL_DEBUG
        fprintf(stderr, "repl rdma: pipeline depth %d -> %d (in_flight=%d)\n",
            depth, depth + 1, in_flight);
#endif
    } else if (in_flight >= depth && depth > KVS_RDMA_PIPELINE_DEPTH_MIN) {
        /* pipeline 饱和 → 减少深度 */
        g_repl_rdma_ctx.send_pipeline_depth = depth - 1;
#if KVS_REPL_DEBUG
        fprintf(stderr, "repl rdma: pipeline depth %d -> %d (in_flight=%d)\n",
            depth, depth - 1, in_flight);
#endif
    }
}

/* ---- CQ completion 处理函数（CQ 轮询线程和 fallback 路径共用）---- */
static void repl_rdma_cq_process_wc(struct ibv_wc *wc, int *adapt_counter) {
    if (wc->status != IBV_WC_SUCCESS) {
        fprintf(stderr, "repl rdma: cq_poll error status=%d opcode=%d wr_id=0x%lx\n",
            wc->status, wc->opcode, (unsigned long)wc->wr_id);
        if (wc->opcode == IBV_WC_SEND) g_rdma_send_cq_error_count++;
        else g_rdma_recv_cq_error_count++;
        g_repl_rdma_ctx.connected = 0;
        return;
    }
    if (wc->opcode == IBV_WC_SEND) {
        if (wc->wr_id & KVS_RDMA_PIPELINE_WR_ID_FLAG) {
            int slot = (int)(wc->wr_id & ~KVS_RDMA_PIPELINE_WR_ID_FLAG);
            repl_rdma_release_send_slot(slot);
            if (adapt_counter && ++(*adapt_counter) >= 16) {
                repl_rdma_adjust_pipeline_depth();
                *adapt_counter = 0;
            }
        }
    } else if (wc->opcode == IBV_WC_RECV) {
        int slot = (wc->wr_id > 0 && wc->wr_id <= (uint64_t)g_repl_rdma_ctx.active_recv_slots)
            ? (int)(wc->wr_id - 1) : -1;
        if (slot >= 0 && slot < g_repl_rdma_ctx.active_recv_slots) {
            g_repl_rdma_ctx.recv_slots[slot].posted = 0;
            repl_rdma_pending_recv_push(slot, (size_t)wc->byte_len);
        }
    }
}

/* ---- CQ 轮询线程 ----
 * 后台独立 poll CQ，将完成事件分发到对应队列。
 * 在 pipeline 模式下，发送/接收 completion 均由此线程异步处理。
 */
static void *repl_rdma_cq_poll_thread(void *arg) {
    (void)arg;
    struct ibv_cq *cq = g_repl_rdma_ctx.cq;
    struct ibv_wc wc_batch[KVS_RDMA_CQ_BATCH];
    struct ibv_cq *ev_cq;
    void *ev_ctx;
    int adapt_counter = 0;

    repl_rdma_log("cq_poll", "thread started");
    while (g_repl_rdma_ctx.cq_poll_thread_running && g_repl_rdma_ctx.connected) {
        /* 使用 completion channel 事件驱动，避免 busy poll */
        if (!g_repl_rdma_ctx.comp_chan) break;
        if (ibv_get_cq_event(g_repl_rdma_ctx.comp_chan, &ev_cq, &ev_ctx) != 0) {
            if (errno == EINTR) continue;
            repl_rdma_log("cq_poll", "ibv_get_cq_event failed");
            usleep(10000);
            continue;
        }
        ibv_ack_cq_events(cq, 1);

        /* 批量 poll completions */
        for (;;) {
            int n = ibv_poll_cq(cq, KVS_RDMA_CQ_BATCH, wc_batch);
            if (n <= 0) break;
            for (int i = 0; i < n; i++) {
                repl_rdma_cq_process_wc(&wc_batch[i], &adapt_counter);
                if (!g_repl_rdma_ctx.connected) break;
            }
            if (!g_repl_rdma_ctx.connected) break;
        }

        /* 标准 RDMA 编程模式: re-arm → poll(再确认) → wait.
         *
         * 必须先 re-arm 再 poll，因为:
         * 如果先 poll(全空) 再 re-arm，中间有 completion 到达时
         * 该 completion 不会触发 event，导致永久阻塞。
         */
        if (g_repl_rdma_ctx.cq && g_repl_rdma_ctx.comp_chan && g_repl_rdma_ctx.connected) {
            ibv_req_notify_cq(cq, 0);
            /*  Drain: poll 一次确认 poll→re-arm 之间没有漏掉 completion */
            int n = ibv_poll_cq(cq, KVS_RDMA_CQ_BATCH, wc_batch);
            if (n > 0) {
                for (int i = 0; i < n; i++) {
                    repl_rdma_cq_process_wc(&wc_batch[i], &adapt_counter);
                    if (!g_repl_rdma_ctx.connected) break;
                }
                /* 有数据，不阻塞等待，立即继续 re-arm→drain 循环 */
                continue;
            }
        }
        /* 无更多 completion → 回到 ibv_get_cq_event 阻塞等待 */
    }
    /* Drain remaining events on exit */
    if (g_repl_rdma_ctx.comp_chan) {
        struct ibv_cq *drain_cq;
        void *drain_ctx;
        while (ibv_get_cq_event(g_repl_rdma_ctx.comp_chan, &drain_cq, &drain_ctx) == 0) {
            ibv_ack_cq_events(cq, 1);
        }
    }
    g_cq_poll_thread_exited = 1;
    repl_rdma_log("cq_poll", "thread exiting");
    return NULL;
}

static int repl_rdma_start_cq_poll_thread(void) {
    if (g_repl_rdma_ctx.cq_poll_thread_running) return 0;
    if (!g_repl_rdma_ctx.comp_chan || !g_repl_rdma_ctx.cq) return -1;
    /* 先 arm CQ notification */
    if (ibv_req_notify_cq(g_repl_rdma_ctx.cq, 0) != 0) {
        repl_rdma_log("cq_poll", "ibv_req_notify_cq failed");
        return -1;
    }
    g_repl_rdma_ctx.cq_poll_thread_running = 1;
    if (pthread_create(&g_repl_rdma_ctx.cq_poll_thread, NULL,
                       repl_rdma_cq_poll_thread, NULL) != 0) {
        g_repl_rdma_ctx.cq_poll_thread_running = 0;
        repl_rdma_log("cq_poll", "pthread_create failed");
        return -1;
    }
    pthread_detach(g_repl_rdma_ctx.cq_poll_thread);
    repl_rdma_log("cq_poll", "thread started");
    return 0;
}


static void repl_rdma_stop_cq_poll_thread(void) {
static volatile int g_cq_poll_thread_exited = 0;
    if (!g_repl_rdma_ctx.cq_poll_thread_running) return;
    g_repl_rdma_ctx.cq_poll_thread_running = 0;
    g_cq_poll_thread_exited = 0;
    /* 断开 completion channel 以唤醒线程 */
    if (g_repl_rdma_ctx.comp_chan) {
        ibv_destroy_comp_channel(g_repl_rdma_ctx.comp_chan);
        g_repl_rdma_ctx.comp_chan = NULL;
    }
    /* 等待线程退出（最多 3 秒）*/
    for (int i = 0; i < 300 && !g_cq_poll_thread_exited; i++) {
        usleep(10000);
    }
    repl_rdma_log("cq_poll", "thread stop signalled");
}

static void repl_transport_tcp_disconnect_slave(int fd) {
    if (fd >= 0) close(fd);
}

static int repl_transport_ebpf_connect_slave(const char *host, int port) {
    int fd = repl_transport_tcp_connect_slave(host, port);
    if (fd < 0) return -1;
    if (repl_ebpf_register_fd(fd, 0) != 0) {
        fprintf(stderr, "repl ebpf: fd registration failed on slave link, using tcp-compatible path\n");
    }
    return fd;
}

static void repl_transport_ebpf_disconnect_slave(int fd) {
    repl_ebpf_unregister_fd(fd);
    repl_transport_tcp_disconnect_slave(fd);
}

static int repl_transport_rdma_send(conn_t *c, const unsigned char *buf, size_t len) {
    transport_log("RDMA send %zu bytes", len);
    (void)c;
    (void)buf;
    (void)len;
#if KVS_ENABLE_RDMA
    if (g_repl_rdma_ctx.connected) {
        return repl_rdma_try_send(buf, len);
    }
#endif
    return -1;
}

static const repl_transport_ops_t *repl_transport_ops(void);
static const repl_transport_ops_t *repl_transport_ops_for_conn(conn_t *c);
static int repl_should_use_rdma_now(void);
static void repl_transport_mark_active(const char *name);
static void repl_transport_trigger_fallback(const char *reason, int cooldown_ms);
static int repl_realtime_should_use_ebpf(void);

static int repl_transport_rdma_connect_slave(const char *host, int port) {
    (void)host;
    (void)port;
#if KVS_ENABLE_RDMA
    struct sockaddr_in dst;
    int rdma_port = g_cfg.rdma_port > 0 ? g_cfg.rdma_port : port + 1;
    repl_rdma_log("connect_slave", "begin");
    if (repl_rdma_prepare_addr(host, rdma_port, &dst) != 0) {
        repl_rdma_log("prepare_addr", "failed");
        repl_transport_trigger_fallback("rdma_prepare_addr_failed", 5000);
        return -1;
    }
    repl_rdma_log("prepare_addr", "ok");
    repl_rdma_reset_ctx();
    /* Retry event channel creation: transient failures can occur under load */
    for (int ec_retry = 0; ec_retry < 3; ec_retry++) {
        g_repl_rdma_ctx.ec = rdma_create_event_channel();
        if (g_repl_rdma_ctx.ec) break;
        char ec_err[128];
        snprintf(ec_err, sizeof(ec_err), "create failed errno=%d retry=%d", errno, ec_retry);
        repl_rdma_log("event_channel", ec_err);
        if (ec_retry < 2) usleep(200000);
    }
    if (!g_repl_rdma_ctx.ec) {
        repl_rdma_log("event_channel", "create failed after retries");
        return -1;
    }
    repl_rdma_log("event_channel", "created");
    if (rdma_create_id(g_repl_rdma_ctx.ec, &g_repl_rdma_ctx.id, NULL, RDMA_PS_TCP) != 0) {
        repl_rdma_log("create_id", "failed");
        repl_rdma_reset_ctx();
        return -1;
    }
    repl_rdma_log("create_id", "ok");
    if (rdma_resolve_addr(g_repl_rdma_ctx.id, NULL, (struct sockaddr *)&dst, 1000) != 0) {
        repl_rdma_log("resolve_addr", "rdma_resolve_addr failed");
        repl_transport_trigger_fallback("rdma_resolve_addr_failed", 5000);
        repl_rdma_reset_ctx();
        return -1;
    }
    repl_rdma_log("resolve_addr", "issued");
    if (repl_rdma_wait_event(RDMA_CM_EVENT_ADDR_RESOLVED, 1500) != 0) {
        repl_rdma_reset_ctx();
        return -1;
    }
    g_repl_rdma_ctx.addr_resolved = 1;
    repl_rdma_log("resolve_addr", "resolved");
    if (rdma_resolve_route(g_repl_rdma_ctx.id, 1000) != 0) {
        repl_rdma_log("resolve_route", "rdma_resolve_route failed");
        repl_transport_trigger_fallback("rdma_resolve_route_failed", 5000);
        repl_rdma_reset_ctx();
        return -1;
    }
    repl_rdma_log("resolve_route", "issued");
    if (repl_rdma_wait_event(RDMA_CM_EVENT_ROUTE_RESOLVED, 1500) != 0) {
        repl_rdma_reset_ctx();
        return -1;
    }
    g_repl_rdma_ctx.route_resolved = 1;
    repl_rdma_log("resolve_route", "resolved");
    if (!g_repl_rdma_ctx.comp_chan && g_repl_rdma_ctx.id && g_repl_rdma_ctx.id->verbs) {
        g_repl_rdma_ctx.comp_chan = ibv_create_comp_channel(g_repl_rdma_ctx.id->verbs);
        if (!g_repl_rdma_ctx.comp_chan) {
            repl_rdma_log("comp_channel", "create failed");
            repl_rdma_reset_ctx();
            return -1;
        }
        repl_rdma_log("comp_channel", "created");
    }
    if (!g_repl_rdma_ctx.pd && g_repl_rdma_ctx.id && g_repl_rdma_ctx.id->verbs) {
        g_repl_rdma_ctx.pd = ibv_alloc_pd(g_repl_rdma_ctx.id->verbs);
        if (!g_repl_rdma_ctx.pd) {
            repl_rdma_log("alloc_pd", "failed");
            repl_rdma_reset_ctx();
            return -1;
        }
        repl_rdma_log("alloc_pd", "ok");
    }
    if (!g_repl_rdma_ctx.cq && g_repl_rdma_ctx.id && g_repl_rdma_ctx.id->verbs) {
        repl_rdma_refresh_runtime_cfg();
        g_repl_rdma_ctx.cq = ibv_create_cq(g_repl_rdma_ctx.id->verbs, g_repl_rdma_ctx.active_qp_wr_depth, NULL, g_repl_rdma_ctx.comp_chan, 0);
        if (!g_repl_rdma_ctx.cq) {
            repl_rdma_log("create_cq", "failed");
            repl_rdma_reset_ctx();
            return -1;
        }
        repl_rdma_log("create_cq", "ok");
    }
    if (repl_rdma_create_qp() != 0) {
        repl_rdma_reset_ctx();
        return -1;
    }
    if (repl_rdma_prepare_buffers() != 0) {
        repl_rdma_reset_ctx();
        return -1;
    }
    if (repl_rdma_post_initial_recv() != 0) {
        repl_rdma_reset_ctx();
        return -1;
    }
    if (repl_rdma_connect_handshake() != 0) {
        repl_transport_trigger_fallback("rdma_connect_handshake_failed", 5000);
        repl_rdma_reset_ctx();
        return -1;
    }
    /* 启动 CQ 轮询线程（pipeline 模式：异步处理 send/recv completion） */
    if (g_repl_rdma_ctx.send_pipeline_enabled) {
        repl_rdma_start_cq_poll_thread();
    }
    repl_transport_mark_active("rdma");
    repl_rdma_log("connect_slave", "path complete but transport still experimental");
    return 1;
#endif
    return -1;
}

static void repl_transport_rdma_disconnect_slave(int fd) {
    (void)fd;
#if KVS_ENABLE_RDMA
    repl_rdma_reset_ctx();
#endif
}

static const repl_transport_ops_t g_repl_transport_tcp_ops = {
    .name = "tcp",
    .supported = 1,
    .send = repl_transport_tcp_send,
    .connect_slave = repl_transport_tcp_connect_slave,
    .disconnect_slave = repl_transport_tcp_disconnect_slave,
};

static const repl_transport_ops_t g_repl_transport_ebpf_ops = {
    .name = "ebpf",
    .supported = 1,
    .send = repl_transport_ebpf_send,
    .connect_slave = repl_transport_ebpf_connect_slave,
    .disconnect_slave = repl_transport_ebpf_disconnect_slave,
};

static const repl_transport_ops_t g_repl_transport_rdma_ops = {
    .name = "rdma",
    .supported = KVS_ENABLE_RDMA,
    .send = repl_transport_rdma_send,
    .connect_slave = repl_transport_rdma_connect_slave,
    .disconnect_slave = repl_transport_rdma_disconnect_slave,
};

/* kprobe+RDMA WRITE transport ops — kprobe 透明拦截 TCP send，
 * 用户态转发模块通过 RDMA WRITE 发送。send() 不应被直接调用。 */
/* 后台连接线程参数 */
typedef struct kprobe_mr_connect_arg_s {
    char host[64];
    int port;
    int tcp_fd;
} kprobe_mr_connect_arg_t;

static void *kprobe_mr_connect_thread(void *arg) {
    kprobe_mr_connect_arg_t *a = (kprobe_mr_connect_arg_t *)arg;
    fprintf(stderr, "kprobe rdma: [DBG] MR connect thread started for %s:%d\n",
        a->host, a->port);
    int rc = repl_kprobe_rdma_connect_mr(a->host, a->port, a->tcp_fd);
    fprintf(stderr, "kprobe rdma: [DBG] MR connect thread DONE rc=%d (0=OK)\n", rc);
    free(a);
    return NULL;
}

/* kprobe+RDMA WRITE transport ops
 * send() 返回 -1 触发 TCP fallback，kprobe 在内核态透明拦截。
 * 首次调用时在后台线程连接 slave 的 kprobe-rdma listener。 */
static int repl_transport_kprobe_rdma_send(conn_t *c, const unsigned char *buf, size_t len) {
    (void)buf; (void)len;
    /* 只对有效的 TCP socket fd 发起连接，跳过 stdin/stdout/stderr 等非 socket fd */
    if (!c || c->fd <= 2 || !KVS_ENABLE_KPROBE_RDMA) return 0;
    static volatile int mr_connect_started = 0;
    if (!mr_connect_started) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        if (getpeername(c->fd, (struct sockaddr *)&peer, &peer_len) == 0) {
            mr_connect_started = 1;
            char host[64];
            inet_ntop(AF_INET, &peer.sin_addr, host, sizeof(host));
            kprobe_mr_connect_arg_t *a = (kprobe_mr_connect_arg_t *)malloc(sizeof(*a));
            if (a) {
                snprintf(a->host, sizeof(a->host), "%s", host);
                a->port = (int)g_cfg.port;
                a->tcp_fd = dup(c->fd);
                pthread_t tid;
                if (pthread_create(&tid, NULL, kprobe_mr_connect_thread, a) == 0)
                    pthread_detach(tid);
                else {
                    close(a->tcp_fd);
                    free(a);
                }
            }
        }
    }
    /* 返回 -1，让 reactor 通过 TCP 发送（BPF kprobe 在内核拦截后经 ringbuf
     * 触发回调做 RDMA WRITE，此处只做 TCP 保底） */
    return -1;
}

static int repl_transport_kprobe_rdma_connect_slave(const char *host, int port) {
#if KVS_ENABLE_KPROBE_RDMA
    return repl_kprobe_rdma_establish(host, port);
#else
    (void)host; (void)port;
    return -1;
#endif
}

static void repl_transport_kprobe_rdma_disconnect_slave(int fd) {
    (void)fd;
#if KVS_ENABLE_KPROBE_RDMA
    repl_kprobe_rdma_cleanup();
#endif
}

static const repl_transport_ops_t g_repl_transport_kprobe_rdma_ops = {
    .name = "kprobe-rdma",
    .supported = KVS_ENABLE_KPROBE_RDMA,
    .send = repl_transport_kprobe_rdma_send,
    .connect_slave = repl_transport_kprobe_rdma_connect_slave,
    .disconnect_slave = repl_transport_kprobe_rdma_disconnect_slave,
};

static const char *repl_rdma_state_name(repl_rdma_state_t st) {
    switch (st) {
        case REPL_RDMA_STATE_INIT: return "INIT";
        case REPL_RDMA_STATE_CONNECTING: return "CONNECTING";
        case REPL_RDMA_STATE_ESTABLISHED: return "ESTABLISHED";
        case REPL_RDMA_STATE_SYNCING: return "SYNCING";
        case REPL_RDMA_STATE_STEADY: return "STEADY";
        case REPL_RDMA_STATE_BACKOFF: return "BACKOFF";
        case REPL_RDMA_STATE_FAILED: return "FAILED";
        case REPL_RDMA_STATE_FALLBACK_TCP: return "FALLBACK_TCP";
        default: return "UNKNOWN";
    }
}

static void repl_rdma_set_state(repl_rdma_state_t st, const char *reason) {
#if KVS_ENABLE_RDMA
    if (g_repl_rdma_ctx.state != st) {
        fprintf(stderr, "repl rdma: state transition %s -> %s%s%s\n",
            repl_rdma_state_name(g_repl_rdma_ctx.state), repl_rdma_state_name(st), reason ? " reason=" : "", reason ? reason : "");
    }
#endif
    g_repl_rdma_ctx.state = st;
}

static void repl_transport_mark_active(const char *name) {
    snprintf(g_repl_transport_active, sizeof(g_repl_transport_active), "%s", (name && *name) ? name : "tcp");
}

static void repl_transport_trigger_fallback(const char *reason, int cooldown_ms) {
    g_repl_transport_fallback_count++;
    snprintf(g_repl_transport_fallback_reason, sizeof(g_repl_transport_fallback_reason), "%s", reason ? reason : "transport_failure");
    g_repl_transport_fallback_until_ms = kvs_now_ms() + (cooldown_ms > 0 ? cooldown_ms : 5000);
    repl_transport_mark_active("tcp");
#if KVS_ENABLE_RDMA
    repl_rdma_set_state(REPL_RDMA_STATE_FALLBACK_TCP, g_repl_transport_fallback_reason);
#endif
}

const char *repl_transport_configured_name(void) {
    int use_rdma_fullsync = !strcasecmp(repl_fullsync_transport_name(), "rdma");
    int use_kprobe_realtime = !strcasecmp(repl_realtime_transport_name(), "kprobe-rdma");
    int use_ebpf_realtime = repl_realtime_should_use_ebpf();
    if (use_rdma_fullsync && use_kprobe_realtime) return "rdma+kprobe";
    if (use_rdma_fullsync && use_ebpf_realtime) return "rdma+ebpf";
    if (!strcasecmp(g_cfg.repl_transport_backend, "rdma")) return "rdma";
    if (!strcasecmp(g_cfg.repl_transport_backend, "ebpf") || !strcasecmp(g_cfg.repl_transport_backend, "sockmap")) return "ebpf";
    if (!strcasecmp(g_cfg.repl_transport_backend, "kprobe-rdma")) return "kprobe-rdma";
    return "tcp";
}

const char *repl_transport_active_name(void) {
    int use_rdma_fullsync = !strcasecmp(repl_fullsync_transport_name(), "rdma");
    int use_kprobe_realtime = !strcasecmp(repl_realtime_transport_name(), "kprobe-rdma");
    int use_ebpf_realtime = repl_realtime_should_use_ebpf();
    if (use_rdma_fullsync && use_kprobe_realtime) return "rdma+kprobe";
    if (use_rdma_fullsync && use_ebpf_realtime) return "rdma+ebpf";
    return g_repl_transport_active[0] ? g_repl_transport_active : repl_transport_configured_name();
}

const char *repl_transport_fallback_reason(void) {
    return g_repl_transport_fallback_reason;
}

unsigned long long repl_transport_fallback_count(void) {
    return g_repl_transport_fallback_count;
}

long long repl_transport_fallback_until_ms(void) {
    return g_repl_transport_fallback_until_ms;
}

static const repl_transport_ops_t *repl_transport_ops_for_conn(conn_t *c) {
    if (!c) {
        if (g_slave_transport_kind == KVS_REPL_TRANSPORT_RDMA) return &g_repl_transport_rdma_ops;
        if (g_slave_transport_kind == KVS_REPL_TRANSPORT_EBPF) return &g_repl_transport_ebpf_ops;
        if (g_slave_transport_kind == KVS_REPL_TRANSPORT_KPROBE_RDMA) return &g_repl_transport_kprobe_rdma_ops;
        return &g_repl_transport_tcp_ops;
    }
    if (c->repl_transport_kind == KVS_REPL_TRANSPORT_RDMA) return &g_repl_transport_rdma_ops;
    if (c->repl_transport_kind == KVS_REPL_TRANSPORT_EBPF) return &g_repl_transport_ebpf_ops;
    if (c->repl_transport_kind == KVS_REPL_TRANSPORT_KPROBE_RDMA) return &g_repl_transport_kprobe_rdma_ops;
    return &g_repl_transport_tcp_ops;
}

static int repl_should_use_rdma_now(void) {
    if (strcasecmp(repl_fullsync_transport_name(), "rdma") != 0 && strcasecmp(g_cfg.repl_transport_backend, "rdma") != 0) return 0;
    if (!g_repl_transport_rdma_ops.supported) return 0;
    if (g_repl_transport_fallback_until_ms > kvs_now_ms()) return 0;
    return 1;
}

static int repl_should_use_ebpf_now(void) {
    const char *t = repl_realtime_transport_name();
    if (strcasecmp(t, "ebpf") != 0 && strcasecmp(t, "sockmap") != 0
        && strcasecmp(g_cfg.repl_transport_backend, "ebpf") != 0 && strcasecmp(g_cfg.repl_transport_backend, "sockmap") != 0) return 0;
    if (!g_repl_transport_ebpf_ops.supported || !repl_ebpf_supported()) return 0;
    if (g_repl_transport_fallback_until_ms > kvs_now_ms()) return 0;
    return 1;
}

static const repl_transport_ops_t *repl_transport_ops(void) {
    /* In hybrid mode, the main ops are for realtime transport */
    int use_rdma_fullsync = !strcasecmp(repl_fullsync_transport_name(), "rdma");
    int use_ebpf_realtime = repl_realtime_should_use_ebpf();
    if (use_rdma_fullsync && use_ebpf_realtime) {
        /* Hybrid: RDMA for fullsync, eBPF for realtime.
         * The main transport is eBPF (over TCP) for realtime data. */
        if (g_repl_transport_ebpf_ops.supported && repl_ebpf_supported())
            return &g_repl_transport_ebpf_ops;
        return &g_repl_transport_tcp_ops;
    }
    if (repl_should_use_rdma_now()) return &g_repl_transport_rdma_ops;
    if (repl_should_use_ebpf_now()) return &g_repl_transport_ebpf_ops;
    return &g_repl_transport_tcp_ops;
}

static int repl_transport_supported(void) {
    if (!strcasecmp(g_cfg.repl_transport_backend, "rdma")) return g_repl_transport_rdma_ops.supported;
    if (!strcasecmp(g_cfg.repl_transport_backend, "kprobe-rdma")) return g_repl_transport_kprobe_rdma_ops.supported;
    if (!strcasecmp(g_cfg.repl_transport_backend, "ebpf") || !strcasecmp(g_cfg.repl_transport_backend, "sockmap")) return g_repl_transport_ebpf_ops.supported && repl_ebpf_supported();
    return 1;
}

const char *repl_transport_name(void) {
    int use_rdma_fullsync = !strcasecmp(repl_fullsync_transport_name(), "rdma");
    int use_kprobe_realtime = !strcasecmp(repl_realtime_transport_name(), "kprobe-rdma");
    int use_ebpf_realtime = repl_realtime_should_use_ebpf();
    if (use_rdma_fullsync && use_kprobe_realtime) return "rdma+kprobe";
    if (use_rdma_fullsync && use_ebpf_realtime) return "rdma+ebpf";
    return repl_transport_active_name();
}

int repl_transport_send(conn_t *c, const unsigned char *buf, size_t len) {
    const repl_transport_ops_t *ops = repl_transport_ops_for_conn(c);
    int rc = ops->send(c, buf, len);
    if (rc == 0) {
        repl_transport_mark_active(ops->name);
        return 0;
    }
    if (ops == &g_repl_transport_rdma_ops) {
        repl_transport_trigger_fallback("rdma_send_failure", 5000);
    } else if (ops == &g_repl_transport_ebpf_ops) {
        repl_transport_trigger_fallback("ebpf_send_failure", 5000);
    } else if (ops == &g_repl_transport_kprobe_rdma_ops) {
        /* kprobe-rdma send 正常情况下不直接调用；若被调用，不做 fallback */
    }
    return rc;
}

int repl_transport_send_many(conn_t *c, const unsigned char *buf1, size_t len1, const unsigned char *buf2, size_t len2) {
    if (repl_transport_send(c, buf1, len1) != 0) return -1;
    if (repl_transport_send(c, buf2, len2) != 0) return -1;
    return 0;
}

/* ---- Dual-transport: fullsync vs realtime ---- */

const char *repl_fullsync_transport_name(void) {
    /* Use dedicated config if set, otherwise fall back to repl_transport_backend */
    if (g_cfg.repl_fullsync_transport[0]) return g_cfg.repl_fullsync_transport;
    if (!strcasecmp(g_cfg.repl_transport_backend, "rdma")) return "rdma";
    return "tcp";
}

const char *repl_realtime_transport_name(void) {
    if (g_cfg.repl_realtime_transport[0]) return g_cfg.repl_realtime_transport;
    if (!strcasecmp(g_cfg.repl_transport_backend, "ebpf") || !strcasecmp(g_cfg.repl_transport_backend, "sockmap")) return "ebpf";
    if (!strcasecmp(g_cfg.repl_transport_backend, "kprobe-rdma")) return "kprobe-rdma";
    return "tcp";
}

static const repl_transport_ops_t *repl_transport_ops_for_context(int send_ctx) {
    if (send_ctx == KVS_REPL_SEND_FULLSYNC) {
        const char *t = repl_fullsync_transport_name();
        if (!strcasecmp(t, "rdma") && g_repl_transport_rdma_ops.supported && g_repl_transport_fallback_until_ms <= kvs_now_ms()) {
            transport_log("fullsync using RDMA");
            return &g_repl_transport_rdma_ops;
        }
        transport_log("fullsync using TCP");
        return &g_repl_transport_tcp_ops;
    }
    /* KVS_REPL_SEND_REALTIME */
    const char *t = repl_realtime_transport_name();
    if (!strcasecmp(t, "kprobe-rdma") && g_repl_transport_kprobe_rdma_ops.supported && g_cfg.kprobe_enabled) {
        transport_log("realtime using KPROBE+RDMA");
        return &g_repl_transport_kprobe_rdma_ops;
    }
    if ((!strcasecmp(t, "ebpf") || !strcasecmp(t, "sockmap")) && g_repl_transport_ebpf_ops.supported && repl_ebpf_supported()) {
        transport_log("realtime using EBPF");
        return &g_repl_transport_ebpf_ops;
    }
    transport_log("realtime using TCP");
    return &g_repl_transport_tcp_ops;
}

int repl_fullsync_send(conn_t *c, const unsigned char *buf, size_t len) {
    const repl_transport_ops_t *ops = repl_transport_ops_for_context(KVS_REPL_SEND_FULLSYNC);
    int rc = ops->send(c, buf, len);
    if (rc == 0) {
        return 0;
    }
    /* Fallback: try TCP on failure, and disable RDMA retry for this session */
    transport_log("%s failed, fallback to TCP", ops->name);
    repl_transport_trigger_fallback("fullsync_send_fail", 3600000); /* 1h cooldown */
    rc = repl_transport_tcp_send(c, buf, len);
    if (rc == 0) return 0;
    return -1;
}

int repl_realtime_send(conn_t *c, const unsigned char *buf, size_t len) {
    const repl_transport_ops_t *ops = repl_transport_ops_for_context(KVS_REPL_SEND_REALTIME);
    int rc = ops->send(c, buf, len);
    if (rc == 0) {
        return 0;
    }
    /* Fallback: TCP send — 同时为 kprobe 提供抓取数据源 */
    rc = repl_transport_tcp_send(c, buf, len);
    return rc;
}

static int repl_realtime_should_use_ebpf(void) {
    const char *t = repl_realtime_transport_name();
    return !strcasecmp(t, "ebpf") || !strcasecmp(t, "sockmap");
}

static void build_slave_state_path(void) {
    if (g_slave_state_path[0]) return;
    snprintf(g_slave_state_path, sizeof(g_slave_state_path), "%s.replstate", g_cfg.aof_path);
}


void repl_note_fullsync(size_t snapshot_bytes) {
    g_repl_fullsync_count++;
    g_repl_snapshot_bytes += (unsigned long long)snapshot_bytes;
}

static int ensure_repl_backlog(void) {
    if (g_repl_backlog.buf) return 0;
    g_repl_backlog.buf = (unsigned char *)kvs_malloc(KVS_REPL_BACKLOG_SIZE);
    if (!g_repl_backlog.buf) return -1;
    g_repl_backlog.cap = KVS_REPL_BACKLOG_SIZE;
    g_repl_backlog.histlen = 0;
    g_repl_backlog.head = 0;
    g_repl_backlog.start_offset = g_master_repl_offset;
    g_repl_backlog.end_offset = g_master_repl_offset;
    return 0;
}

int repl_backlog_feed(const unsigned char *buf, size_t len) {
    if (!buf || len == 0) return 0;
    if (ensure_repl_backlog() != 0) return -1;
    if (len >= g_repl_backlog.cap) {
        buf += len - g_repl_backlog.cap;
        len = g_repl_backlog.cap;
        memcpy(g_repl_backlog.buf, buf, len);
        g_repl_backlog.head = 0;
        g_repl_backlog.histlen = len;
        g_repl_backlog.end_offset += len;
        g_repl_backlog.start_offset = g_repl_backlog.end_offset - g_repl_backlog.histlen;
        return 0;
    }

    size_t tail = (g_repl_backlog.head + g_repl_backlog.histlen) % g_repl_backlog.cap;
    size_t first = g_repl_backlog.cap - tail;
    if (first > len) first = len;
    memcpy(g_repl_backlog.buf + tail, buf, first);
    if (len > first) memcpy(g_repl_backlog.buf, buf + first, len - first);

    if (g_repl_backlog.histlen + len <= g_repl_backlog.cap) {
        g_repl_backlog.histlen += len;
    } else {
        size_t overflow = g_repl_backlog.histlen + len - g_repl_backlog.cap;
        g_repl_backlog.head = (g_repl_backlog.head + overflow) % g_repl_backlog.cap;
        g_repl_backlog.histlen = g_repl_backlog.cap;
    }

    g_repl_backlog.end_offset += len;
    g_repl_backlog.start_offset = g_repl_backlog.end_offset - g_repl_backlog.histlen;
    return 0;
}

void repl_note_broadcast(size_t bytes) {
    g_repl_broadcast_bytes += (unsigned long long)bytes;
    g_master_repl_offset += (unsigned long long)bytes;
}

static void ensure_master_replid(void) {
    unsigned int a, b, c, d, e;
    if (g_master_replid[0]) return;
    a = (unsigned int)(kvs_now_ms() & 0xffffffffu);
    b = (unsigned int)getpid();
    c = (unsigned int)((uintptr_t)&g_master_replid & 0xffffffffu);
    d = (unsigned int)(time(NULL) & 0xffffffffu);
    e = (unsigned int)((uintptr_t)pthread_self() & 0xffffffffu);
    snprintf(g_master_replid, sizeof(g_master_replid), "%08x%08x%08x%08x%08x", a, b, c, d, e);
    g_master_replid[40] = '\0';
}

static void repl_set_link_state(int up) {
    int old;
    pthread_mutex_lock(&g_slave_conf_lock);
    old = g_master_link_up;
    g_master_link_up = up;
    if (up) g_master_last_io_ms = kvs_now_ms();
    pthread_mutex_unlock(&g_slave_conf_lock);
#if KVS_ENABLE_RDMA
    if (!strcasecmp(repl_transport_name(), "rdma") && old != up) {
        fprintf(stderr, "repl rdma: link_state transition %s -> %s\n", old ? "up" : "down", up ? "up" : "down");
    }
#endif
}

int repl_slaveof(const char *host, int port) {
    if (!host || port <= 0) return -1;
    pthread_mutex_lock(&g_slave_conf_lock);
    snprintf(g_slave_host, sizeof(g_slave_host), "%s", host);
    g_slave_port = port;
    g_cfg.role = ROLE_SLAVE;
    snprintf(g_cfg.master_host, sizeof(g_cfg.master_host), "%s", host);
    g_cfg.master_port = port;
    g_slave_conf_gen++;
    pthread_mutex_unlock(&g_slave_conf_lock);
    repl_set_link_state(0);
    repl_slave_state_save();
    return 0;
}

int repl_slaveof_noone(void) {
    pthread_mutex_lock(&g_slave_conf_lock);
    g_slave_host[0] = '\0';
    g_slave_port = 0;
    g_cfg.role = ROLE_MASTER;
    g_cfg.master_host[0] = '\0';
    g_cfg.master_port = 0;
    g_slave_conf_gen++;
    pthread_mutex_unlock(&g_slave_conf_lock);
    repl_set_link_state(0);
    repl_slave_state_save();
    return 0;
}

int repl_get_master_addr(char *host, size_t cap, int *port) {
    if (!host || cap == 0 || !port) return -1;
    pthread_mutex_lock(&g_slave_conf_lock);
    snprintf(host, cap, "%s", g_slave_host);
    *port = g_slave_port;
    pthread_mutex_unlock(&g_slave_conf_lock);
    return 0;
}

int repl_is_master_link_up(void) {
    int up;
    pthread_mutex_lock(&g_slave_conf_lock);
    up = g_master_link_up;
    pthread_mutex_unlock(&g_slave_conf_lock);
    return up;
}

const char *repl_master_link_state_name(void) {
    return repl_is_master_link_up() ? "up" : "down";
}

const char *repl_master_id(void) {
    ensure_master_replid();
    return g_master_replid;
}

unsigned long long repl_master_offset(void) {
    return g_master_repl_offset;
}

unsigned long long repl_connected_slaves(void) {
    unsigned long long n = 0;
    pthread_mutex_lock(&g_repl_lock);
    for (conn_t *c = g_replicas; c; c = c->next_replica) n++;
    pthread_mutex_unlock(&g_repl_lock);
    return n;
}

unsigned long long repl_fullsync_count(void) {
    return g_repl_fullsync_count;
}

unsigned long long repl_partialsync_ok_count(void) {
    return g_repl_partialsync_ok_count;
}

unsigned long long repl_partialsync_err_count(void) {
    return g_repl_partialsync_err_count;
}

unsigned long long repl_broadcast_bytes(void) {
    return g_repl_broadcast_bytes;
}

unsigned long long repl_snapshot_bytes(void) {
    return g_repl_snapshot_bytes;
}

unsigned long long repl_backlog_size(void) {
    return (unsigned long long)g_repl_backlog.cap;
}

unsigned long long repl_backlog_histlen(void) {
    return (unsigned long long)g_repl_backlog.histlen;
}

unsigned long long repl_backlog_start_offset(void) {
    return g_repl_backlog.start_offset;
}

unsigned long long repl_backlog_end_offset(void) {
    return g_repl_backlog.end_offset;
}

int repl_rdma_effective_recv_slots(void) {
#if KVS_ENABLE_RDMA
    return repl_rdma_cfg_recv_slots();
#else
    return 0;
#endif
}

int repl_rdma_effective_qp_wr_depth(void) {
#if KVS_ENABLE_RDMA
    return repl_rdma_cfg_qp_wr_depth();
#else
    return 0;
#endif
}

int repl_rdma_effective_chunk_size(void) {
#if KVS_ENABLE_RDMA
    return (int)repl_rdma_cfg_chunk_size();
#else
    return 0;
#endif
}

int repl_rdma_is_connected(void) {
#if KVS_ENABLE_RDMA
    return g_repl_rdma_ctx.connected;
#else
    return 0;
#endif
}

unsigned long long repl_rdma_disconnect_count(void) {
    return g_rdma_disconnect_count;
}

unsigned long long repl_rdma_reject_count(void) {
    return g_rdma_reject_count;
}

unsigned long long repl_rdma_send_cq_error_count(void) {
    return g_rdma_send_cq_error_count;
}

unsigned long long repl_rdma_recv_cq_error_count(void) {
    return g_rdma_recv_cq_error_count;
}

void repl_note_partialsync_result(int ok) {
    if (ok) g_repl_partialsync_ok_count++;
    else g_repl_partialsync_err_count++;
}

void repl_slave_set_sync_state(const char *replid, unsigned long long applied_offset, unsigned long long durable_offset, int fullsync_loading, unsigned long long fullsync_target_bytes) {
    if (replid && *replid) {
        snprintf(g_slave_master_replid, sizeof(g_slave_master_replid), "%s", replid);
    }
    g_slave_repl_applied_offset = applied_offset;
    g_slave_repl_durable_offset = durable_offset;
    g_slave_repl_offset = applied_offset;
    g_slave_loading_fullsync = fullsync_loading;
    g_slave_fullsync_target_bytes = fullsync_loading ? fullsync_target_bytes : 0;
    g_slave_fullsync_loaded_bytes = 0;
#if KVS_ENABLE_RDMA
    if (!strcasecmp(g_cfg.repl_fullsync_transport, "rdma") || !strcasecmp(g_cfg.repl_transport_backend, "rdma"))
        fprintf(stderr, "repl rdma: slave_sync_state - replid=%s applied_offset=%llu durable_offset=%llu fullsync_loading=%d target_bytes=%llu\n",
            g_slave_master_replid, g_slave_repl_applied_offset, g_slave_repl_durable_offset, g_slave_loading_fullsync, g_slave_fullsync_target_bytes);
#endif
    repl_slave_state_save();
}

void repl_slave_finish_fullsync(void) {
    g_slave_loading_fullsync = 0;
    g_slave_fullsync_target_bytes = 0;
    g_slave_fullsync_loaded_bytes = 0;
    if (g_slave_repl_durable_offset < g_slave_repl_applied_offset) {
        g_slave_repl_durable_offset = g_slave_repl_applied_offset;
    }
#if KVS_ENABLE_RDMA
    if (!strcasecmp(g_cfg.repl_fullsync_transport, "rdma") || !strcasecmp(g_cfg.repl_transport_backend, "rdma")) {
        fprintf(stderr, "repl rdma: slave_fullsync - finished applied_offset=%llu durable_offset=%llu\n", g_slave_repl_applied_offset, g_slave_repl_durable_offset);
        {
            extern kv_config_t g_cfg;
            extern kvs_hash_t global_hash;
            int cnt = 0;
            if (global_hash.nodes) {
                for (int i = 0; i < global_hash.max_slots; ++i) {
                    for (hashnode_t *node = global_hash.nodes[i]; node; node = node->next) {
                        cnt++;
                    }
                }
            }
            fprintf(stderr, "repl rdma: slave_debug - total_hash_entries=%d\n", cnt);
            char *v = kvs_hash_get(&global_hash, "pre:k:000000");
            fprintf(stderr, "repl rdma: slave_debug - HGET pre:k:000000 = %s\n", v ? v : "(null)");
        }
    }
#endif
    /* 全量同步完成后，将当前内存数据保存到 dump 文件
     * 格式与 kvs_dump_to_fd() 一致（二进制长度前缀格式）
     * 后续增量数据通过 AOF 持续写入 */
    int dump_fd = open(g_cfg.dump_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dump_fd >= 0) {
        if (kvs_dump_to_fd(dump_fd) == 0) {
            fprintf(stderr, "repl: slave fullsync dump saved to %s\n", g_cfg.dump_path);
        } else {
            fprintf(stderr, "repl: slave fullsync dump write failed\n");
        }
        close(dump_fd);
    } else {
        fprintf(stderr, "repl: slave fullsync dump open failed: %s\n", strerror(errno));
    }
    repl_slave_state_save();
    repl_slave_send_ack();
}

void repl_slave_note_applied(size_t rawlen) {
    if (!g_slave_loading_fullsync) {
        unsigned long long before = g_slave_repl_applied_offset;
        g_slave_repl_applied_offset += (unsigned long long)rawlen;
        g_slave_repl_offset = g_slave_repl_applied_offset;
#if KVS_ENABLE_RDMA
    if (!strcasecmp(g_cfg.repl_fullsync_transport, "rdma") || !strcasecmp(g_cfg.repl_transport_backend, "rdma"))
        fprintf(stderr, "repl rdma: slave_apply - applied_before=%llu rawlen=%zu applied_after=%llu durable=%llu\n",
            before, rawlen, g_slave_repl_applied_offset, g_slave_repl_durable_offset);
#endif
        repl_slave_state_save();
    } else {
        g_slave_fullsync_loaded_bytes += (unsigned long long)rawlen;
#if KVS_ENABLE_RDMA
        if (!strcasecmp(g_cfg.repl_fullsync_transport, "rdma") || !strcasecmp(g_cfg.repl_transport_backend, "rdma"))
            fprintf(stderr, "repl rdma: slave_apply - fullsync_chunk=%zu loaded=%llu target=%llu\n",
                rawlen, g_slave_fullsync_loaded_bytes, g_slave_fullsync_target_bytes);
#endif
        if (g_slave_fullsync_target_bytes > 0 && g_slave_fullsync_loaded_bytes >= g_slave_fullsync_target_bytes) {
            repl_slave_finish_fullsync();
        }
    }
}

void repl_slave_note_durable(size_t rawlen) {
    (void)rawlen;
    if (g_slave_loading_fullsync) {
        repl_slave_send_ack();
        return;
    }
    if (g_slave_repl_durable_offset < g_slave_repl_applied_offset) {
        g_slave_repl_durable_offset = g_slave_repl_applied_offset;
        repl_slave_state_save();
    }
#if KVS_ENABLE_RDMA
    if (!strcasecmp(g_cfg.repl_fullsync_transport, "rdma") || !strcasecmp(g_cfg.repl_transport_backend, "rdma"))
        fprintf(stderr, "repl rdma: slave_durable - applied=%llu durable=%llu\n",
            g_slave_repl_applied_offset, g_slave_repl_durable_offset);
#endif
    repl_slave_send_ack();
}

static int repl_transport_send_on_slave_link(const unsigned char *buf, size_t len) {
    if (!buf || len == 0) return 0;
    if (g_slave_transport_kind == KVS_REPL_TRANSPORT_RDMA) {
        return g_repl_transport_rdma_ops.send(NULL, buf, len);
    }
    return -1;
}

int repl_slave_send_ack(void) {
    unsigned char cmd[256];
    char applied[32];
    char durable[32];
    size_t n;
    long long now;
    if (g_cfg.role != ROLE_SLAVE) return 0;
    if (!repl_is_master_link_up()) return 0;
    /* Rate-limit: at most one REPLACK per second */
    now = kvs_now_ms();
    if (now - g_slave_last_ack_ms < 1000) return 0;
    snprintf(applied, sizeof(applied), "%llu", g_slave_repl_applied_offset);
    snprintf(durable, sizeof(durable), "%llu", g_slave_repl_durable_offset);
    n = resp_build_cmd3(cmd, sizeof(cmd), "REPLACK", applied, durable);
    if (g_slave_transport_kind == KVS_REPL_TRANSPORT_RDMA) {
        if (repl_transport_send_on_slave_link(cmd, n) == 0) {
            g_slave_last_ack_ms = kvs_now_ms();
            return 0;
        }
        return -1;
    }
    /* TCP / eBPF transport: send REPLACK over the slave fd */
    if (g_slave_fd >= 0) {
        ssize_t sent = send(g_slave_fd, cmd, n, 0);
        if (sent == (ssize_t)n) {
            g_slave_last_ack_ms = kvs_now_ms();
            return 0;
        }
    }
    return -1;
}

void repl_replica_update_ack(conn_t *c, unsigned long long applied_offset, unsigned long long durable_offset) {
    if (!c) return;
    c->repl_applied_offset_ack = applied_offset;
    c->repl_durable_offset_ack = durable_offset;
    c->repl_last_ack_ms = kvs_now_ms();
}

const char *repl_slave_master_id(void) {
    return g_slave_master_replid;
}

unsigned long long repl_slave_offset(void) {
    return g_slave_repl_applied_offset;
}

unsigned long long repl_slave_applied_offset(void) {
    return g_slave_repl_applied_offset;
}

unsigned long long repl_slave_durable_offset(void) {
    return g_slave_repl_durable_offset;
}

int repl_slave_loading_fullsync(void) {
    return g_slave_loading_fullsync;
}

int repl_slave_state_load(void) {
    FILE *fp;
    char replid[41] = {0};
    unsigned long long applied_offset = 0;
    unsigned long long durable_offset = 0;
    build_slave_state_path();
    fp = fopen(g_slave_state_path, "r");
    if (!fp) return 0;
    if (fscanf(fp, "%40s %llu %llu", replid, &applied_offset, &durable_offset) == 3) {
        snprintf(g_slave_master_replid, sizeof(g_slave_master_replid), "%s", replid);
        g_slave_repl_applied_offset = applied_offset;
        g_slave_repl_durable_offset = durable_offset;
        g_slave_repl_offset = applied_offset;
    } else {
        rewind(fp);
        if (fscanf(fp, "%40s %llu", replid, &applied_offset) == 2) {
            snprintf(g_slave_master_replid, sizeof(g_slave_master_replid), "%s", replid);
            g_slave_repl_applied_offset = applied_offset;
            g_slave_repl_durable_offset = applied_offset;
            g_slave_repl_offset = applied_offset;
        }
    }
    fclose(fp);
    return 0;
}

int repl_slave_state_save(void) {
    FILE *fp;
    build_slave_state_path();
    fp = fopen(g_slave_state_path, "w");
    if (!fp) return -1;
    fprintf(fp, "%s %llu %llu\n",
        g_slave_master_replid[0] ? g_slave_master_replid : "?",
        g_slave_repl_applied_offset,
        g_slave_repl_durable_offset);
    fclose(fp);
    return 0;
}

int repl_backlog_can_continue(const char *replid, unsigned long long offset) {
    unsigned long long want_offset;
    ensure_master_replid();
    if (!replid || strcmp(replid, g_master_replid) != 0) return 0;
    if (!g_repl_backlog.buf) return 0;
    want_offset = offset;
    if (want_offset > g_repl_backlog.end_offset) return 0;
    return want_offset >= g_repl_backlog.start_offset;
}

int repl_backlog_write_range(conn_t *c, unsigned long long offset) {
    size_t delta, start_index, first;
    if (!c || !g_repl_backlog.buf) return -1;
    if (offset < g_repl_backlog.start_offset || offset > g_repl_backlog.end_offset) return -1;
    delta = (size_t)(offset - g_repl_backlog.start_offset);
    start_index = (g_repl_backlog.head + delta) % g_repl_backlog.cap;
    first = g_repl_backlog.histlen - delta;
    if (first == 0) return 0;
    if (start_index + first <= g_repl_backlog.cap) {
        if (repl_send_chunked(c, g_repl_backlog.buf + start_index, first) != 0) return -1;
    } else {
        size_t part1 = g_repl_backlog.cap - start_index;
        size_t part2 = first - part1;
        if (repl_send_chunked(c, g_repl_backlog.buf + start_index, part1) != 0) return -1;
        if (repl_send_chunked(c, g_repl_backlog.buf, part2) != 0) return -1;
    }
    c->repl_offset_sent = g_repl_backlog.end_offset;
    c->repl_last_send_ms = kvs_now_ms();
    return 0;
}

int repl_backlog_send_continue(conn_t *c, unsigned long long offset) {
    char hdr[128];
    int hn;
    unsigned long long continue_offset;
    if (!c || !g_repl_backlog.buf) return -1;
    if (offset < g_repl_backlog.start_offset || offset > g_repl_backlog.end_offset) return -1;
    continue_offset = g_repl_backlog.end_offset;
    hn = snprintf(hdr, sizeof(hdr), "+CONTINUE %s %llu\r\n", repl_master_id(), continue_offset);
    repl_note_send_context("continue-header", (size_t)hn, offset, (unsigned char *)hdr);
    if (repl_send_chunked(c, (unsigned char *)hdr, (size_t)hn) != 0) return -1;
    repl_note_send_context("continue-backlog", (size_t)(g_repl_backlog.end_offset - offset), offset, g_repl_backlog.buf);
    return repl_backlog_write_range(c, offset);
}

static int snapshot_slave_conf(char *host, size_t cap, int *port, int *gen, int *role) {
    if (!host || !port || !gen || !role) return -1;
    pthread_mutex_lock(&g_slave_conf_lock);
    snprintf(host, cap, "%s", g_slave_host);
    *port = g_slave_port;
    *gen = g_slave_conf_gen;
    *role = g_cfg.role;
    pthread_mutex_unlock(&g_slave_conf_lock);
    return 0;
}

static int slave_should_reconnect(int local_gen) {
    int changed = 0;
    pthread_mutex_lock(&g_slave_conf_lock);
    if (g_cfg.role != ROLE_SLAVE || local_gen != g_slave_conf_gen) changed = 1;
    pthread_mutex_unlock(&g_slave_conf_lock);
    return changed;
}

static void repl_slave_retry_pause(int rdma_fail_streak) {
    const char *tname = repl_transport_name();
    if (!strcasecmp(tname, "rdma") || !strcasecmp(tname, "rdma+ebpf")) {
        int delay_ms = 100 * (rdma_fail_streak > 0 ? rdma_fail_streak : 1);
        if (delay_ms < 100) delay_ms = 100;
        if (delay_ms > 1000) delay_ms = 1000;
        usleep((useconds_t)delay_ms * 1000);
    } else sleep(1);
}

static void repl_slave_ack_heartbeat(void) {
    long long now = kvs_now_ms();
    if (!repl_is_master_link_up()) return;
    if (now - g_slave_last_ack_ms < 1000) return;
    repl_slave_send_ack();
}

#if KVS_ENABLE_RDMA
typedef struct repl_rdma_bg_connect_arg_s {
    char host[128];
    int port;
} repl_rdma_bg_connect_arg_t;

static void *repl_rdma_bg_connect_thread(void *arg) {
    repl_rdma_bg_connect_arg_t *a = (repl_rdma_bg_connect_arg_t *)arg;
    repl_transport_rdma_connect_slave(a->host, a->port);
    kvs_free(a);
    return NULL;
}
#endif

static void *slave_thread(void *arg) {
    int rdma_fail_streak = 0;
    (void)arg;
    for (;;) {
        char host[128];
        int port = 0;
        int gen = 0;
        int role = ROLE_MASTER;
        snapshot_slave_conf(host, sizeof(host), &port, &gen, &role);

        if (role != ROLE_SLAVE || host[0] == '\0' || port <= 0) {
            if (!strcasecmp(repl_transport_name(), "rdma")) repl_rdma_log("slave_loop", "link down because slave config is inactive");
            repl_set_link_state(0);
            repl_slave_retry_pause(rdma_fail_streak);
            continue;
        }

        if (!repl_transport_supported()) {
            if (!strcasecmp(repl_transport_name(), "rdma")) repl_rdma_log("slave_loop", "link down because transport is unsupported");
            repl_set_link_state(0);
            fprintf(stderr, "repl transport '%s' requested but not compiled in; falling back to disconnected state\n", repl_transport_name());
            rdma_fail_streak++;
            repl_slave_retry_pause(rdma_fail_streak);
            continue;
        }

        /* ---- Hybrid dual-transport: RDMA for fullsync + kprobe-rdma for realtime ---- */
        int use_rdma_fullsync = !strcasecmp(repl_fullsync_transport_name(), "rdma") && KVS_ENABLE_RDMA;
        int use_kprobe_realtime = !strcasecmp(repl_realtime_transport_name(), "kprobe-rdma");

        if (use_rdma_fullsync && use_kprobe_realtime) {
            int tcp_fd = repl_transport_tcp_connect_slave(host, port);
            if (tcp_fd < 0) {
                repl_set_link_state(0);
                rdma_fail_streak++;
                repl_slave_retry_pause(rdma_fail_streak);
                continue;
            }

            /* 后台启动 RDMA fullsync QP 连接 */
#if KVS_ENABLE_RDMA
            repl_rdma_bg_connect_arg_t *rdma_arg = (repl_rdma_bg_connect_arg_t *)kvs_malloc(sizeof(repl_rdma_bg_connect_arg_t));
            if (rdma_arg) {
                snprintf(rdma_arg->host, sizeof(rdma_arg->host), "%s", host);
                rdma_arg->port = port;
                pthread_t rdma_tid;
                if (pthread_create(&rdma_tid, NULL, repl_rdma_bg_connect_thread, rdma_arg) != 0) {
                    kvs_free(rdma_arg);
                } else {
                    pthread_detach(rdma_tid);
                }
            }
#endif
            g_slave_transport_kind = KVS_REPL_TRANSPORT_KPROBE_RDMA;
            repl_transport_mark_active("kprobe-rdma");
            rdma_fail_streak = 0;

            g_slave_fd = tcp_fd;
            repl_set_link_state(1);
            unsigned char buf[BUFFER_CAP * 4 + 4096];
            size_t blen = 0;
            int replsync_sent = 0;
            long long rdma_wait_start = kvs_now_ms();

            for (;;) {
                if (slave_should_reconnect(gen)) break;

                /* 延迟 REPLSYNC 直到 RDMA fullsync QP 就绪或超时（5s） */
                if (!replsync_sent) {
                    int rdma_ready = 0;
#if KVS_ENABLE_RDMA
                    rdma_ready = g_repl_rdma_ctx.connected;
#endif
                    if (rdma_ready || (kvs_now_ms() - rdma_wait_start) > 5000) {
                        unsigned char cmd[256];
                        char offbuf[32], durablebuf[32];
                        snprintf(offbuf, sizeof(offbuf), "%llu", g_slave_repl_offset);
                        snprintf(durablebuf, sizeof(durablebuf), "%llu", g_slave_repl_durable_offset);
                        size_t n = resp_build_cmd4(cmd, sizeof(cmd), "REPLSYNC",
                            g_slave_master_replid[0] ? g_slave_master_replid : "?", offbuf, durablebuf);
                        if (send(tcp_fd, cmd, n, 0) < 0) break;
                        replsync_sent = 1;
                        repl_transport_mark_active(rdma_ready ? "rdma+kprobe" : "kprobe-rdma");
#if KVS_ENABLE_RDMA
                        if (rdma_ready) fprintf(stderr, "kprobe rdma: REPLSYNC sent over TCP, fullsync will use RDMA\n");
                        else fprintf(stderr, "kprobe rdma: REPLSYNC sent over TCP, RDMA not ready, fullsync will fallback to TCP\n");
#endif
                        continue;
                    }
                    usleep(50000);
                    continue;
                }

                int had_new_data = 0;

                /* TCP recv 控制消息（全量数据通过 RDMA arrival 通知触达） */
                ssize_t r = recv(tcp_fd, buf + blen, sizeof(buf) - blen, MSG_DONTWAIT);
                if (r > 0) {
                    blen += (size_t)r;
                    had_new_data = 1;
                    parse_resp_stream(NULL, buf, &blen, 1);
                    repl_slave_ack_heartbeat();
                    repl_set_link_state(1);
                } else if (r == 0) {
                    break;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    break;
                }

                if (blen > 0) {
                    parse_resp_stream(NULL, buf, &blen, 1);
                }

                /* 也轮询 RDMA 全量数据 */
#if KVS_ENABLE_RDMA
                if (g_repl_rdma_ctx.connected) {
                    int recv_slot = -1;
                    size_t rdma_blen = 0;
                    if (repl_rdma_wait_cq_recv_completion(100, &recv_slot, &rdma_blen) == 0
                        && recv_slot >= 0 && rdma_blen > 0) {
                        unsigned char *payload;
                        if (rdma_blen > g_repl_rdma_ctx.recv_slots[recv_slot].cap)
                            rdma_blen = g_repl_rdma_ctx.recv_slots[recv_slot].cap;
                        payload = repl_rdma_dup_recv_payload(recv_slot, rdma_blen);
                        if (payload) {
                            if (repl_rdma_repost_recv(recv_slot) == 0) {
                                if (blen + rdma_blen <= sizeof(buf)) {
                                    memcpy(buf + blen, payload, rdma_blen);
                                    blen += rdma_blen;
                                    had_new_data = 1;
                                } else {
                                    fprintf(stderr, "kprobe rdma: slave buffer overflow blen=%zu rdma=%zu\n",
                                        blen, rdma_blen);
                                }
                            }
                            kvs_free(payload);
                        }
                    }
                }
#endif

                if (had_new_data && blen > 0) {
                    parse_resp_stream(NULL, buf, &blen, 1);
                    repl_slave_ack_heartbeat();
                    repl_set_link_state(1);
                } else if (had_new_data) {
                    repl_slave_ack_heartbeat();
                    repl_set_link_state(1);
                }
            }

            g_slave_fd = -1;
            repl_transport_tcp_disconnect_slave(tcp_fd);
            g_slave_transport_kind = KVS_REPL_TRANSPORT_TCP;
            repl_set_link_state(0);
            sleep(1);
            continue;
        }

        /* ---- Hybrid dual-transport: RDMA for fullsync + eBPF for realtime ---- */
        int use_ebpf_realtime = repl_realtime_should_use_ebpf();

        /* If using dual transport, always establish TCP as the primary link.
         * RDMA is established first (background), and REPLSYNC is delayed until
         * RDMA is ready or a timeout expires, so fullsync can use RDMA. */
        if (use_rdma_fullsync && use_ebpf_realtime) {
            int tcp_fd = repl_transport_tcp_connect_slave(host, port);
            if (tcp_fd < 0) {
                repl_set_link_state(0);
                rdma_fail_streak++;
                repl_slave_retry_pause(rdma_fail_streak);
                continue;
            }

            /* Kick off RDMA connection attempt in a background thread.
             * We delay REPLSYNC until RDMA is ready so fullsync can use RDMA. */
#if KVS_ENABLE_RDMA
            repl_rdma_bg_connect_arg_t *rdma_arg = (repl_rdma_bg_connect_arg_t *)kvs_malloc(sizeof(repl_rdma_bg_connect_arg_t));
            if (rdma_arg) {
                snprintf(rdma_arg->host, sizeof(rdma_arg->host), "%s", host);
                rdma_arg->port = port;
                pthread_t rdma_tid;
                if (pthread_create(&rdma_tid, NULL, repl_rdma_bg_connect_thread, rdma_arg) != 0) {
                    kvs_free(rdma_arg);
                } else {
                    pthread_detach(rdma_tid);
                }
            }
#endif
            g_slave_transport_kind = KVS_REPL_TRANSPORT_EBPF;
            repl_transport_mark_active("ebpf");
            rdma_fail_streak = 0;

            /* Register TCP fd with eBPF sockmap for realtime sync */
            if (repl_ebpf_register_fd(tcp_fd, 0) != 0) {
                fprintf(stderr, "repl ebpf: fd registration failed on slave link, using tcp-compatible path\n");
            }

            g_slave_fd = tcp_fd;
            repl_set_link_state(1);
            unsigned char buf[BUFFER_CAP * 4 + 4096];
            size_t blen = 0;
            int replsync_sent = 0;
            long long rdma_wait_start = kvs_now_ms();

            for (;;) {
                if (slave_should_reconnect(gen)) break;

                /* Delay REPLSYNC until RDMA is connected or timeout (5s).
                 * This lets fullsync bulk data flow over RDMA when available. */
                if (!replsync_sent) {
                    int rdma_ready = 0;
#if KVS_ENABLE_RDMA
                    rdma_ready = g_repl_rdma_ctx.connected;
#endif
                    if (rdma_ready || (kvs_now_ms() - rdma_wait_start) > 5000) {
                        unsigned char cmd[256];
                        char offbuf[32], durablebuf[32];
                        snprintf(offbuf, sizeof(offbuf), "%llu", g_slave_repl_offset);
                        snprintf(durablebuf, sizeof(durablebuf), "%llu", g_slave_repl_durable_offset);
                        size_t n = resp_build_cmd4(cmd, sizeof(cmd), "REPLSYNC",
                            g_slave_master_replid[0] ? g_slave_master_replid : "?", offbuf, durablebuf);
                        if (send(tcp_fd, cmd, n, 0) < 0) break;
                        replsync_sent = 1;
                        repl_transport_mark_active(rdma_ready ? "rdma+ebpf" : "ebpf");
#if KVS_ENABLE_RDMA
                        if (rdma_ready) repl_rdma_log("slave_loop", "REPLSYNC sent, fullsync will use RDMA");
#endif
                        continue;
                    }
                    /* Wait a bit for RDMA to connect, then retry */
                    usleep(50000);  /* 50ms */
                    continue;
                }

                int had_new_data = 0;

                /* TCP recv is non-blocking; FULLRESYNC header arrives via RDMA
                 * alongside snapshot data, so we don't block waiting for TCP. */
                ssize_t r = recv(tcp_fd, buf + blen, sizeof(buf) - blen, MSG_DONTWAIT);
                if (r > 0) {
                    blen += (size_t)r;
                    had_new_data = 1;
                    /* Parse TCP data immediately to free buffer space for RDMA */
                    parse_resp_stream(NULL, buf, &blen, 1);
                    repl_slave_ack_heartbeat();
                    repl_set_link_state(1);
                } else if (r == 0) {
                    break;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    break;
                }

                /* Parse any leftover from previous RDMA chunk to maximize
                 * buffer space before checking for new RDMA data */
                if (blen > 0) {
                    parse_resp_stream(NULL, buf, &blen, 1);
                }

                /* Also check RDMA for fullsync data */
#if KVS_ENABLE_RDMA
                if (g_repl_rdma_ctx.connected) {
                    int recv_slot = -1;
                    size_t rdma_blen = 0;
                    if (repl_rdma_wait_cq_recv_completion(100, &recv_slot, &rdma_blen) == 0
                        && recv_slot >= 0 && rdma_blen > 0) {
                        unsigned char *payload;
                        if (rdma_blen > g_repl_rdma_ctx.recv_slots[recv_slot].cap)
                            rdma_blen = g_repl_rdma_ctx.recv_slots[recv_slot].cap;
                        payload = repl_rdma_dup_recv_payload(recv_slot, rdma_blen);
                        if (payload) {
                            if (repl_rdma_repost_recv(recv_slot) == 0) {
                                fprintf(stderr, "repl rdma: slave_debug_rdma - recv_slot=%d rdma_blen=%zu blen_before=%zu buf_size=%zu\n",
                                    recv_slot, rdma_blen, blen, sizeof(buf));
                                if (blen + rdma_blen <= sizeof(buf)) {
                                    memcpy(buf + blen, payload, rdma_blen);
                                    blen += rdma_blen;
                                    had_new_data = 1;
                                } else {
                                    fprintf(stderr, "repl rdma: slave_debug_rdma - BUFFER OVERFLOW! blen=%zu rdma_blen=%zu sizeof(buf)=%zu\n",
                                        blen, rdma_blen, sizeof(buf));
                                }
                            }
                            kvs_free(payload);
                        }
                    }
                }
#endif

                /* Only parse when new data was added to avoid busy-loop
                 * on incomplete data. */
                if (had_new_data && blen > 0) {
                    size_t before = blen;
                    parse_resp_stream(NULL, buf, &blen, 1);
                    fprintf(stderr, "repl rdma: slave_debug_parse - before=%zu after=%zu consumed=%zu\n",
                        before, blen, before - blen);
                    repl_slave_ack_heartbeat();
                    repl_set_link_state(1);
                } else if (had_new_data) {
                    /* TCP had data but parse_resp_stream consumed it all */
                    repl_slave_ack_heartbeat();
                    repl_set_link_state(1);
                }
            }

            g_slave_fd = -1;
            repl_transport_ebpf_disconnect_slave(tcp_fd);
            g_slave_transport_kind = KVS_REPL_TRANSPORT_TCP;
            repl_set_link_state(0);
            sleep(1);
            continue;
        }

        /* ---- Single transport path (backward compatible) ---- */
        int fd = repl_transport_ops()->connect_slave(host, port);
        if (fd < 0) {
            if (!strcasecmp(repl_transport_configured_name(), "rdma")) {
                repl_rdma_set_state(REPL_RDMA_STATE_BACKOFF, "connect_slave_failed");
            }
            if (!strcasecmp(repl_transport_name(), "rdma")) repl_rdma_log("slave_loop", "link down because connect_slave failed");
            repl_set_link_state(0);
            rdma_fail_streak++;
            repl_slave_retry_pause(rdma_fail_streak);
            continue;
        }

        if (!strcasecmp(repl_transport_name(), "rdma") ||
            !strcasecmp(repl_transport_name(), "rdma+kprobe")) {
            unsigned char cmd[256];
            unsigned char stream_buf[BUFFER_CAP * 4 + 4096];
            char offbuf[32];
            char durablebuf[32];
            size_t n;
            size_t blen;
            size_t stream_len = 0;
            int recv_slot;
            g_slave_transport_kind = KVS_REPL_TRANSPORT_RDMA;
            repl_rdma_set_state(REPL_RDMA_STATE_SYNCING, "slave_replsync_sent");
            snprintf(offbuf, sizeof(offbuf), "%llu", g_slave_repl_offset);
            snprintf(durablebuf, sizeof(durablebuf), "%llu", g_slave_repl_durable_offset);
            n = resp_build_cmd4(cmd, sizeof(cmd), "REPLSYNC", g_slave_master_replid[0] ? g_slave_master_replid : "?", offbuf, durablebuf);
            if (repl_transport_send_on_slave_link(cmd, n) != 0) {
                repl_rdma_log("slave_loop", "link down because initial REPLSYNC send failed");
                repl_transport_ops()->disconnect_slave(fd);
                repl_set_link_state(0);
                rdma_fail_streak++;
                repl_slave_retry_pause(rdma_fail_streak);
                continue;
            }
            rdma_fail_streak = 0;
            repl_transport_mark_active("rdma");
            repl_set_link_state(1);
            repl_rdma_log("slave_loop", "sent initial REPLSYNC over rdma");
            for (;;) {
                if (slave_should_reconnect(gen)) {
                    repl_rdma_log("slave_loop", "breaking for reconnect generation change");
                    break;
                }
                if (!g_repl_rdma_ctx.connected) {
                    repl_rdma_log("slave_loop", "breaking because rdma transport disconnected");
                    break;
                }
                recv_slot = -1;
                if (repl_rdma_wait_cq_recv_completion(2000, &recv_slot, &blen) == 0 && recv_slot >= 0 && blen > 0) {
                    unsigned char *payload;
                    if (blen > g_repl_rdma_ctx.recv_slots[recv_slot].cap) blen = g_repl_rdma_ctx.recv_slots[recv_slot].cap;
                    payload = repl_rdma_dup_recv_payload(recv_slot, blen);
                    if (!payload) {
                        repl_rdma_log("slave_loop", "failed to copy recv payload");
                        break;
                    }
                    if (repl_rdma_repost_recv(recv_slot) != 0) {
                        kvs_free(payload);
                        repl_rdma_log("slave_loop", "failed to repost recv");
                        break;
                    }
                    if (stream_len + blen > sizeof(stream_buf)) {
                        kvs_free(payload);
                        stream_len = 0;
                        repl_rdma_log("slave_loop", "stream buffer overflow while appending recv payload");
                        break;
                    }
#if KVS_ENABLE_RDMA
                    {
                        size_t preview_len = blen < 96 ? blen : 96;
                        fprintf(stderr, "repl rdma: slave_chunk - recv_len=%zu preview=%.*s\n", blen, (int)preview_len, payload);
                    }
#endif
                    memcpy(stream_buf + stream_len, payload, blen);
                    stream_len += blen;
                    kvs_free(payload);
                    parse_resp_stream(NULL, stream_buf, &stream_len, 1);
                    repl_slave_ack_heartbeat();
                    repl_rdma_set_state(REPL_RDMA_STATE_STEADY, "processed_replication_chunk");
                    repl_rdma_log("slave_loop", "processed rdma response chunk");
                    repl_set_link_state(1);
                    continue;
                }
                if (!g_repl_rdma_ctx.connected) {
                    repl_rdma_log("slave_loop", "link down after recv wait because transport disconnected");
                    break;
                }
                repl_set_link_state(1);
            }
            /* 主动断开 RDMA 连接，触发重连 */
            g_repl_rdma_ctx.connected = 0;
            while (!slave_should_reconnect(gen) && g_repl_rdma_ctx.connected) {
                sleep(1);
                repl_set_link_state(1);
            }
            if (slave_should_reconnect(gen)) repl_rdma_log("slave_loop", "link down because reconnect was requested");
            else if (!g_repl_rdma_ctx.connected) repl_rdma_log("slave_loop", "link down because transport remained disconnected after recv loop");
            repl_transport_ops()->disconnect_slave(fd);
            g_slave_transport_kind = KVS_REPL_TRANSPORT_TCP;
            repl_set_link_state(0);
            if (!g_repl_rdma_ctx.connected) rdma_fail_streak++;
            else rdma_fail_streak = 0;
            repl_slave_retry_pause(rdma_fail_streak);
            continue;
        }

        unsigned char cmd[256];
        char offbuf[32];
        char durablebuf[32];
        snprintf(offbuf, sizeof(offbuf), "%llu", g_slave_repl_offset);
        snprintf(durablebuf, sizeof(durablebuf), "%llu", g_slave_repl_durable_offset);
        size_t n = resp_build_cmd4(cmd, sizeof(cmd), "REPLSYNC", g_slave_master_replid[0] ? g_slave_master_replid : "?", offbuf, durablebuf);
        if (send(fd, cmd, n, 0) < 0) {
            repl_transport_mark_active("tcp");
            g_slave_transport_kind = KVS_REPL_TRANSPORT_TCP;
            repl_transport_ops()->disconnect_slave(fd);
            repl_set_link_state(0);
            sleep(1);
            continue;
        }

        if (repl_should_use_ebpf_now()) {
            repl_transport_mark_active("ebpf");
            g_slave_transport_kind = KVS_REPL_TRANSPORT_EBPF;
        } else {
            repl_transport_mark_active("tcp");
            g_slave_transport_kind = KVS_REPL_TRANSPORT_TCP;
        }
        g_slave_fd = fd;
        repl_set_link_state(1);
        unsigned char buf[BUFFER_CAP];
        size_t blen = 0;

        for (;;) {
            if (slave_should_reconnect(gen)) break;
            ssize_t r = recv(fd, buf + blen, sizeof(buf) - blen, 0);
            if (r > 0) {
                blen += (size_t)r;
                parse_resp_stream(NULL, buf, &blen, 1);
                repl_slave_ack_heartbeat();
                repl_set_link_state(1);
                continue;
            }
            if (r == 0) break;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }

        g_slave_fd = -1;
        repl_transport_ops()->disconnect_slave(fd);
        g_slave_transport_kind = KVS_REPL_TRANSPORT_TCP;
        repl_set_link_state(0);
        sleep(1);
    }
    return NULL;
}

int repl_handle_replica_send_failure(conn_t *c, conn_t **linkp) {
    if (!c || !linkp) return 0;
#if KVS_ENABLE_RDMA
    if (c == &g_rdma_master_replica_conn) {
        conn_t *next = c->next_replica;
        repl_remove_slave(c);
        repl_rdma_drop_master_replica_shallow(c);
        *linkp = next;
        {
            char last_stage[64];
            char last_preview[160];
            unsigned long long last_len = 0;
            unsigned long long last_offset = 0;
            repl_get_last_send_context(last_stage, sizeof(last_stage), &last_len, &last_offset, last_preview, sizeof(last_preview));
            fprintf(stderr, "repl rdma: broadcast_context - last_send_stage=%s last_send_len=%llu last_send_offset=%llu last_send_preview=%s\n",
                last_stage, last_len, last_offset, last_preview);
        }
        repl_rdma_log("broadcast", "dropping stale master rdma replica before reset");
        repl_rdma_reset_conn_ctx(1);
        repl_rdma_log("broadcast", "master rdma conn ctx reset complete");
        return 1;
    }
#endif
    return 0;
}

int start_slave_thread(void) {
    pthread_t tid;
    if (g_slave_thread_started) return 0;
    if (g_cfg.role == ROLE_SLAVE && g_cfg.master_host[0] && g_cfg.master_port > 0) {
        repl_slaveof(g_cfg.master_host, g_cfg.master_port);
    }
    if (pthread_create(&tid, NULL, slave_thread, NULL) != 0) return -1;
    pthread_detach(tid);
    g_slave_thread_started = 1;
    return 0;
}

#if KVS_ENABLE_RDMA
static void *rdma_master_listener_thread(void *arg) {
    (void)arg;
    struct sockaddr_in addr;
    struct rdma_cm_event *event = NULL;
    struct rdma_conn_param param;
    int consecutive_failures = 0;
    for (;;) {
        long long accept_start_ms = 0;
        long long initial_recv_start_ms = 0;
        if (g_cfg.role != ROLE_MASTER || (!repl_should_use_rdma_now())) {
            repl_rdma_reset_ctx();
            consecutive_failures = 0;
            sleep(1);
            continue;
        }

        /* Helper: on any setup failure, increment counter and fallback after 10 */
        #define LISTENER_FAIL(step_name) do { \
            consecutive_failures++; \
            repl_rdma_log("listener", step_name " failed"); \
            if (consecutive_failures >= 10) { \
                fprintf(stderr, "repl rdma: listener - giving up after %d failures (%s), falling back to TCP for 10s\n", \
                    consecutive_failures, step_name); \
                repl_transport_trigger_fallback("rdma_listener_fail", 10000); \
                repl_rdma_reset_ctx(); \
                consecutive_failures = 0; \
                sleep(3); \
            } else { \
                repl_rdma_reset_conn_ctx(1); \
                usleep(500000); \
            } \
            continue; \
        } while(0)

        if (!g_repl_rdma_ctx.ec) {
            for (int ec_retry = 0; ec_retry < 5; ec_retry++) {
                g_repl_rdma_ctx.ec = rdma_create_event_channel();
                if (g_repl_rdma_ctx.ec) break;
                char ec_err[128];
                snprintf(ec_err, sizeof(ec_err), "create failed errno=%d retry=%d", errno, ec_retry);
                repl_rdma_log("listener", ec_err);
                if (ec_retry < 4) usleep(200000);
            }
            if (!g_repl_rdma_ctx.ec) {
                LISTENER_FAIL("event channel create");
            }
            consecutive_failures = 0;
        }
        if (!g_repl_rdma_ctx.listen_id) {
            repl_rdma_drop_master_replica_shallow(&g_rdma_master_replica_conn);
            if (rdma_create_id(g_repl_rdma_ctx.ec, &g_repl_rdma_ctx.listen_id, NULL, RDMA_PS_TCP) != 0) {
                LISTENER_FAIL("create listen id");
            }
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            {
                int rdma_port = g_cfg.rdma_port > 0 ? g_cfg.rdma_port : g_cfg.port + 1;
                addr.sin_port = htons((uint16_t)rdma_port);
            }
            if (inet_pton(AF_INET, g_cfg.master_host[0] ? g_cfg.master_host : "0.0.0.0", &addr.sin_addr) <= 0) addr.sin_addr.s_addr = htonl(INADDR_ANY);
            if (rdma_bind_addr(g_repl_rdma_ctx.listen_id, (struct sockaddr *)&addr) != 0) {
                LISTENER_FAIL("bind");
            }
            if (rdma_listen(g_repl_rdma_ctx.listen_id, 4) != 0) {
                /* Destroy failed listen_id before retry; preserve ec */
                rdma_destroy_id(g_repl_rdma_ctx.listen_id);
                g_repl_rdma_ctx.listen_id = NULL;
                LISTENER_FAIL("listen");
            }
            repl_rdma_log("listener", "listening");
            consecutive_failures = 0;
        }
        #undef LISTENER_FAIL

        if (rdma_get_cm_event(g_repl_rdma_ctx.ec, &event) != 0) {
            repl_rdma_log("listener", "get_cm_event failed");
            repl_rdma_reset_ctx();
            sleep(1);
            continue;
        }
        if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
            fprintf(stderr, "repl rdma: listener unexpected event - got=%s expect=%s\n", rdma_event_str(event->event), rdma_event_str(RDMA_CM_EVENT_CONNECT_REQUEST));
            rdma_ack_cm_event(event);
            repl_rdma_reset_ctx();
            sleep(1);
            continue;
        }
        consecutive_failures = 0;
        repl_rdma_log("listener", "connect request received");
        g_repl_rdma_ctx.accepted_id = event->id;
        rdma_ack_cm_event(event);
        g_repl_rdma_ctx.id = g_repl_rdma_ctx.accepted_id;
        g_repl_rdma_ctx.comp_chan = ibv_create_comp_channel(g_repl_rdma_ctx.id->verbs);
        if (!g_repl_rdma_ctx.comp_chan) {
            repl_rdma_log("listener", "comp channel create failed");
            repl_rdma_reset_ctx();
            sleep(1);
            continue;
        }
        g_repl_rdma_ctx.pd = ibv_alloc_pd(g_repl_rdma_ctx.id->verbs);
        if (!g_repl_rdma_ctx.pd) {
            repl_rdma_log("listener", "alloc pd failed");
            repl_rdma_reset_ctx();
            sleep(1);
            continue;
        }
        repl_rdma_refresh_runtime_cfg();
        g_repl_rdma_ctx.cq = ibv_create_cq(g_repl_rdma_ctx.id->verbs, g_repl_rdma_ctx.active_qp_wr_depth, NULL, g_repl_rdma_ctx.comp_chan, 0);
        if (!g_repl_rdma_ctx.cq) {
            repl_rdma_log("listener", "create cq failed");
            repl_rdma_reset_ctx();
            sleep(1);
            continue;
        }
        if (repl_rdma_create_qp() != 0 || repl_rdma_prepare_buffers() != 0 || repl_rdma_post_initial_recv() != 0) {
            repl_rdma_log("listener", "resource prepare failed");
            repl_rdma_reset_ctx();
            sleep(1);
            continue;
        }
        memset(&param, 0, sizeof(param));
        param.initiator_depth = 1;
        param.responder_resources = 1;
        param.rnr_retry_count = 3;
        accept_start_ms = kvs_now_ms();
        if (rdma_accept(g_repl_rdma_ctx.id, &param) != 0) {
            repl_rdma_log("listener", "rdma_accept failed");
            repl_rdma_reset_ctx();
            sleep(1);
            continue;
        }
        repl_rdma_log("listener", "accept issued");
        if (repl_rdma_wait_event(RDMA_CM_EVENT_ESTABLISHED, 5000) != 0) {
            fprintf(stderr, "repl rdma: listener_established_wait_fail - elapsed_ms=%lld\n", kvs_now_ms() - accept_start_ms);
            repl_rdma_reset_ctx();
            sleep(1);
            continue;
        }
        fprintf(stderr, "repl rdma: listener_established_ok - elapsed_ms=%lld\n", kvs_now_ms() - accept_start_ms);
        g_repl_rdma_ctx.connected = 1;
        repl_rdma_log("listener", "established");
        /* 启动 CQ 轮询线程（pipeline 模式） */
        if (g_repl_rdma_ctx.send_pipeline_enabled) {
            repl_rdma_start_cq_poll_thread();
        }
        {
            int recv_slot = -1;
            size_t recv_len = 0;
            unsigned char stream_buf[BUFFER_CAP * 4 + 4096];
            size_t stream_len = 0;
            memset(&g_rdma_master_replica_conn, 0, sizeof(g_rdma_master_replica_conn));
            g_rdma_master_replica_conn.repl_transport_kind = KVS_REPL_TRANSPORT_RDMA;
            initial_recv_start_ms = kvs_now_ms();

            /* In hybrid mode (RDMA fullsync + eBPF realtime), the main thread
             * sends fullsync data over RDMA and polls the shared CQ for send
             * completions. The listener must NOT compete for CQ entries;
             * it just waits for the connection to close. */
            int hybrid_mode = !strcasecmp(repl_fullsync_transport_name(), "rdma")
                           && repl_realtime_should_use_ebpf();

            for (;;) {
                unsigned char *payload;
                size_t blen;
                if (!g_repl_rdma_ctx.connected) break;

                if (hybrid_mode) {
                    /* Hybrid: let main thread own the CQ; just sleep and
                     * periodically check connection status. */
                    sleep(1);
                    continue;
                }

                recv_slot = -1;
                recv_len = 0;
                if (repl_rdma_wait_cq_recv_completion(2000, &recv_slot, &recv_len) != 0 || recv_slot < 0 || recv_len == 0) {
                    continue;
                }
                blen = recv_len;
                if (blen > g_repl_rdma_ctx.recv_slots[recv_slot].cap) blen = g_repl_rdma_ctx.recv_slots[recv_slot].cap;
                payload = repl_rdma_dup_recv_payload(recv_slot, blen);
                if (!payload) {
                    repl_rdma_log("listener", "failed to copy recv payload");
                    break;
                }
                if (repl_rdma_repost_recv(recv_slot) != 0) {
                    kvs_free(payload);
                    repl_rdma_log("listener", "failed to repost recv");
                    break;
                }
                if (stream_len + blen > sizeof(stream_buf)) {
                    kvs_free(payload);
                    stream_len = 0;
                    repl_rdma_log("listener", "stream buffer overflow while appending recv payload");
                    break;
                }
                memcpy(stream_buf + stream_len, payload, blen);
                stream_len += blen;
                kvs_free(payload);
                parse_resp_stream(&g_rdma_master_replica_conn, stream_buf, &stream_len, 0);
                if (initial_recv_start_ms != 0) {
                    fprintf(stderr, "repl rdma: listener_initial_payload_ok - elapsed_ms=%lld recv_len=%zu\n", kvs_now_ms() - initial_recv_start_ms, recv_len);
                    repl_rdma_log("listener", "processed initial rdma payload");
                    initial_recv_start_ms = 0;
                }
            }
            if (initial_recv_start_ms != 0) {
                fprintf(stderr, "repl rdma: listener_initial_payload_fail - elapsed_ms=%lld recv_slot=%d recv_len=%zu\n", kvs_now_ms() - initial_recv_start_ms, recv_slot, recv_len);
                repl_rdma_log("listener", "no initial rdma payload received");
            }
        }
        repl_rdma_log("listener", g_repl_rdma_ctx.connected ? "listener loop exiting while still marked connected" : "listener loop exiting after disconnect");
        /* cleanup 由主线程的 failover 逻辑处理，listener 线程不直接重置
         * 避免线程间 RDMA 资源竞态导致的 segfault */
    }
    return NULL;
}
#endif

int start_rdma_master_listener(void) {
#if KVS_ENABLE_RDMA
    pthread_t tid;
    const char *fullsync_t = repl_fullsync_transport_name();
    if (g_cfg.role != ROLE_MASTER) return 0;
    if (strcasecmp(fullsync_t, "rdma") != 0 && strcasecmp(g_cfg.repl_transport_backend, "rdma") != 0) return 0;
    if (g_rdma_master_listener_started) return 0;
    if (pthread_create(&tid, NULL, rdma_master_listener_thread, NULL) != 0) return -1;
    pthread_detach(tid);
    g_rdma_master_listener_started = 1;
#endif
    return 0;
}
