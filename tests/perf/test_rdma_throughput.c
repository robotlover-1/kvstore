/*
 * test_rdma_throughput.c — RDMA write/send 吞吐量测试 (rdma_cm 版本)
 *
 * 使用 rdma_cm (RDMA Connection Manager) 建立连接，
 * 与项目 kvs_repl.c 全量同步的 RDMA 路径一致。
 *
 * v2: 对齐生产代码 (kvs_repl.c) 的核心 RDMA 模式：
 *   - completion channel + ibv_get_cq_event 事件驱动
 *   - pipeline 发送 (4 槽位，fire-and-forget)
 *   - 独立 CQ 轮询线程 + re-arm → drain → wait
 *   - QP depth = 64 (与生产一致)
 *
 * 用法:
 *   # 服务端（接收方）
 *   ./test_rdma_throughput --server --port 18516
 *
 *   # 客户端（发送方）
 *   ./test_rdma_throughput --host <server_ip> --port 18516 \
 *       --mode write --size 65536 --iters 10000
 *
 * 依赖: libibverbs, librdmacm, libpthread
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

/* ========== 配置 ========== */
#define DEFAULT_PORT      18516
#define DEFAULT_SIZE      65536
#define DEFAULT_ITERS     5000
#define DEFAULT_MODE      "write"

/* ---- 对齐 kvs_repl.c ---- */
#define QP_WR_DEPTH             1024 /* 对齐原测试 */
#define PIPELINE_DEPTH          1    /* 单 buffer = 原代码模式 */
#define PIPELINE_WR_ID_FLAG     0x80000000UL
#define MAX_RECV_POST           256

/* 通过 rdma_cm private_data 交换 MR 信息 */
typedef struct {
    uint64_t addr;
    uint32_t rkey;
} mr_info_t;

/* pipeline 发送槽位（对齐 repl_rdma_send_slot_t） */
typedef struct {
    struct ibv_mr *mr;
    unsigned char *buf;
    size_t cap;
    volatile int in_flight;
} send_slot_t;

/* ========== RDMA 资源（rdma_cm 方式） ========== */
typedef struct {
    struct rdma_event_channel *ec;
    struct rdma_cm_id *id;
    struct rdma_cm_id *listen_id;  /* server only */
    struct ibv_pd *pd;
    struct ibv_mr *mr;             /* recv buffer MR (server) / fallback MR (client) */
    struct ibv_cq *cq;

    void *buf;
    size_t buf_size;

    mr_info_t local_mr;
    mr_info_t remote_mr;

    volatile int connected;

    /* ---- pipeline 发送（对齐 kvs_repl.c）---- */
    send_slot_t send_slots[PIPELINE_DEPTH];
    int send_pipeline_depth;

    /* ---- 统计 ---- */
    volatile int send_completed;
    volatile int recv_completed;
    volatile size_t recv_bytes;
} rdma_res_t;

/* ========== 等待 rdma_cm 事件 ========== */
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

/* ========== 释放 send slot（由 CQ poll thread 调用）========== */
static volatile int g_release_calls = 0;
static volatile int g_release_rejected = 0;
static void release_send_slot(rdma_res_t *r, int slot) {
    if (slot >= 0 && slot < PIPELINE_DEPTH) {
        r->send_slots[slot].in_flight = 0;
        __sync_fetch_and_add(&g_release_calls, 1);
    } else {
        __sync_fetch_and_add(&g_release_rejected, 1);
    }
}

/* ========== 获取空闲 send slot（内联 poll，对齐 kvs_repl.c fallback 路径）========== */
static int acquire_send_slot(rdma_res_t *r) {
    for (;;) {
        if (!r->connected) return -1;
        for (int i = 0; i < r->send_pipeline_depth; i++) {
            if (!r->send_slots[i].in_flight) {
                return i;
            }
        }
        /* 所有 slot 在飞 → 批量 poll CQ 回收 completion */
        struct ibv_wc wc[8];
        int n = ibv_poll_cq(r->cq, 8, wc);
        if (n > 0) {
            for (int j = 0; j < n; j++) {
                if (wc[j].status == IBV_WC_SUCCESS &&
                    (wc[j].opcode == IBV_WC_SEND || wc[j].opcode == IBV_WC_RDMA_WRITE)) {
                    if (wc[j].wr_id & PIPELINE_WR_ID_FLAG) {
                        release_send_slot(r, (int)(wc[j].wr_id & ~PIPELINE_WR_ID_FLAG));
                        r->send_completed++;
                    }
                }
            }
            continue;
        }
        /* 无 completion → 短暂等待 */
        usleep(10);
    }
}

/* ========== 服务端 ========== */
static int run_server(int port, size_t buf_size) {
    rdma_res_t r;
    struct rdma_cm_event *event = NULL;
    int ret = -1;

    memset(&r, 0, sizeof(r));
    r.buf_size = buf_size;
    r.send_pipeline_depth = PIPELINE_DEPTH;

    /* 1. 创建 event channel */
    r.ec = rdma_create_event_channel();
    if (!r.ec) {
        perror("rdma_create_event_channel");
        return -1;
    }

    /* 2. 创建 listen id */
    if (rdma_create_id(r.ec, &r.listen_id, NULL, RDMA_PS_TCP) != 0) {
        perror("rdma_create_id (listen)");
        goto cleanup;
    }

    /* 3. bind + listen */
    struct sockaddr_in addr = { .sin_family = AF_INET,
                                .sin_addr.s_addr = htonl(INADDR_ANY),
                                .sin_port = htons((uint16_t)port) };
    if (rdma_bind_addr(r.listen_id, (struct sockaddr *)&addr) != 0) {
        perror("rdma_bind_addr");
        goto cleanup;
    }
    if (rdma_listen(r.listen_id, 1) != 0) {
        perror("rdma_listen");
        goto cleanup;
    }
    printf("[server] rdma_cm listening on port %d\n", port);
    fflush(stdout);

    /* 4. 等待连接请求 */
    if (wait_cm_event(r.ec, RDMA_CM_EVENT_CONNECT_REQUEST, &event, 30000) != 0) {
        fprintf(stderr, "[server] timeout waiting for CONNECT_REQUEST\n");
        goto cleanup;
    }
    r.id = event->id;
    rdma_ack_cm_event(event);
    event = NULL;
    printf("[server] connect request received\n");
    fflush(stdout);

    /* 5. 分配资源（服务端用内联 poll，无需 comp_chan） */
    r.pd = ibv_alloc_pd(r.id->verbs);
    if (!r.pd) { perror("ibv_alloc_pd"); goto cleanup; }

    r.cq = ibv_create_cq(r.id->verbs, QP_WR_DEPTH, NULL, NULL, 0);
    if (!r.cq) { perror("ibv_create_cq"); goto cleanup; }

    struct ibv_qp_init_attr qp_attr = {
        .send_cq = r.cq,
        .recv_cq = r.cq,
        .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = QP_WR_DEPTH,
                  .max_recv_wr = QP_WR_DEPTH,
                  .max_send_sge = 1,
                  .max_recv_sge = 1 },
    };
    if (rdma_create_qp(r.id, r.pd, &qp_attr) != 0) {
        perror("rdma_create_qp");
        goto cleanup;
    }

    /* 7. 注册 MR */
    r.buf = aligned_alloc(4096, buf_size);
    if (!r.buf) { perror("aligned_alloc"); goto cleanup; }
    memset(r.buf, 0, buf_size);

    r.mr = ibv_reg_mr(r.pd, r.buf, buf_size,
                      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                          IBV_ACCESS_REMOTE_WRITE);
    if (!r.mr) { perror("ibv_reg_mr"); goto cleanup; }

    r.local_mr.addr = (uint64_t)(uintptr_t)r.buf;
    r.local_mr.rkey = r.mr->rkey;

    /* 8. rdma_accept（附带 MR 信息） */
    struct rdma_conn_param param;
    memset(&param, 0, sizeof(param));
    param.initiator_depth = 1;
    param.responder_resources = 1;
    param.rnr_retry_count = 3;
    param.private_data = &r.local_mr;
    param.private_data_len = sizeof(r.local_mr);

    if (rdma_accept(r.id, &param) != 0) {
        perror("rdma_accept");
        goto cleanup;
    }

    /* 9. 等待 ESTABLISHED */
    if (wait_cm_event(r.ec, RDMA_CM_EVENT_ESTABLISHED, &event, 5000) != 0) {
        fprintf(stderr, "[server] timeout waiting for ESTABLISHED\n");
        goto cleanup;
    }

    if (event->param.conn.private_data_len >= sizeof(mr_info_t)) {
        memcpy(&r.remote_mr, event->param.conn.private_data, sizeof(mr_info_t));
    }
    rdma_ack_cm_event(event);
    event = NULL;
    r.connected = 1;

    printf("[server] RDMA 连接已建立\n");
    printf("[server] 远端 MR: addr=0x%lx rkey=%u\n",
           r.remote_mr.addr, r.remote_mr.rkey);
    fflush(stdout);

    /* 10. Pre-post recv WRs */
    {
        struct ibv_sge sge = { .addr = (uint64_t)(uintptr_t)r.buf,
                                .length = (uint32_t)buf_size,
                                .lkey = r.mr->lkey };
        struct ibv_recv_wr recv_wr = { .wr_id = 0,
                                        .sg_list = &sge,
                                        .num_sge = 1 };
        struct ibv_recv_wr *bad_wr = NULL;
        for (int i = 0; i < MAX_RECV_POST; i++) {
            if (ibv_post_recv(r.id->qp, &recv_wr, &bad_wr) != 0) break;
        }
    }

    /* 11. 内联 poll CQ（服务端纯 sink，只收不检查） */
    while (r.connected) {
        struct ibv_wc wc[16];
        int n = ibv_poll_cq(r.cq, 16, wc);
        for (int i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                if (wc[i].status == IBV_WC_WR_FLUSH_ERR) {
                    r.connected = 0;
                    break;
                }
                continue;
            }
            if (wc[i].opcode == IBV_WC_RECV ||
                wc[i].opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
                r.recv_bytes += wc[i].byte_len;
                r.recv_completed++;
                /* 重新 post recv */
                struct ibv_sge sge = { .addr = (uint64_t)(uintptr_t)r.buf,
                                        .length = (uint32_t)buf_size,
                                        .lkey = r.mr->lkey };
                struct ibv_recv_wr recv_wr = { .wr_id = (uint64_t)r.recv_completed,
                                                .sg_list = &sge,
                                                .num_sge = 1 };
                struct ibv_recv_wr *bad_wr = NULL;
                ibv_post_recv(r.id->qp, &recv_wr, &bad_wr);
            }
        }
        if (n == 0) usleep(100);
    }

    printf("[server] 接收完成: %d msgs, %.2f MB\n",
           r.recv_completed, (double)r.recv_bytes / (1024.0 * 1024.0));
    ret = 0;

cleanup:
    if (event) rdma_ack_cm_event(event);
    if (r.id && r.id->qp) rdma_destroy_qp(r.id);
    if (r.cq) ibv_destroy_cq(r.cq);
    if (r.mr) ibv_dereg_mr(r.mr);
    if (r.buf) free(r.buf);
    if (r.pd) ibv_dealloc_pd(r.pd);
    if (r.id) rdma_destroy_id(r.id);
    if (r.listen_id) rdma_destroy_id(r.listen_id);
    if (r.ec) rdma_destroy_event_channel(r.ec);
    return ret;
}

/* ========== 客户端 ========== */
static int run_client(const char *host, size_t buf_size, int iters,
                      const char *mode, int port) {
    rdma_res_t r;
    struct rdma_cm_event *event = NULL;
    int ret = -1;

    memset(&r, 0, sizeof(r));
    r.buf_size = buf_size;
    r.send_pipeline_depth = PIPELINE_DEPTH;

    /* 1. 创建 event channel */
    r.ec = rdma_create_event_channel();
    if (!r.ec) {
        perror("rdma_create_event_channel");
        return -1;
    }

    /* 2. 创建 id */
    if (rdma_create_id(r.ec, &r.id, NULL, RDMA_PS_TCP) != 0) {
        perror("rdma_create_id");
        goto cleanup;
    }

    /* 3. 解析地址（remap loopback → 实际 IP，对齐 kvs_repl.c） */
    struct sockaddr_in addr = { .sin_family = AF_INET,
                                .sin_port = htons((uint16_t)port) };
    {
        int addr_ok = 0;

        /* Loopback remap: RDMA 不能走 lo，用实际网卡 IP */
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
                    {
                        char ipbuf[INET_ADDRSTRLEN] = {0};
                        inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));
                        fprintf(stderr, "[client] loopback remapped: %s → %s\n", host, ipbuf);
                    }
                    addr_ok = 1;
                    break;
                }
                freeifaddrs(ifaddr);
            }
            if (!addr_ok)
                fprintf(stderr, "[client] loopback remap failed, trying %s directly\n", host);
        }

        /* 直接解析（非 loopback 或 remap 失败时） */
        if (!addr_ok) {
            if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
                struct hostent *he = gethostbyname(host);
                if (!he) {
                    fprintf(stderr, "解析主机 %s 失败\n", host);
                    goto cleanup;
                }
                memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
            }
        }
    }

    if (rdma_resolve_addr(r.id, NULL, (struct sockaddr *)&addr, 2000) != 0) {
        perror("rdma_resolve_addr");
        goto cleanup;
    }

    if (wait_cm_event(r.ec, RDMA_CM_EVENT_ADDR_RESOLVED, &event, 3000) != 0) {
        fprintf(stderr, "ADDR_RESOLVED 超时或失败\n");
        goto cleanup;
    }
    rdma_ack_cm_event(event);
    event = NULL;

    /* 4. 解析路由 */
    if (rdma_resolve_route(r.id, 2000) != 0) {
        perror("rdma_resolve_route");
        goto cleanup;
    }

    if (wait_cm_event(r.ec, RDMA_CM_EVENT_ROUTE_RESOLVED, &event, 3000) != 0) {
        fprintf(stderr, "ROUTE_RESOLVED 超时或失败\n");
        goto cleanup;
    }
    rdma_ack_cm_event(event);
    event = NULL;

    /* 5. 分配资源（客户端用内联 poll，无需 comp_chan） */
    r.pd = ibv_alloc_pd(r.id->verbs);
    if (!r.pd) { perror("ibv_alloc_pd"); goto cleanup; }

    r.cq = ibv_create_cq(r.id->verbs, QP_WR_DEPTH, NULL, NULL, 0);
    if (!r.cq) { perror("ibv_create_cq"); goto cleanup; }

    struct ibv_qp_init_attr qp_attr = {
        .send_cq = r.cq,
        .recv_cq = r.cq,
        .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = QP_WR_DEPTH,
                  .max_recv_wr = QP_WR_DEPTH,
                  .max_send_sge = 1,
                  .max_recv_sge = 1 },
    };
    if (rdma_create_qp(r.id, r.pd, &qp_attr) != 0) {
        perror("rdma_create_qp");
        goto cleanup;
    }

    /* 7. 分配 pipeline send buffers + 注册 MR（对齐生产 repl_rdma_prepare_buffers） */
    for (int i = 0; i < PIPELINE_DEPTH; i++) {
        r.send_slots[i].buf = (unsigned char *)aligned_alloc(4096, buf_size);
        if (!r.send_slots[i].buf) {
            perror("aligned_alloc (send slot)");
            goto cleanup;
        }
        memset(r.send_slots[i].buf, 'R', buf_size);
        r.send_slots[i].cap = buf_size;
        r.send_slots[i].in_flight = 0;

        r.send_slots[i].mr = ibv_reg_mr(r.pd, r.send_slots[i].buf, buf_size,
                                        IBV_ACCESS_LOCAL_WRITE);
        if (!r.send_slots[i].mr) {
            perror("ibv_reg_mr (send slot)");
            goto cleanup;
        }
    }

    /* 8. 注册 MR（用于 rdma_cm private_data 交换） */
    r.buf = aligned_alloc(4096, buf_size);
    if (!r.buf) { perror("aligned_alloc"); goto cleanup; }
    memset(r.buf, 'R', buf_size);

    r.mr = ibv_reg_mr(r.pd, r.buf, buf_size,
                      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                          IBV_ACCESS_REMOTE_WRITE);
    if (!r.mr) { perror("ibv_reg_mr"); goto cleanup; }

    r.local_mr.addr = (uint64_t)(uintptr_t)r.buf;
    r.local_mr.rkey = r.mr->rkey;

    /* 9. rdma_connect（附带 MR 信息） */
    struct rdma_conn_param param;
    memset(&param, 0, sizeof(param));
    param.initiator_depth = 1;
    param.responder_resources = 1;
    param.retry_count = 7;
    param.rnr_retry_count = 7;
    param.private_data = &r.local_mr;
    param.private_data_len = sizeof(r.local_mr);

    if (rdma_connect(r.id, &param) != 0) {
        perror("rdma_connect");
        goto cleanup;
    }

    /* 10. 等待 ESTABLISHED */
    if (wait_cm_event(r.ec, RDMA_CM_EVENT_ESTABLISHED, &event, 5000) != 0) {
        fprintf(stderr, "ESTABLISHED 超时或失败\n");
        goto cleanup;
    }

    if (event->param.conn.private_data_len >= sizeof(mr_info_t)) {
        memcpy(&r.remote_mr, event->param.conn.private_data, sizeof(mr_info_t));
    }
    rdma_ack_cm_event(event);
    event = NULL;
    r.connected = 1;

    printf("[client] RDMA 连接已建立\n");
    printf("[client] 远端 MR: addr=0x%lx rkey=%u\n",
           r.remote_mr.addr, r.remote_mr.rkey);
    fflush(stdout);

    /* 11. 发送循环（对齐原代码：单 buffer，高 inflight，poll when needed） */
    int is_write = (strcmp(mode, "write") == 0);
    int actual_iters = 0;
    int inflight = 0;

    double t0 = now_us();

    for (int i = 0; i < iters; i++) {
        struct ibv_sge sge = {
            .addr = (uint64_t)(uintptr_t)r.send_slots[0].buf,
            .length = (uint32_t)buf_size,
            .lkey = r.send_slots[0].mr->lkey };
        struct ibv_send_wr send_wr = {
            .sg_list = &sge,
            .num_sge = 1,
            .send_flags = IBV_SEND_SIGNALED };
        struct ibv_send_wr *bad_wr = NULL;

        if (is_write) {
            send_wr.opcode = IBV_WR_RDMA_WRITE;
            send_wr.wr.rdma.remote_addr = r.remote_mr.addr;
            send_wr.wr.rdma.rkey = r.remote_mr.rkey;
        } else {
            send_wr.opcode = IBV_WR_SEND;
        }

        if (ibv_post_send(r.id->qp, &send_wr, &bad_wr) != 0) {
            /* QP send queue 满 → poll 回收 completion 后重试 */
            struct ibv_wc wc[32];
            int n = ibv_poll_cq(r.cq, 32, wc);
            if (n > 0) {
                inflight -= n;
                r.send_completed += n;
                if (ibv_post_send(r.id->qp, &send_wr, &bad_wr) != 0) {
                    fprintf(stderr, "[client] ibv_post_send 失败 iter=%d (retry)\n", i);
                    break;
                }
                inflight++;
                actual_iters++;
            } else {
                fprintf(stderr, "[client] ibv_post_send 失败 iter=%d (qp full)\n", i);
                break;
            }
        } else {
            inflight++;
            actual_iters++;
        }

        /* 周期性 poll：每 16 WR 或 inflight >= 256 时回收 */
        if (inflight >= 256 || (i % 16 == 0) || i == iters - 1) {
            struct ibv_wc wc[32];
            int total = 0;
            while (inflight > 512) {
                int n = ibv_poll_cq(r.cq, 32, wc);
                if (n <= 0) { usleep(10); continue; }
                inflight -= n;
                r.send_completed += n;
                total += n;
            }
            if (inflight > 512) {
                int n = ibv_poll_cq(r.cq, 64, wc);
                if (n > 0) { inflight -= n; r.send_completed += n; }
            }
        }
    }

    /* 12. Drain 所有 inflight */
    {
        while (inflight > 0 && r.connected) {
            struct ibv_wc wc[64];
            int n = ibv_poll_cq(r.cq, 64, wc);
            if (n <= 0) { usleep(100); continue; }
            inflight -= n;
            r.send_completed += n;
        }
    }

    double t1 = now_us();
    double elapsed_s = (t1 - t0) / 1000000.0;
    size_t actual_bytes = (size_t)actual_iters * buf_size;
    double throughput_bps = (double)(actual_bytes * 8) / elapsed_s;

    printf("\n=== RDMA 吞吐量结果 ===\n");
    printf("  模式:       %s\n", is_write ? "RDMA_WRITE" : "RDMA_SEND");
    printf("  payload:    %zu bytes\n", buf_size);
    printf("  iters:      %d (成功: %d)\n", iters, actual_iters);
    printf("  CQ 完成:     send=%d\n", r.send_completed);
    printf("  release_calls: %d rejected=%d\n", g_release_calls, g_release_rejected);
    printf("  总数据量:   %.2f MB\n", (double)actual_bytes / (1024.0 * 1024.0));
    printf("  耗时:       %.3f s\n", elapsed_s);
    printf("  吞吐量:     %s\n", throughput_str(throughput_bps));

    /* 断开连接 */
    rdma_disconnect(r.id);

    ret = 0;

cleanup:
    if (event) rdma_ack_cm_event(event);
    if (r.id && r.id->qp) rdma_destroy_qp(r.id);
    if (r.cq) ibv_destroy_cq(r.cq);
    /* 释放 pipeline send slots */
    for (int i = 0; i < PIPELINE_DEPTH; i++) {
        if (r.send_slots[i].mr) ibv_dereg_mr(r.send_slots[i].mr);
        if (r.send_slots[i].buf) free(r.send_slots[i].buf);
    }
    if (r.mr) ibv_dereg_mr(r.mr);
    if (r.buf) free(r.buf);
    if (r.pd) ibv_dealloc_pd(r.pd);
    if (r.id) rdma_destroy_id(r.id);
    if (r.ec) rdma_destroy_event_channel(r.ec);
    return ret;
}

/* ========== Main ========== */
static void usage(const char *prog) {
    fprintf(stderr,
            "用法:\n"
            "  服务端: %s --server [--port PORT] [--size SIZE]\n"
            "  客户端: %s --host <server_ip> [options]\n"
            "选项:\n"
            "  --host, -H     服务端 IP（客户端模式必需）\n"
            "  --server, -s   以服务端模式运行\n"
            "  --port, -p     rdma_cm 端口 (默认: 18516)\n"
            "  --size           传输 payload 大小 (默认: 65536)\n"
            "  --iters          传输次数 (默认: 5000)\n"
            "  --mode           write 或 send (默认: write)\n"
            "  --help, -h      显示帮助\n",
            prog, prog);
}

int main(int argc, char **argv) {
    const char *host = NULL;
    int server_mode = 0;
    int port = DEFAULT_PORT;
    size_t buf_size = DEFAULT_SIZE;
    int iters = DEFAULT_ITERS;
    const char *mode = DEFAULT_MODE;

    struct option long_opts[] = {
        {"host", required_argument, 0, 'H'},
        {"server", no_argument, 0, 's'},
        {"port", required_argument, 0, 'p'},
        {"size", required_argument, 0, 1000},
        {"iters", required_argument, 0, 1001},
        {"mode", required_argument, 0, 1002},
        /* 以下参数为兼容旧命令行保留，rdma_cm 自动处理，接受但忽略 */
        {"ib-dev", required_argument, 0, 1003},
        {"ib-port", required_argument, 0, 1004},
        {"gid-idx", required_argument, 0, 1005},
        {"help", no_argument, 0, 'h'},
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
        case 1003: /* --ib-dev: rdma_cm 自动选择，忽略 */ break;
        case 1004: /* --ib-port: rdma_cm 自动选择，忽略 */ break;
        case 1005: /* --gid-idx: rdma_cm 自动选择，忽略 */ break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }

    if (!server_mode && !host) {
        fprintf(stderr, "错误: 客户端模式需要 --host\n");
        usage(argv[0]);
        return 1;
    }

    if (strcmp(mode, "write") != 0 && strcmp(mode, "send") != 0) {
        fprintf(stderr, "错误: mode 必须是 write 或 send\n");
        return 1;
    }

    return server_mode ? run_server(port, buf_size)
                       : run_client(host, buf_size, iters, mode, port);
}
