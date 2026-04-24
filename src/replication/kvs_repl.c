#include "kvstore/kvstore.h"
#include <poll.h>

#if KVS_ENABLE_RDMA
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#endif

#define KVS_REPL_BACKLOG_SIZE (1024 * 1024)

typedef struct repl_backlog_s {
    unsigned char *buf;
    size_t cap;
    size_t histlen;
    size_t head;
    unsigned long long start_offset;
    unsigned long long end_offset;
} repl_backlog_t;

static pthread_mutex_t g_slave_conf_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_slave_host[128] = "";
static int g_slave_port = 0;
static int g_slave_conf_gen = 0;
static int g_master_link_up = 0;
static long long g_master_last_io_ms = 0;
static int g_slave_thread_started = 0;
static int g_rdma_master_listener_started = 0;
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
static repl_backlog_t g_repl_backlog = {0};
static char g_slave_master_replid[41] = "?";
static unsigned long long g_slave_repl_offset = 0;
static int g_slave_loading_fullsync = 0;
static char g_slave_state_path[512] = {0};
static conn_t g_rdma_master_replica_conn = {0};

#if KVS_ENABLE_RDMA
#define KVS_RDMA_RECV_SLOTS_MAX 64
#define KVS_RDMA_RECV_SLOTS_DEFAULT 32
#define KVS_RDMA_CHUNK_SIZE_DEFAULT (BUFFER_CAP / 4)
#define KVS_RDMA_QP_WR_DEPTH_DEFAULT 64

static void repl_rdma_reset_ctx(void);
static void repl_rdma_reset_conn_ctx(int preserve_listener);
static void repl_rdma_log(const char *stage, const char *detail);

typedef struct repl_rdma_recv_slot_s {
    struct ibv_mr *mr;
    unsigned char *buf;
    size_t cap;
    int posted;
} repl_rdma_recv_slot_t;

typedef struct repl_rdma_ctx_s {
    struct rdma_event_channel *ec;
    struct rdma_cm_id *id;
    struct rdma_cm_id *listen_id;
    struct rdma_cm_id *accepted_id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_comp_channel *comp_chan;
    struct ibv_mr *send_mr;
    unsigned char *send_buf;
    size_t send_buf_cap;
    repl_rdma_recv_slot_t recv_slots[KVS_RDMA_RECV_SLOTS_MAX];
    size_t recv_buf_cap;
    int active_recv_slots;
    int active_qp_wr_depth;
    size_t active_chunk_size;
    int addr_resolved;
    int route_resolved;
    int qp_ready;
    int connected;
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
    if (v > BUFFER_CAP) v = BUFFER_CAP;
    return v;
}

static void repl_rdma_refresh_runtime_cfg(void) {
    g_repl_rdma_ctx.active_recv_slots = repl_rdma_cfg_recv_slots();
    g_repl_rdma_ctx.active_qp_wr_depth = repl_rdma_cfg_qp_wr_depth();
    g_repl_rdma_ctx.active_chunk_size = repl_rdma_cfg_chunk_size();
}

static void repl_rdma_reset_conn_ctx(int preserve_listener) {
    int i;
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
    if (g_repl_rdma_ctx.send_mr) {
        ibv_dereg_mr(g_repl_rdma_ctx.send_mr);
        g_repl_rdma_ctx.send_mr = NULL;
    }
    if (g_repl_rdma_ctx.send_buf) {
        kvs_free(g_repl_rdma_ctx.send_buf);
        g_repl_rdma_ctx.send_buf = NULL;
    }
    g_repl_rdma_ctx.send_buf_cap = 0;
    g_repl_rdma_ctx.recv_buf_cap = 0;
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

static int repl_rdma_prepare_addr(const char *host, int port, struct sockaddr_in *addr) {
    if (!host || !addr || port <= 0) return -1;
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((uint16_t)port);
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
    param.retry_count = 3;
    param.rnr_retry_count = 3;
    repl_rdma_log("connect", "issuing rdma_connect");
    if (rdma_connect(g_repl_rdma_ctx.id, &param) != 0) {
        repl_rdma_log("connect", "rdma_connect failed");
        return -1;
    }
    if (repl_rdma_wait_event(RDMA_CM_EVENT_ESTABLISHED, establish_timeout_ms) != 0) {
        repl_rdma_log("connect", "established wait timed out or failed");
        return -1;
    }
    g_repl_rdma_ctx.connected = 1;
    repl_rdma_log("connect", "established");
    return 0;
}

static int repl_rdma_prepare_buffers(void) {
    const size_t cap = BUFFER_CAP;
    int i;
    repl_rdma_refresh_runtime_cfg();
    if (!g_repl_rdma_ctx.pd) return -1;
    g_repl_rdma_ctx.send_buf = (unsigned char *)kvs_malloc(cap);
    if (!g_repl_rdma_ctx.send_buf) {
        repl_rdma_log("prepare_buffers", "buffer alloc failed");
        return -1;
    }
    g_repl_rdma_ctx.send_buf_cap = cap;
    g_repl_rdma_ctx.recv_buf_cap = cap;
    memset(g_repl_rdma_ctx.send_buf, 0, cap);
    g_repl_rdma_ctx.send_mr = ibv_reg_mr(g_repl_rdma_ctx.pd, g_repl_rdma_ctx.send_buf, cap, IBV_ACCESS_LOCAL_WRITE);
    if (!g_repl_rdma_ctx.send_mr) {
        repl_rdma_log("prepare_buffers", "send mr register failed");
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
        int n = ibv_poll_cq(g_repl_rdma_ctx.cq, 1, &wc);
        if (n > 0) {
            if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_SEND) {
                return 0;
            }
            if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_RECV) {
                continue;
            }
            fprintf(stderr, "repl rdma: send_cq failed - status=%d opcode=%d\n", wc.status, wc.opcode);
            g_rdma_send_cq_error_count++;
            g_repl_rdma_ctx.connected = 0;
            return -1;
        }
        if (repl_rdma_drain_cm_events_nonblock() != 0 || !g_repl_rdma_ctx.connected) {
            repl_rdma_log("send_cq", "transport already disconnected");
            return -1;
        }
        if (kvs_now_ms() >= deadline) {
            repl_rdma_drain_cm_events_nonblock();
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
    if (!g_repl_rdma_ctx.cq) return -1;
    for (;;) {
        int n = ibv_poll_cq(g_repl_rdma_ctx.cq, 1, &wc);
        if (n > 0) {
            if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_RECV) {
                int slot = (wc.wr_id > 0 && wc.wr_id <= (uint64_t)g_repl_rdma_ctx.active_recv_slots) ? (int)(wc.wr_id - 1) : -1;
                if (slot >= 0 && slot < g_repl_rdma_ctx.active_recv_slots) g_repl_rdma_ctx.recv_slots[slot].posted = 0;
                if (slot_out) *slot_out = slot;
                if (recv_len) *recv_len = (size_t)wc.byte_len;
                return (slot >= 0) ? 0 : -1;
            }
            if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_SEND) {
                continue;
            }
            fprintf(stderr, "repl rdma: recv_cq failed - status=%d opcode=%d\n", wc.status, wc.opcode);
            g_rdma_recv_cq_error_count++;
            if (repl_rdma_drain_cm_events_nonblock() != 0 || !g_repl_rdma_ctx.connected) {
                repl_rdma_log("recv_cq", "transport already disconnected after recv failure");
            }
            return -1;
        }
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
    if (!buf || len == 0) return 0;
    if (repl_rdma_drain_cm_events_nonblock() != 0) return -1;
    if (!g_repl_rdma_ctx.connected || !g_repl_rdma_ctx.id || !g_repl_rdma_ctx.id->qp) return -1;
    if (!g_repl_rdma_ctx.send_buf || !g_repl_rdma_ctx.send_mr) return -1;
    if (len > g_repl_rdma_ctx.send_buf_cap) {
        repl_rdma_log("try_send", "payload too large");
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
        return -1;
    }
    return repl_rdma_wait_cq_send_completion(1000);
}
#endif

static void repl_transport_tcp_disconnect_slave(int fd) {
    if (fd >= 0) close(fd);
}

static int repl_transport_rdma_send(conn_t *c, const unsigned char *buf, size_t len) {
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

static int repl_transport_rdma_connect_slave(const char *host, int port) {
    (void)host;
    (void)port;
#if KVS_ENABLE_RDMA
    struct sockaddr_in dst;
    repl_rdma_log("connect_slave", "begin");
    if (repl_rdma_prepare_addr(host, port, &dst) != 0) {
        repl_rdma_log("prepare_addr", "failed");
        return -1;
    }
    repl_rdma_log("prepare_addr", "ok");
    repl_rdma_reset_ctx();
    g_repl_rdma_ctx.ec = rdma_create_event_channel();
    if (!g_repl_rdma_ctx.ec) {
        repl_rdma_log("event_channel", "create failed");
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
        repl_rdma_reset_ctx();
        return -1;
    }
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

static const repl_transport_ops_t g_repl_transport_rdma_ops = {
    .name = "rdma",
    .supported = KVS_ENABLE_RDMA,
    .send = repl_transport_rdma_send,
    .connect_slave = repl_transport_rdma_connect_slave,
    .disconnect_slave = repl_transport_rdma_disconnect_slave,
};

static const repl_transport_ops_t *repl_transport_ops(void) {
    if (!strcasecmp(g_cfg.repl_transport_backend, "rdma")) return &g_repl_transport_rdma_ops;
    return &g_repl_transport_tcp_ops;
}

static int repl_transport_supported(void) {
    return repl_transport_ops()->supported;
}

const char *repl_transport_name(void) {
    return repl_transport_ops()->name;
}

int repl_transport_send(conn_t *c, const unsigned char *buf, size_t len) {
    return repl_transport_ops()->send(c, buf, len);
}

int repl_transport_send_many(conn_t *c, const unsigned char *buf1, size_t len1, const unsigned char *buf2, size_t len2) {
    if (repl_transport_send(c, buf1, len1) != 0) return -1;
    if (repl_transport_send(c, buf2, len2) != 0) return -1;
    return 0;
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

void repl_slave_set_sync_state(const char *replid, unsigned long long offset, int fullsync_loading) {
    if (replid && *replid) {
        snprintf(g_slave_master_replid, sizeof(g_slave_master_replid), "%s", replid);
    }
    g_slave_repl_offset = offset;
    g_slave_loading_fullsync = fullsync_loading;
#if KVS_ENABLE_RDMA
    fprintf(stderr, "repl rdma: slave_sync_state - replid=%s offset=%llu fullsync_loading=%d\n",
        g_slave_master_replid, g_slave_repl_offset, g_slave_loading_fullsync);
#endif
    repl_slave_state_save();
}

void repl_slave_finish_fullsync(void) {
    g_slave_loading_fullsync = 0;
#if KVS_ENABLE_RDMA
    fprintf(stderr, "repl rdma: slave_fullsync - finished offset=%llu\n", g_slave_repl_offset);
#endif
    repl_slave_state_save();
}

void repl_slave_note_applied(size_t rawlen) {
    if (!g_slave_loading_fullsync) {
        unsigned long long before = g_slave_repl_offset;
        g_slave_repl_offset += (unsigned long long)rawlen;
#if KVS_ENABLE_RDMA
        fprintf(stderr, "repl rdma: slave_apply - offset_before=%llu rawlen=%zu offset_after=%llu\n",
            before, rawlen, g_slave_repl_offset);
#endif
        repl_slave_state_save();
    } else {
#if KVS_ENABLE_RDMA
        fprintf(stderr, "repl rdma: slave_apply - fullsync_chunk=%zu\n", rawlen);
#endif
    }
}

const char *repl_slave_master_id(void) {
    return g_slave_master_replid;
}

unsigned long long repl_slave_offset(void) {
    return g_slave_repl_offset;
}

int repl_slave_loading_fullsync(void) {
    return g_slave_loading_fullsync;
}

int repl_slave_state_load(void) {
    FILE *fp;
    char replid[41] = {0};
    unsigned long long offset = 0;
    build_slave_state_path();
    fp = fopen(g_slave_state_path, "r");
    if (!fp) return 0;
    if (fscanf(fp, "%40s %llu", replid, &offset) == 2) {
        snprintf(g_slave_master_replid, sizeof(g_slave_master_replid), "%s", replid);
        g_slave_repl_offset = offset;
    }
    fclose(fp);
    return 0;
}

int repl_slave_state_save(void) {
    FILE *fp;
    build_slave_state_path();
    fp = fopen(g_slave_state_path, "w");
    if (!fp) return -1;
    fprintf(fp, "%s %llu\n", g_slave_master_replid[0] ? g_slave_master_replid : "?", g_slave_repl_offset);
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
    if (!strcasecmp(repl_transport_name(), "rdma")) {
        int delay_ms = 100 * (rdma_fail_streak > 0 ? rdma_fail_streak : 1);
        if (delay_ms < 100) delay_ms = 100;
        if (delay_ms > 1000) delay_ms = 1000;
        usleep((useconds_t)delay_ms * 1000);
    } else sleep(1);
}

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

        int fd = repl_transport_ops()->connect_slave(host, port);
        if (fd < 0) {
            if (!strcasecmp(repl_transport_name(), "rdma")) repl_rdma_log("slave_loop", "link down because connect_slave failed");
            repl_set_link_state(0);
            rdma_fail_streak++;
            repl_slave_retry_pause(rdma_fail_streak);
            continue;
        }

        if (!strcasecmp(repl_transport_name(), "rdma")) {
            unsigned char cmd[256];
            unsigned char stream_buf[BUFFER_CAP];
            char offbuf[32];
            size_t n;
            size_t blen;
            size_t stream_len = 0;
            int recv_slot;
            snprintf(offbuf, sizeof(offbuf), "%llu", g_slave_repl_offset);
            n = resp_build_cmd3(cmd, sizeof(cmd), "REPLSYNC", g_slave_master_replid[0] ? g_slave_master_replid : "?", offbuf);
            if (repl_transport_send(NULL, cmd, n) != 0) {
                repl_rdma_log("slave_loop", "link down because initial REPLSYNC send failed");
                repl_transport_ops()->disconnect_slave(fd);
                repl_set_link_state(0);
                rdma_fail_streak++;
                repl_slave_retry_pause(rdma_fail_streak);
                continue;
            }
            rdma_fail_streak = 0;
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
                    memcpy(stream_buf + stream_len, payload, blen);
                    stream_len += blen;
                    kvs_free(payload);
                    parse_resp_stream(NULL, stream_buf, &stream_len, 1);
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
            while (!slave_should_reconnect(gen) && g_repl_rdma_ctx.connected) {
                sleep(1);
                repl_set_link_state(1);
            }
            if (slave_should_reconnect(gen)) repl_rdma_log("slave_loop", "link down because reconnect was requested");
            else if (!g_repl_rdma_ctx.connected) repl_rdma_log("slave_loop", "link down because transport remained disconnected after recv loop");
            repl_transport_ops()->disconnect_slave(fd);
            repl_set_link_state(0);
            if (!g_repl_rdma_ctx.connected) rdma_fail_streak++;
            else rdma_fail_streak = 0;
            repl_slave_retry_pause(rdma_fail_streak);
            continue;
        }

        unsigned char cmd[256];
        char offbuf[32];
        snprintf(offbuf, sizeof(offbuf), "%llu", g_slave_repl_offset);
        size_t n = resp_build_cmd3(cmd, sizeof(cmd), "REPLSYNC", g_slave_master_replid[0] ? g_slave_master_replid : "?", offbuf);
        if (send(fd, cmd, n, 0) < 0) {
            repl_transport_ops()->disconnect_slave(fd);
            repl_set_link_state(0);
            sleep(1);
            continue;
        }

        repl_set_link_state(1);
        unsigned char buf[BUFFER_CAP];
        size_t blen = 0;

        for (;;) {
            if (slave_should_reconnect(gen)) break;
            ssize_t r = recv(fd, buf + blen, sizeof(buf) - blen, 0);
            if (r > 0) {
                blen += (size_t)r;
                parse_resp_stream(NULL, buf, &blen, 1);
                repl_set_link_state(1);
                continue;
            }
            if (r == 0) break;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }

        repl_transport_ops()->disconnect_slave(fd);
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
    for (;;) {
        long long accept_start_ms = 0;
        long long initial_recv_start_ms = 0;
        if (g_cfg.role != ROLE_MASTER || strcasecmp(g_cfg.repl_transport_backend, "rdma") != 0) {
            repl_rdma_reset_ctx();
            sleep(1);
            continue;
        }
        if (!g_repl_rdma_ctx.ec) {
            g_repl_rdma_ctx.ec = rdma_create_event_channel();
            if (!g_repl_rdma_ctx.ec) {
                repl_rdma_log("listener", "event channel create failed");
                sleep(1);
                continue;
            }
        }
        if (!g_repl_rdma_ctx.listen_id) {
            repl_rdma_drop_master_replica_shallow(&g_rdma_master_replica_conn);
            if (rdma_create_id(g_repl_rdma_ctx.ec, &g_repl_rdma_ctx.listen_id, NULL, RDMA_PS_TCP) != 0) {
                repl_rdma_log("listener", "create listen id failed");
                repl_rdma_reset_conn_ctx(1);
                sleep(1);
                continue;
            }
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons((uint16_t)g_cfg.port);
            if (inet_pton(AF_INET, g_cfg.master_host[0] ? g_cfg.master_host : "0.0.0.0", &addr.sin_addr) <= 0) addr.sin_addr.s_addr = htonl(INADDR_ANY);
            if (rdma_bind_addr(g_repl_rdma_ctx.listen_id, (struct sockaddr *)&addr) != 0) {
                repl_rdma_log("listener", "bind failed");
                repl_rdma_reset_conn_ctx(1);
                sleep(1);
                continue;
            }
            if (rdma_listen(g_repl_rdma_ctx.listen_id, 4) != 0) {
                repl_rdma_log("listener", "listen failed");
                repl_rdma_reset_conn_ctx(1);
                sleep(1);
                continue;
            }
            repl_rdma_log("listener", "listening");
        }
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
        {
            int recv_slot = -1;
            size_t recv_len = 0;
            memset(&g_rdma_master_replica_conn, 0, sizeof(g_rdma_master_replica_conn));
            initial_recv_start_ms = kvs_now_ms();
            if (repl_rdma_wait_cq_recv_completion(2000, &recv_slot, &recv_len) == 0 && recv_slot >= 0 && recv_len > 0) {
                size_t blen = recv_len;
                unsigned char *payload;
                if (blen > g_repl_rdma_ctx.recv_slots[recv_slot].cap) blen = g_repl_rdma_ctx.recv_slots[recv_slot].cap;
                payload = repl_rdma_dup_recv_payload(recv_slot, blen);
                if (!payload) {
                    repl_rdma_log("listener", "failed to copy recv payload");
                } else {
                    if (repl_rdma_repost_recv(recv_slot) != 0) {
                        repl_rdma_log("listener", "failed to repost recv");
                    }
                    parse_resp_stream(&g_rdma_master_replica_conn, payload, &blen, 0);
                    kvs_free(payload);
                    fprintf(stderr, "repl rdma: listener_initial_payload_ok - elapsed_ms=%lld recv_len=%zu\n", kvs_now_ms() - initial_recv_start_ms, recv_len);
                    repl_rdma_log("listener", "processed initial rdma payload");
                }
            } else {
                fprintf(stderr, "repl rdma: listener_initial_payload_fail - elapsed_ms=%lld recv_slot=%d recv_len=%zu\n", kvs_now_ms() - initial_recv_start_ms, recv_slot, recv_len);
                repl_rdma_log("listener", "no initial rdma payload received");
            }
        }
        while (g_cfg.role == ROLE_MASTER && !strcasecmp(g_cfg.repl_transport_backend, "rdma") && g_repl_rdma_ctx.connected) {
            sleep(1);
        }
        repl_rdma_log("listener", g_repl_rdma_ctx.connected ? "listener loop exiting while still marked connected" : "listener loop exiting after disconnect");
        repl_rdma_reset_conn_ctx(1);
    }
    return NULL;
}
#endif

int start_rdma_master_listener(void) {
#if KVS_ENABLE_RDMA
    pthread_t tid;
    if (g_cfg.role != ROLE_MASTER || strcasecmp(g_cfg.repl_transport_backend, "rdma") != 0) return 0;
    if (g_rdma_master_listener_started) return 0;
    if (pthread_create(&tid, NULL, rdma_master_listener_thread, NULL) != 0) return -1;
    pthread_detach(tid);
    g_rdma_master_listener_started = 1;
#endif
    return 0;
}
