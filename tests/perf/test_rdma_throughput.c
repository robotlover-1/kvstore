/*
 * test_rdma_throughput.c — RDMA SEND/RECV 吞吐量测试 (rdma_cm 版本)
 *
 * v3: 对齐生产 kvs_repl.c 全量同步路径：
 *   - 默认 IBV_WR_SEND（与生产一致，WRITE 仍可通过 --mode write 测试）
 *   - QP depth = 64（与生产 KVS_RDMA_QP_WR_DEPTH_DEFAULT=64 一致）
 *   - 独立 send slots + 独立 recv slots（每个 slot 独占 buffer/MR）
 *   - 每条消息携带 seq/len header，接收端校验
 *   - FIN/ACK 握手：以远端确认完成作为端到端计时边界
 *   - 两个吞吐量指标：send-local（WR completion）和 e2e（ACK 到达）
 *   - Per-WC 状态校验
 *
 * 用法:
 *   # 服务端（接收方）
 *   ./test_rdma_throughput --server --port 18516
 *
 *   # 客户端（发送方，默认 SEND 模式，对齐生产）
 *   ./test_rdma_throughput --host <server_ip> --port 18516 \
 *       --size 65536 --iters 5000
 *
 *   # RDMA WRITE 微基准（非生产模式）
 *   ./test_rdma_throughput --host <server_ip> --port 18516 \
 *       --mode write --size 65536 --iters 5000
 *
 * 依赖: libibverbs, librdmacm
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <ifaddrs.h>
#include <infiniband/verbs.h>
#include <net/if.h>
#include <netdb.h>
#include <pthread.h>
#include <rdma/rdma_cma.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "common.h"

/* ========== 配置（对齐生产 kvs_repl.c） ========== */
#define DEFAULT_PORT          18516
#define DEFAULT_SIZE          65536
#define DEFAULT_ITERS         5000
#define DEFAULT_MODE          "send"       /* v3: 默认 SEND，对齐生产 */
#define DEFAULT_SEND_SLOTS    16           /* v4: 对齐生产 pipeline=16 */
#define DEFAULT_RECV_SLOTS    64           /* v4: 对齐生产 recv_slots=64 */
#define DEFAULT_QP_DEPTH      64           /* 测试用；生产 min_depth = recv_slots*2 ≥ 128 */
#define DEFAULT_QP_COUNT      1            /* 默认单 QP；>1 启多 QP 并行 */

/* 多 QP 线程上下文 */
typedef struct {
    int qp_idx;
    const char *host;
    int port;
    size_t buf_size;
    int iters;
    int send_slot_count;
    int qp_depth;
    /* 返回值 */
    double throughput_send_bps;
    double throughput_ack_bps;
    int actual_iters;
    int wc_completed;
    int wc_errors;
    int ack_received;
    int thread_ok;
} qp_thread_ctx_t;

#define BATCH_MAX              8           /* v4: 对齐生产 KVS_RDMA_BATCH_MAX */
#define SIGNAL_INTERVAL        8           /* v4: 对齐生产 KVS_RDMA_SIGNAL_INTERVAL */
#define MAX_BATCH_TRACKERS    64           /* v4: 对齐生产 KVS_RDMA_MAX_BATCH_TRACKERS */
#define PIPELINE_WR_ID_FLAG    0x80000000UL

#define FIN_SEQ               UINT64_MAX   /* FIN 控制消息的 seq 标记 */

/* 消息头（每条 SEND 消息前 16 字节） */
typedef struct {
    uint64_t seq;
    uint32_t payload_len;
    uint32_t crc;             /* 预留给 CRC32C，当前填 0 */
} __attribute__((packed)) msg_hdr_t;

#define HDR_SIZE  sizeof(msg_hdr_t)

/* ACK 消息（服务端 → 客户端，FIN 之后） */
typedef struct {
    uint64_t last_seq;        /* 最后收到的数据 seq */
    uint64_t total_bytes;     /* 收到的 payload 总字节数（不含 header） */
    uint64_t msg_count;       /* 收到的消息数（含 FIN） */
    uint64_t seq_errors;      /* 序号不连续次数 */
} ack_msg_t;

/* rdma_cm private_data 交换 MR 信息 */
typedef struct {
    uint64_t addr;
    uint32_t rkey;
} mr_info_t;

/* v4: batch tracker — 对齐生产 rdma_batch_tracker_t */
typedef struct {
    int slots[BATCH_MAX];
    int count;
    int signaled_slot;
    int active;
} batch_tracker_t;

/* 发送槽位 */
typedef struct {
    struct ibv_mr *mr;
    unsigned char *buf;       /* [HDR_SIZE + payload_size] */
    size_t cap;               /* 总 buffer 大小 */
    volatile int in_flight;
    uint64_t seq;
} send_slot_t;

/* 接收槽位 */
typedef struct {
    struct ibv_mr *mr;
    unsigned char *buf;       /* [HDR_SIZE + payload_size] */
    size_t cap;
    volatile int posted;      /* RECV WR 已投递 */
} recv_slot_t;

/* 连接资源 */
typedef struct {
    struct rdma_event_channel *ec;
    struct rdma_cm_id *id;
    struct rdma_cm_id *listen_id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;

    mr_info_t local_mr;
    mr_info_t remote_mr;

    /* send slots（客户端用） */
    send_slot_t *send_slots;
    int send_slot_count;
    volatile int send_slots_in_flight;

    /* recv slots（服务端用） */
    recv_slot_t *recv_slots;
    int recv_slot_count;

    size_t buf_size;          /* payload 大小（--size） */
    int qp_depth;

    volatile int connected;

    /* v4: batch + tracker — 对齐生产 kvs_repl.c */
    batch_tracker_t batch_trackers[MAX_BATCH_TRACKERS];
    int batch_tracker_next;
    int pending_batch_count;
    int pending_batch_slots[BATCH_MAX];
    size_t pending_batch_lens[BATCH_MAX];

    /* 统计 */
    volatile int send_completed;
    volatile int send_wc_errors;
} rdma_res_t;

/* ========== rdma_cm 事件等待 ========== */
static int wait_cm_event(struct rdma_event_channel *ec,
                         enum rdma_cm_event_type expected,
                         struct rdma_cm_event **out_event,
                         int timeout_ms) {
    struct rdma_cm_event *event = NULL;
    struct pollfd pfd;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = ec->fd;
    pfd.events = POLLIN;

    if (poll(&pfd, 1, timeout_ms) <= 0) {
        fprintf(stderr, "[cm] poll timeout waiting for %s\n",
                rdma_event_str(expected));
        return -1;
    }

    if (rdma_get_cm_event(ec, &event) != 0) return -1;
    if (event->event != expected) {
        fprintf(stderr, "[cm] unexpected event: got=%s expect=%s\n",
                rdma_event_str(event->event), rdma_event_str(expected));
        rdma_ack_cm_event(event);
        return -1;
    }
    *out_event = event;
    return 0;
}

/* ========== CRC（当前用简单校验，后续可替换 CRC32C） ========== */
static uint32_t simple_crc(const unsigned char *data, size_t len) {
    uint32_t c = 0;
    for (size_t i = 0; i < len; i++)
        c = ((c << 1) | (c >> 31)) ^ data[i];
    return c;
}

/* ========== v4: 对齐生产 — batch post + selective signal + batch tracker ========== */

static int flush_batch(rdma_res_t *r);

/* 释放单个 slot（对齐 repl_rdma_release_send_slot） */
static void release_send_slot(rdma_res_t *r, int slot) {
    if (slot >= 0 && slot < r->send_slot_count) {
        r->send_slots[slot].in_flight = 0;
        __sync_fetch_and_sub(&r->send_slots_in_flight, 1);
    }
}

/* CQ poll + batch tracker 批量回收（对齐 repl_rdma_cq_process_wc） */
static void cq_poll_and_release(rdma_res_t *r, int max_wc) {
    struct ibv_wc wc[32];
    int n = ibv_poll_cq(r->cq, max_wc > 32 ? 32 : max_wc, wc);
    for (int j = 0; j < n; j++) {
        if (wc[j].status != IBV_WC_SUCCESS) {
            __sync_fetch_and_add(&r->send_wc_errors, 1);
            continue;
        }
        if (wc[j].opcode == IBV_WC_SEND) {
            int signaled_slot = (int)(wc[j].wr_id & ~PIPELINE_WR_ID_FLAG);
            /* 对齐生产：查找 batch tracker，批量回收 unsignaled slot */
            int batch_found = 0;
            for (int t = 0; t < MAX_BATCH_TRACKERS; t++) {
                batch_tracker_t *tr = &r->batch_trackers[t];
                if (tr->active && tr->signaled_slot == signaled_slot) {
                    for (int s = 0; s < tr->count; s++)
                        release_send_slot(r, tr->slots[s]);
                    tr->active = 0;
                    batch_found = 1;
                    r->send_completed += tr->count;
                    break;
                }
            }
            if (!batch_found)
                release_send_slot(r, signaled_slot);
        }
    }
}

/* 获取空闲 send slot（对齐 repl_rdma_acquire_send_slot） */
static int acquire_send_slot(rdma_res_t *r) {
    for (;;) {
        if (!r->connected) return -1;
        for (int i = 0; i < r->send_slot_count; i++) {
            if (!r->send_slots[i].in_flight) {
                r->send_slots[i].in_flight = 1;
                __sync_fetch_and_add(&r->send_slots_in_flight, 1);
                return i;
            }
        }
        /* flush 残量 batch（send_slots < BATCH_MAX 时 batch 永不满） */
        if (r->pending_batch_count > 0)
            flush_batch(r);
        /* 无空闲 slot → poll CQ（对齐生产内联 poll） */
        cq_poll_and_release(r, 16);
        if (r->send_slots_in_flight >= r->send_slot_count)
            usleep(50);  /* 所有 slot 在飞，短暂等 */
    }
}

/* 批量 flush：链式 WR + 选择性 signal + batch tracker（对齐 repl_rdma_flush_batch_locked） */
static int flush_batch(rdma_res_t *r) {
    int count = r->pending_batch_count;
    if (count == 0) return 0;
    struct ibv_sge sge[BATCH_MAX];
    struct ibv_send_wr wr[BATCH_MAX];
    struct ibv_send_wr *bad_wr = NULL;

    /* 申请 batch tracker */
    int tracker_idx = -1;
    for (int t = 0; t < MAX_BATCH_TRACKERS; t++) {
        int idx = (r->batch_tracker_next + t) % MAX_BATCH_TRACKERS;
        if (!r->batch_trackers[idx].active) {
            tracker_idx = idx;
            r->batch_tracker_next = (idx + 1) % MAX_BATCH_TRACKERS;
            break;
        }
    }
    int signal_every = (tracker_idx >= 0) ? SIGNAL_INTERVAL : 1;
    int signaled_slot = -1;

    for (int i = 0; i < count; i++) {
        int slot = r->pending_batch_slots[i];
        memset(&sge[i], 0, sizeof(sge[i]));
        sge[i].addr = (uintptr_t)r->send_slots[slot].buf;
        sge[i].length = (uint32_t)r->pending_batch_lens[i];
        sge[i].lkey = r->send_slots[slot].mr->lkey;
        memset(&wr[i], 0, sizeof(wr[i]));
        wr[i].wr_id = (uint64_t)slot | PIPELINE_WR_ID_FLAG;
        wr[i].sg_list = &sge[i];
        wr[i].num_sge = 1;
        wr[i].opcode = IBV_WR_SEND;
        /* 对齐生产：仅每 signal_every 个 WR 中最后一个 signaled */
        int do_signal = ((i + 1) % signal_every == 0) || (i == count - 1);
        wr[i].send_flags = do_signal ? IBV_SEND_SIGNALED : 0;
        if (do_signal) signaled_slot = slot;
        wr[i].next = (i < count - 1) ? &wr[i + 1] : NULL;
        if (tracker_idx >= 0 && i < BATCH_MAX)
            r->batch_trackers[tracker_idx].slots[i] = slot;
    }
    if (tracker_idx >= 0) {
        r->batch_trackers[tracker_idx].count = count;
        r->batch_trackers[tracker_idx].signaled_slot = signaled_slot;
        r->batch_trackers[tracker_idx].active = 1;
    }

    if (ibv_post_send(r->id->qp, &wr[0], &bad_wr) != 0) {
        if (tracker_idx >= 0) r->batch_trackers[tracker_idx].active = 0;
        for (int i = 0; i < count; i++)
            release_send_slot(r, r->pending_batch_slots[i]);
        r->pending_batch_count = 0;
        return -1;
    }
    r->pending_batch_count = 0;
    return 0;
}

/* drain 所有 send completion（对齐生产，使用 batch tracker） */
static void drain_send_cq(rdma_res_t *r) {
    while (r->send_slots_in_flight > 0) {
        cq_poll_and_release(r, 32);
        if (r->send_slots_in_flight > 0)
            usleep(100);
    }
}

/* ========== CRC32C 校验函数 ========== */
static uint32_t crc32c_calc(const unsigned char *data, size_t len) {
    (void)data;
    (void)len;
    return 0;  /* 预留，后续可用 aarch64 CRC32C 或软件实现 */
}

/* ========== 填充消息 ========== */
static void fill_msg(send_slot_t *slot, uint64_t seq, size_t payload_len) {
    msg_hdr_t *hdr = (msg_hdr_t *)slot->buf;
    hdr->seq = seq;
    hdr->payload_len = (uint32_t)payload_len;
    /* CRC over header fields (exclude crc field itself) */
    hdr->crc = simple_crc((unsigned char *)hdr, offsetof(msg_hdr_t, crc));
    /* fill payload with pattern 'R' */
    memset(slot->buf + HDR_SIZE, 'R', payload_len);
}

/* ========== 服务端 ========== */
static int run_server(int port, size_t buf_size,
                      int recv_slot_count, int qp_depth) {
    rdma_res_t r;
    struct rdma_cm_event *event = NULL;
    int ret = -1;
    size_t slot_size = HDR_SIZE + buf_size;
    /* P0: 独立 ACK send buffer（声明在顶部，cleanup 可安全访问） */
    unsigned char *ack_send_buf = NULL;
    struct ibv_mr *ack_send_mr = NULL;

    memset(&r, 0, sizeof(r));
    r.buf_size = buf_size;
    r.recv_slot_count = recv_slot_count;
    r.qp_depth = qp_depth;

    /* recv slots 分配 */
    r.recv_slots = (recv_slot_t *)calloc((size_t)recv_slot_count, sizeof(recv_slot_t));
    if (!r.recv_slots) { perror("calloc recv_slots"); return -1; }

    /* rdma_cm setup */
    r.ec = rdma_create_event_channel();
    if (!r.ec) { perror("rdma_create_event_channel"); goto cleanup; }

    if (rdma_create_id(r.ec, &r.listen_id, NULL, RDMA_PS_TCP) != 0) {
        perror("rdma_create_id (listen)"); goto cleanup;
    }

    struct sockaddr_in addr = { .sin_family = AF_INET,
                                .sin_addr.s_addr = htonl(INADDR_ANY),
                                .sin_port = htons((uint16_t)port) };
    if (rdma_bind_addr(r.listen_id, (struct sockaddr *)&addr) != 0) {
        perror("rdma_bind_addr"); goto cleanup;
    }
    if (rdma_listen(r.listen_id, 1) != 0) {
        perror("rdma_listen"); goto cleanup;
    }
    printf("[server] rdma_cm listening on port %d (recv_slots=%d qp_depth=%d)\n",
           port, recv_slot_count, qp_depth);
    fflush(stdout);

    if (wait_cm_event(r.ec, RDMA_CM_EVENT_CONNECT_REQUEST, &event, 120000) != 0) {
        goto cleanup;
    }
    r.id = event->id;
    rdma_ack_cm_event(event);
    event = NULL;

    /* 分配资源 */
    r.pd = ibv_alloc_pd(r.id->verbs);
    if (!r.pd) { perror("ibv_alloc_pd"); goto cleanup; }

    r.cq = ibv_create_cq(r.id->verbs, qp_depth, NULL, NULL, 0);
    if (!r.cq) { perror("ibv_create_cq"); goto cleanup; }

    struct ibv_qp_init_attr qp_attr = {
        .send_cq = r.cq,
        .recv_cq = r.cq,
        .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = (uint32_t)qp_depth,
                  .max_recv_wr = (uint32_t)qp_depth,
                  .max_send_sge = 1,
                  .max_recv_sge = 1 },
    };
    if (rdma_create_qp(r.id, r.pd, &qp_attr) != 0) {
        perror("rdma_create_qp"); goto cleanup;
    }

    /* 分配 + 注册独立的 recv slots */
    for (int i = 0; i < recv_slot_count; i++) {
        r.recv_slots[i].buf = (unsigned char *)aligned_alloc(4096, slot_size);
        if (!r.recv_slots[i].buf) { perror("aligned_alloc recv"); goto cleanup; }
        memset(r.recv_slots[i].buf, 0, slot_size);
        r.recv_slots[i].cap = slot_size;

        r.recv_slots[i].mr = ibv_reg_mr(r.pd, r.recv_slots[i].buf, slot_size,
                                        IBV_ACCESS_LOCAL_WRITE);
        if (!r.recv_slots[i].mr) {
            perror("ibv_reg_mr (recv slot)"); goto cleanup;
        }
    }

    /* 使用第一个 recv slot 的 MR 地址给对方做 private_data 交换（FOR WRITE MODE ONLY） */
    r.local_mr.addr = (uint64_t)(uintptr_t)r.recv_slots[0].buf;
    r.local_mr.rkey = r.recv_slots[0].mr->rkey;

    /* accept */
    struct rdma_conn_param param;
    memset(&param, 0, sizeof(param));
    param.initiator_depth = 1;
    param.responder_resources = 1;
    param.rnr_retry_count = 3;
    param.private_data = &r.local_mr;
    param.private_data_len = sizeof(r.local_mr);

    if (rdma_accept(r.id, &param) != 0) {
        perror("rdma_accept"); goto cleanup;
    }

    if (wait_cm_event(r.ec, RDMA_CM_EVENT_ESTABLISHED, &event, 5000) != 0) {
        goto cleanup;
    }
    if (event->param.conn.private_data_len >= sizeof(mr_info_t)) {
        memcpy(&r.remote_mr, event->param.conn.private_data, sizeof(mr_info_t));
    }
    rdma_ack_cm_event(event);
    event = NULL;
    r.connected = 1;
    printf("[server] RDMA 连接已建立 (recv_slots=%d, slot_size=%zu)\n",
           recv_slot_count, slot_size);
    fflush(stdout);

    /* P0: 独立 ACK send buffer — 不复用 recv_slots[0]，避免与接收数据并发覆盖 */
    ack_send_buf = aligned_alloc(4096, sizeof(ack_msg_t));
    if (ack_send_buf) {
        ack_send_mr = ibv_reg_mr(r.pd, ack_send_buf, sizeof(ack_msg_t),
                                 IBV_ACCESS_LOCAL_WRITE);
    }

    /* 预投递所有 RECV WR */
    for (int i = 0; i < recv_slot_count; i++) {
        struct ibv_sge sge = {
            .addr = (uint64_t)(uintptr_t)r.recv_slots[i].buf,
            .length = (uint32_t)slot_size,
            .lkey = r.recv_slots[i].mr->lkey };
        struct ibv_recv_wr recv_wr = {
            .wr_id = (uint64_t)i, .sg_list = &sge, .num_sge = 1 };
        struct ibv_recv_wr *bad_wr = NULL;
        if (ibv_post_recv(r.id->qp, &recv_wr, &bad_wr) == 0) {
            r.recv_slots[i].posted = 1;
        }
    }

    /* 接收循环 */
    uint64_t msg_count = 0, payload_bytes = 0, seq_errors = 0;
    int64_t last_seq = -1;
    int fin_received = 0;

    while (r.connected) {
        struct ibv_wc wc[16];
        int n = ibv_poll_cq(r.cq, 16, wc);
        for (int i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                if (wc[i].status == IBV_WC_WR_FLUSH_ERR) {
                    r.connected = 0; break;
                }
                continue;
            }
            if (wc[i].opcode == IBV_WC_RECV || wc[i].opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
                int slot_idx = (int)wc[i].wr_id;
                if (slot_idx < 0 || slot_idx >= recv_slot_count) continue;

                /* 校验消息头 */
                if (wc[i].byte_len < HDR_SIZE) {
                    seq_errors++;
                } else {
                    msg_hdr_t *hdr = (msg_hdr_t *)r.recv_slots[slot_idx].buf;
                    /* basic header validation */
                    if (hdr->payload_len > buf_size) seq_errors++;
                    payload_bytes += hdr->payload_len;
                    msg_count++;

                    if (hdr->seq == FIN_SEQ) {
                        fin_received = 1;
                    } else {
                        if (last_seq >= 0 && hdr->seq != (uint64_t)(last_seq + 1))
                            seq_errors++;
                        last_seq = (int64_t)hdr->seq;
                    }
                }

                /* FIN → 发送 ACK 后断开 */
                if (fin_received) {
                    ack_msg_t ack = {
                        .last_seq = (uint64_t)(last_seq >= 0 ? last_seq : 0),
                        .total_bytes = payload_bytes,
                        .msg_count = msg_count,
                        .seq_errors = seq_errors };
                    /* P0: 使用独立 ACK buffer，不复用 recv slot */
                    if (ack_send_buf && ack_send_mr) {
                        memcpy(ack_send_buf, &ack, sizeof(ack));
                    }
                    struct ibv_sge sge = {
                        .addr = (uint64_t)(uintptr_t)(ack_send_buf ? ack_send_buf : r.recv_slots[0].buf),
                        .length = sizeof(ack_msg_t),
                        .lkey = (ack_send_mr ? ack_send_mr : r.recv_slots[0].mr)->lkey };
                    struct ibv_send_wr wr = {
                        .wr_id = 0, .sg_list = &sge, .num_sge = 1,
                        .opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED };
                    struct ibv_send_wr *bad = NULL;
                    ibv_post_send(r.id->qp, &wr, &bad);

                    /* drain send CQ for the ACK */
                    struct ibv_wc swc;
                    while (ibv_poll_cq(r.cq, 1, &swc) == 0) usleep(100);

                    printf("[server] FIN 已处理，ACK 已发送\n");
                    printf("[server] msg_count=%lu payload_bytes=%lu seq_errors=%lu\n",
                           msg_count, payload_bytes, seq_errors);
                    r.connected = 0;
                    break;
                }

                /* repost 该 recv slot */
                r.recv_slots[slot_idx].posted = 0;
                struct ibv_sge sge = {
                    .addr = (uint64_t)(uintptr_t)r.recv_slots[slot_idx].buf,
                    .length = (uint32_t)slot_size,
                    .lkey = r.recv_slots[slot_idx].mr->lkey };
                struct ibv_recv_wr recv_wr = {
                    .wr_id = (uint64_t)slot_idx, .sg_list = &sge, .num_sge = 1 };
                struct ibv_recv_wr *bad_wr = NULL;
                if (ibv_post_recv(r.id->qp, &recv_wr, &bad_wr) == 0) {
                    r.recv_slots[slot_idx].posted = 1;
                }
            }
        }
        if (n == 0 && !fin_received) usleep(100);
    }

    ret = 0;

cleanup:
    if (event) rdma_ack_cm_event(event);
    if (r.id && r.id->qp) rdma_destroy_qp(r.id);
    if (r.cq) ibv_destroy_cq(r.cq);
    if (r.recv_slots) {
        for (int i = 0; i < recv_slot_count; i++) {
            if (r.recv_slots[i].mr) ibv_dereg_mr(r.recv_slots[i].mr);
            free(r.recv_slots[i].buf);
        }
        free(r.recv_slots);
    }
    if (ack_send_mr) ibv_dereg_mr(ack_send_mr);
    free(ack_send_buf);
    if (r.pd) ibv_dealloc_pd(r.pd);
    if (r.id) rdma_destroy_id(r.id);
    if (r.listen_id) rdma_destroy_id(r.listen_id);
    if (r.ec) rdma_destroy_event_channel(r.ec);
    return ret;
}

/* ========== 客户端 ========== */
static int run_client(const char *host, size_t buf_size, int iters,
                      const char *mode, int port,
                      int send_slot_count, int qp_depth,
                      qp_thread_ctx_t *ctx_out) {
    rdma_res_t r;
    struct rdma_cm_event *event = NULL;
    int ret = -1;
    size_t slot_size = HDR_SIZE + buf_size;

    memset(&r, 0, sizeof(r));
    r.buf_size = buf_size;
    r.send_slot_count = send_slot_count;
    r.qp_depth = qp_depth;

    r.send_slots = (send_slot_t *)calloc((size_t)send_slot_count, sizeof(send_slot_t));
    if (!r.send_slots) { perror("calloc send_slots"); return -1; }

    /* rdma_cm setup */
    r.ec = rdma_create_event_channel();
    if (!r.ec) { perror("rdma_create_event_channel"); goto cleanup; }

    if (rdma_create_id(r.ec, &r.id, NULL, RDMA_PS_TCP) != 0) {
        perror("rdma_create_id"); goto cleanup;
    }

    /* 解析地址 */
    struct sockaddr_in addr = { .sin_family = AF_INET,
                                .sin_port = htons((uint16_t)port) };
    {
        int addr_ok = 0;
        if (!strcmp(host, "127.0.0.1") || !strcmp(host, "localhost")) {
            struct ifaddrs *ifaddr = NULL;
            if (getifaddrs(&ifaddr) == 0) {
                for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
                    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
                    if ((ifa->ifa_flags & IFF_UP) == 0) continue;
                    if (ifa->ifa_flags & IFF_LOOPBACK) continue;
                    struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
                    if (sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK) || sin->sin_addr.s_addr == 0) continue;
                    addr.sin_addr = sin->sin_addr;
                    addr_ok = 1; break;
                }
                freeifaddrs(ifaddr);
            }
        }
        if (!addr_ok) {
            if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
                struct hostent *he = gethostbyname(host);
                if (!he) { fprintf(stderr, "解析主机 %s 失败\n", host); goto cleanup; }
                memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
            }
        }
    }

    if (rdma_resolve_addr(r.id, NULL, (struct sockaddr *)&addr, 2000) != 0) {
        perror("rdma_resolve_addr"); goto cleanup;
    }
    if (wait_cm_event(r.ec, RDMA_CM_EVENT_ADDR_RESOLVED, &event, 3000) != 0) goto cleanup;
    rdma_ack_cm_event(event); event = NULL;

    if (rdma_resolve_route(r.id, 2000) != 0) {
        perror("rdma_resolve_route"); goto cleanup;
    }
    if (wait_cm_event(r.ec, RDMA_CM_EVENT_ROUTE_RESOLVED, &event, 3000) != 0) goto cleanup;
    rdma_ack_cm_event(event); event = NULL;

    /* 分配资源 */
    r.pd = ibv_alloc_pd(r.id->verbs);
    if (!r.pd) { perror("ibv_alloc_pd"); goto cleanup; }

    r.cq = ibv_create_cq(r.id->verbs, qp_depth, NULL, NULL, 0);
    if (!r.cq) { perror("ibv_create_cq"); goto cleanup; }

    struct ibv_qp_init_attr qp_attr = {
        .send_cq = r.cq,
        .recv_cq = r.cq,
        .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = (uint32_t)qp_depth,
                  .max_recv_wr = (uint32_t)qp_depth,
                  .max_send_sge = 1,
                  .max_recv_sge = 1 },
    };
    if (rdma_create_qp(r.id, r.pd, &qp_attr) != 0) {
        perror("rdma_create_qp"); goto cleanup;
    }

    /* 分配 + 注册独立的 send slots */
    for (int i = 0; i < send_slot_count; i++) {
        r.send_slots[i].buf = (unsigned char *)aligned_alloc(4096, slot_size);
        if (!r.send_slots[i].buf) { perror("aligned_alloc send"); goto cleanup; }
        memset(r.send_slots[i].buf, 0, slot_size);
        r.send_slots[i].cap = slot_size;
        r.send_slots[i].in_flight = 0;

        int access = IBV_ACCESS_LOCAL_WRITE;
        if (strcmp(mode, "write") == 0)
            access |= IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

        r.send_slots[i].mr = ibv_reg_mr(r.pd, r.send_slots[i].buf, slot_size, access);
        if (!r.send_slots[i].mr) { perror("ibv_reg_mr (send slot)"); goto cleanup; }
    }

    r.local_mr.addr = (uint64_t)(uintptr_t)r.send_slots[0].buf;
    r.local_mr.rkey = r.send_slots[0].mr->rkey;

    /* ACK 接收 buffer（仅 SEND 模式需要） */
    unsigned char *ack_buf = NULL;
    struct ibv_mr *ack_mr = NULL;

    /* connect */
    struct rdma_conn_param param;
    memset(&param, 0, sizeof(param));
    param.initiator_depth = 1;
    param.responder_resources = 1;
    param.retry_count = 7;
    param.rnr_retry_count = 7;
    param.private_data = &r.local_mr;
    param.private_data_len = sizeof(r.local_mr);

    if (rdma_connect(r.id, &param) != 0) {
        perror("rdma_connect"); goto cleanup;
    }
    if (wait_cm_event(r.ec, RDMA_CM_EVENT_ESTABLISHED, &event, 5000) != 0) goto cleanup;
    if (event->param.conn.private_data_len >= sizeof(mr_info_t)) {
        memcpy(&r.remote_mr, event->param.conn.private_data, sizeof(mr_info_t));
    }
    rdma_ack_cm_event(event); event = NULL;
    r.connected = 1;

    printf("[client] RDMA 连接已建立 (slot_size=%zu send_slots=%d qp_depth=%d)\n",
           slot_size, send_slot_count, qp_depth);

    int is_write = (strcmp(mode, "write") == 0);
    int actual_iters = 0;

    /* P0: ACK RECV 预投递 — 建连后、计时前就 post，避免服务端 FIN→ACK 到达时 RQ 为空 */
    int uses_fin = !is_write;
    if (uses_fin) {
        ack_buf = (unsigned char *)aligned_alloc(4096, sizeof(ack_msg_t));
        if (ack_buf) {
            ack_mr = ibv_reg_mr(r.pd, ack_buf, sizeof(ack_msg_t),
                                IBV_ACCESS_LOCAL_WRITE);
        }
        if (ack_mr) {
            struct ibv_sge sge = {
                .addr = (uint64_t)(uintptr_t)ack_buf,
                .length = sizeof(ack_msg_t),
                .lkey = ack_mr->lkey };
            struct ibv_recv_wr recv_wr = {
                .wr_id = 0xAC0000, .sg_list = &sge, .num_sge = 1 };
            struct ibv_recv_wr *bad = NULL;
            ibv_post_recv(r.id->qp, &recv_wr, &bad);
        }
    }

    /* 计时起点 */
    double t0_send = now_us();

    /* v4: 发送循环 — 对齐生产 batch post + selective signal */
    for (int i = 0; i < iters; i++) {
        int slot_idx = acquire_send_slot(&r);
        if (slot_idx < 0) break;

        fill_msg(&r.send_slots[slot_idx], (uint64_t)i, buf_size);

        if (is_write) {
            /* WRITE 模式：逐 WR 立即 post（RDMA WRITE 不适用 batch tracker） */
            struct ibv_sge sge = {
                .addr = (uint64_t)(uintptr_t)r.send_slots[slot_idx].buf,
                .length = (uint32_t)slot_size,
                .lkey = r.send_slots[slot_idx].mr->lkey };
            struct ibv_send_wr send_wr = {
                .wr_id = (uint64_t)slot_idx, .sg_list = &sge, .num_sge = 1,
                .opcode = IBV_WR_RDMA_WRITE, .send_flags = IBV_SEND_SIGNALED };
            send_wr.wr.rdma.remote_addr = r.remote_mr.addr;
            send_wr.wr.rdma.rkey = r.remote_mr.rkey;
            struct ibv_send_wr *bad_wr = NULL;
            if (ibv_post_send(r.id->qp, &send_wr, &bad_wr) != 0) {
                release_send_slot(&r, slot_idx);
                cq_poll_and_release(&r, 8);
                i--; continue;  /* retry this iter */
            }
        } else {
            /* SEND 模式：积累到 batch，满时批量 flush（对齐生产） */
            int bi = r.pending_batch_count;
            r.pending_batch_slots[bi] = slot_idx;
            r.pending_batch_lens[bi] = slot_size;
            r.pending_batch_count++;
            if (r.pending_batch_count >= BATCH_MAX) {
                if (flush_batch(&r) != 0) break;
            }
        }
        actual_iters++;
    }
    /* flush 尾部 batch（SEND 模式） */
    if (!is_write && r.pending_batch_count > 0) {
        if (flush_batch(&r) != 0) { /* error, slots already released by flush_batch */ }
    }

    /* 发送 FIN（SEND 模式，走 batch 路径确保 FIN 在所有数据之后） */
    if (uses_fin) {
        /* flush 残量 batch 后再发 FIN */
        if (r.pending_batch_count > 0) flush_batch(&r);
        int slot_idx = acquire_send_slot(&r);
        if (slot_idx >= 0) {
            fill_msg(&r.send_slots[slot_idx], FIN_SEQ, 0);
            /* FIN 走 batch：积累后 flush（单 WR batch with signal） */
            int bi = r.pending_batch_count;
            r.pending_batch_slots[bi] = slot_idx;
            r.pending_batch_lens[bi] = HDR_SIZE;
            r.pending_batch_count++;
            flush_batch(&r);  /* 立即 flush FIN batch */
        }
    }

    /* drain 所有 send completion → 记录 send-local 时间 */
    drain_send_cq(&r);
    double t1_send = now_us();
    double elapsed_send_s = (t1_send - t0_send) / 1000000.0;
    double throughput_send_bps = (double)((size_t)actual_iters * buf_size * 8) / elapsed_send_s;

    /* P0: 等待 ACK（RECV 已在计时前预投递） */
    double t1_ack = t1_send;
    int ack_received = 0;
    if (uses_fin && ack_mr) {
        struct ibv_wc wc[4];
        int ack_retries = 0;
        while (ack_retries < 100) {
            int n = ibv_poll_cq(r.cq, 4, wc);
            for (int i = 0; i < n; i++) {
                if (wc[i].opcode == IBV_WC_RECV && wc[i].status == IBV_WC_SUCCESS) {
                    ack_received = 1;
                    break;
                }
            }
            if (ack_received) break;
            usleep(100000); ack_retries++;
        }
        t1_ack = now_us();
    }
    double elapsed_ack_s = (t1_ack - t0_send) / 1000000.0;
    double throughput_ack_bps = uses_fin
        ? (double)((size_t)actual_iters * buf_size * 8) / elapsed_ack_s
        : 0.0;

    /* 如果有 ctx_out，写回结果（多 QP 模式下使用） */
    if (ctx_out) {
        ctx_out->throughput_send_bps = throughput_send_bps;
        ctx_out->throughput_ack_bps = uses_fin ? throughput_ack_bps : 0;
        ctx_out->actual_iters = actual_iters;
        ctx_out->wc_completed = r.send_completed;
        ctx_out->wc_errors = r.send_wc_errors;
        ctx_out->ack_received = ack_received;
    }

    /* 打印结果 */
    if (!ctx_out) {
    printf("\n=== RDMA 吞吐量结果 ===\n");
    printf("  模式:       %s\n", is_write ? "RDMA_WRITE (非生产微基准)" : "RDMA_SEND (对齐生产)");
    printf("  payload:    %zu bytes (+%zu header)\n", buf_size, HDR_SIZE);
    printf("  send_slots: %d  recv_slots: (server)  qp_depth: %d\n",
           send_slot_count, qp_depth);
    printf("  iters:      %d (成功: %d)\n", iters, actual_iters);
    printf("  WC send完成: %d  WC错误: %d\n",
           r.send_completed, r.send_wc_errors);
    printf("  总数据量:   %.2f MB\n",
           (double)((size_t)actual_iters * buf_size) / (1024.0 * 1024.0));
    printf("\n");
    printf("  [send-local] 耗时: % .3f s  吞吐量: %s\n",
           elapsed_send_s, throughput_str(throughput_send_bps));
    if (uses_fin && ack_received) {
        printf("  [e2e (ACK)]  耗时: % .3f s  吞吐量: %s\n",
               elapsed_ack_s, throughput_str(throughput_ack_bps));
    } else if (uses_fin) {
        printf("  [e2e (ACK)]  ACK 未收到\n");
    } else {
        printf("  [e2e (ACK)]  WRITE 模式无远端确认\n");
    }
    } /* !ctx_out: suppress print in multi-QP mode */

    ret = 0;

cleanup:
    if (event) rdma_ack_cm_event(event);
    if (r.id && r.id->qp) rdma_destroy_qp(r.id);
    if (r.cq) ibv_destroy_cq(r.cq);
    if (r.send_slots) {
        for (int i = 0; i < send_slot_count; i++) {
            if (r.send_slots[i].mr) ibv_dereg_mr(r.send_slots[i].mr);
            free(r.send_slots[i].buf);
        }
        free(r.send_slots);
    }
    if (ack_mr) ibv_dereg_mr(ack_mr);
    free(ack_buf);
    if (r.pd) ibv_dealloc_pd(r.pd);
    if (r.id) rdma_destroy_id(r.id);
    if (r.ec) rdma_destroy_event_channel(r.ec);
    return ret;
}

/* ========== 多 QP 线程入口 ========== */
typedef struct { int port; size_t buf_size; int recv_slots; int qp_depth; } srv_thread_arg_t;

static void *run_server_thread(void *arg) {
    srv_thread_arg_t *a = (srv_thread_arg_t *)arg;
    int rc = run_server(a->port, a->buf_size, a->recv_slots, a->qp_depth);
    free(a);
    return (void *)(intptr_t)rc;
}

static void *run_client_thread(void *arg) {
    qp_thread_ctx_t *ctx = (qp_thread_ctx_t *)arg;
    int port = ctx->port + ctx->qp_idx;
    int rc = run_client(ctx->host, ctx->buf_size, ctx->iters, "send",
                        port, ctx->send_slot_count, ctx->qp_depth, ctx);
    ctx->thread_ok = (rc == 0) ? 1 : 0;
    return NULL;
}

/* ========== Main ========== */
static void usage(const char *prog) {
    fprintf(stderr,
        "用法:\n"
        "  服务端: %s --server [--port PORT] [--size SIZE] [--recv-slots N] [--qp-depth N]\n"
        "  客户端: %s --host <server_ip> [options]\n"
        "选项:\n"
        "  --host, -H     服务端 IP（客户端模式必需）\n"
        "  --server, -s   以服务端模式运行\n"
        "  --port, -p     rdma_cm 端口 (默认: %d)\n"
        "  --size           传输 payload 大小 (默认: %d)\n"
        "  --iters          传输次数 (默认: %d)\n"
        "  --mode           send/write (默认: %s, align w/ prod)\n"
        "  --send-slots    发送 pipeline 槽位 (默认: %d)\n"
        "  --recv-slots    接收槽位 (默认: %d)\n"
        "  --qp-depth      QP 深度 (默认: %d)\n"
        "  --help, -h      显示帮助\n",
        prog, prog,
        DEFAULT_PORT, DEFAULT_SIZE, DEFAULT_ITERS,
        DEFAULT_MODE, DEFAULT_SEND_SLOTS, DEFAULT_RECV_SLOTS, DEFAULT_QP_DEPTH);
}

int main(int argc, char **argv) {
    const char *host = NULL;
    int server_mode = 0;
    int port = DEFAULT_PORT;
    size_t buf_size = DEFAULT_SIZE;
    int iters = DEFAULT_ITERS;
    const char *mode = DEFAULT_MODE;
    int send_slot_count = DEFAULT_SEND_SLOTS;
    int recv_slot_count = DEFAULT_RECV_SLOTS;
    int qp_depth = DEFAULT_QP_DEPTH;
    int qp_count = DEFAULT_QP_COUNT;

    struct option long_opts[] = {
        {"host",       required_argument, 0, 'H'},
        {"server",     no_argument,       0, 's'},
        {"port",       required_argument, 0, 'p'},
        {"size",       required_argument, 0, 1000},
        {"iters",      required_argument, 0, 1001},
        {"mode",       required_argument, 0, 1002},
        {"send-slots", required_argument, 0, 1003},
        {"recv-slots", required_argument, 0, 1004},
        {"qp-depth",   required_argument, 0, 1005},
        {"qp-count",   required_argument, 0, 1006},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "H:sp:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'H': host = optarg; break;
        case 's': server_mode = 1; break;
        case 'p': port = atoi(optarg); break;
        case 1000: buf_size = (size_t)atol(optarg); break;
        case 1001: iters = atoi(optarg); break;
        case 1002: mode = optarg; break;
        case 1003: send_slot_count = atoi(optarg); break;
        case 1004: recv_slot_count = atoi(optarg); break;
        case 1005: qp_depth = atoi(optarg); break;
        case 1006: qp_count = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }

    if (!server_mode && !host) {
        fprintf(stderr, "错误: 客户端模式需要 --host\n");
        usage(argv[0]); return 1;
    }
    if (strcmp(mode, "write") != 0 && strcmp(mode, "send") != 0) {
        fprintf(stderr, "错误: mode 必须是 send 或 write\n");
        return 1;
    }

    if (server_mode) {
        if (qp_count <= 1) {
            return run_server(port, buf_size, recv_slot_count, qp_depth);
        }
        /* 多 QP 服务端：N 个线程并行 accept，避免顺序阻塞导致后面的 listener 超时 */
        pthread_t *sthreads = calloc((size_t)qp_count, sizeof(pthread_t));
        if (!sthreads) { perror("calloc"); return 1; }
        for (int qi = 0; qi < qp_count; qi++) {
            srv_thread_arg_t *sarg = malloc(sizeof(srv_thread_arg_t));
            sarg->port = port + qi;
            sarg->buf_size = buf_size;
            sarg->recv_slots = recv_slot_count;
            sarg->qp_depth = qp_depth;
            pthread_create(&sthreads[qi], NULL, run_server_thread, sarg);
        }
        int ok = 0;
        for (int qi = 0; qi < qp_count; qi++) {
            void *ret = NULL;
            pthread_join(sthreads[qi], &ret);
            if (ret == 0) ok++;
        }
        free(sthreads);
        return (ok == qp_count) ? 0 : 1;
    }

    /* 客户端：单 QP 或多 QP 并行 */
    if (qp_count <= 1) {
        return run_client(host, buf_size, iters, mode, port,
                          send_slot_count, qp_depth, NULL);
    }

    /* 多 QP 客户端 */
    qp_thread_ctx_t *ctxs = calloc((size_t)qp_count, sizeof(qp_thread_ctx_t));
    pthread_t *threads = calloc((size_t)qp_count, sizeof(pthread_t));
    if (!ctxs || !threads) { perror("calloc"); return 1; }

    for (int qi = 0; qi < qp_count; qi++) {
        ctxs[qi].qp_idx = qi;
        ctxs[qi].host = host;
        ctxs[qi].port = port;
        ctxs[qi].buf_size = buf_size;
        ctxs[qi].iters = iters / qp_count;
        ctxs[qi].send_slot_count = send_slot_count;
        ctxs[qi].qp_depth = qp_depth;
        pthread_create(&threads[qi], NULL, run_client_thread, &ctxs[qi]);
    }

    /* 等待所有线程，聚合结果 */
    double tput_sum = 0;
    int total_iters = 0, total_wc = 0, total_err = 0, all_ok = 1;
    for (int qi = 0; qi < qp_count; qi++) {
        pthread_join(threads[qi], NULL);
        total_iters += ctxs[qi].actual_iters;
        total_wc += ctxs[qi].wc_completed;
        total_err += ctxs[qi].wc_errors;
        if (!ctxs[qi].thread_ok) all_ok = 0;
        if (ctxs[qi].throughput_send_bps > 0)
            tput_sum += ctxs[qi].throughput_send_bps;
    }

    printf("\n=== 多 QP 聚合结果 (qp_count=%d) ===\n", qp_count);
    printf("  总 iters:   %d\n", total_iters);
    printf("  总 WC:      %d (errors=%d)\n", total_wc, total_err);
    printf("  聚合吞吐:   %s\n", throughput_str(tput_sum));
    for (int qi = 0; qi < qp_count; qi++) {
        printf("  QP%d: %s (iters=%d WC=%d)\n",
               qi, throughput_str(ctxs[qi].throughput_send_bps),
               ctxs[qi].actual_iters, ctxs[qi].wc_completed);
    }

    free(ctxs);
    free(threads);
    return all_ok ? 0 : 1;
}
