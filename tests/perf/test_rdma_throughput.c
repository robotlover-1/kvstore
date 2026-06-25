/*
 * test_rdma_throughput.c — RDMA write/send 吞吐量测试 (rdma_cm 版本)
 *
 * 使用 rdma_cm (RDMA Connection Manager) 建立连接，
 * 与项目 kvs_repl.c 全量同步的 RDMA 路径一致。
 *
 * MR 信息通过 rdma_cm private_data 交换，无需额外 TCP 控制通道。
 *
 * 用法:
 *   # 服务端（接收方）
 *   ./test_rdma_throughput --server --port 18516
 *
 *   # 客户端（发送方）
 *   ./test_rdma_throughput --host <server_ip> --port 18516 \
 *       --mode write --size 65536 --iters 10000
 *
 * 依赖: libibverbs, librdmacm
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <infiniband/verbs.h>
#include <math.h>
#include <netdb.h>
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

/* 通过 rdma_cm private_data 交换 MR 信息 */
typedef struct {
    uint64_t addr;
    uint32_t rkey;
} mr_info_t;

/* ========== RDMA 资源（rdma_cm 方式） ========== */
typedef struct {
    struct rdma_event_channel *ec;
    struct rdma_cm_id *id;
    struct rdma_cm_id *listen_id;  /* server only */
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct ibv_cq *cq;

    void *buf;
    size_t buf_size;

    mr_info_t local_mr;
    mr_info_t remote_mr;

    int connected;
} rdma_res_t;

/* ========== 等待 rdma_cm 事件 ========== */
static int wait_cm_event(struct rdma_event_channel *ec,
                         enum rdma_cm_event_type expected,
                         struct rdma_cm_event **out_event,
                         int timeout_ms) {
    struct rdma_cm_event *event = NULL;
    struct pollfd pfd;

    /* 使用 poll 等待 ec->fd 可读，实现超时（与项目 kvs_repl.c 一致） */
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

/* ========== 服务端 ========== */
static int run_server(int port, size_t buf_size) {
    rdma_res_t r;
    struct rdma_cm_event *event = NULL;
    int ret = -1;

    memset(&r, 0, sizeof(r));
    r.buf_size = buf_size;

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
    r.id = event->id;  /* rdma_cm 分配的 accept id */
    rdma_ack_cm_event(event);
    event = NULL;
    printf("[server] connect request received\n");
    fflush(stdout);

    /* 5. 分配资源（使用 accept id->verbs） */
    r.pd = ibv_alloc_pd(r.id->verbs);
    if (!r.pd) { perror("ibv_alloc_pd"); goto cleanup; }

    r.cq = ibv_create_cq(r.id->verbs, 1024, NULL, NULL, 0);
    if (!r.cq) { perror("ibv_create_cq"); goto cleanup; }

    struct ibv_qp_init_attr qp_attr = {
        .send_cq = r.cq,
        .recv_cq = r.cq,
        .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 1024,
                  .max_recv_wr = 1024,
                  .max_send_sge = 1,
                  .max_recv_sge = 1 },
    };
    if (rdma_create_qp(r.id, r.pd, &qp_attr) != 0) {
        perror("rdma_create_qp");
        goto cleanup;
    }

    /* 6. 注册 MR */
    r.buf = aligned_alloc(4096, buf_size);
    if (!r.buf) { perror("aligned_alloc"); goto cleanup; }
    memset(r.buf, 0, buf_size);

    r.mr = ibv_reg_mr(r.pd, r.buf, buf_size,
                      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                          IBV_ACCESS_REMOTE_WRITE);
    if (!r.mr) { perror("ibv_reg_mr"); goto cleanup; }

    r.local_mr.addr = (uint64_t)(uintptr_t)r.buf;
    r.local_mr.rkey = r.mr->rkey;

    /* 7. rdma_accept（附带 MR 信息通过 private_data） */
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

    /* 8. 等待 ESTABLISHED */
    if (wait_cm_event(r.ec, RDMA_CM_EVENT_ESTABLISHED, &event, 5000) != 0) {
        fprintf(stderr, "[server] timeout waiting for ESTABLISHED\n");
        goto cleanup;
    }

    /* 提取客户端 MR 信息 */
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

    /* 9. 接收数据并统计 */
    /* Pre-post recv WRs */
    {
        struct ibv_sge sge = { .addr = (uint64_t)(uintptr_t)r.buf,
                                .length = (uint32_t)buf_size,
                                .lkey = r.mr->lkey };
        struct ibv_recv_wr recv_wr = { .wr_id = 0,
                                        .sg_list = &sge,
                                        .num_sge = 1 };
        struct ibv_recv_wr *bad_wr = NULL;

        for (int i = 0; i < 256; i++) {
            if (ibv_post_recv(r.id->qp, &recv_wr, &bad_wr) != 0) break;
        }
    }

    double total_bytes = 0;
    int completed = 0;

    /* 等待连接断开或数据接收完成 */
    while (r.connected) {
        struct ibv_wc wc[16];
        int n = ibv_poll_cq(r.cq, 16, wc);
        if (n < 0) {
            perror("ibv_poll_cq");
            break;
        }
        for (int i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                /* DISCONNECT 是正常结束信号 */
                if (wc[i].status == IBV_WC_WR_FLUSH_ERR) {
                    r.connected = 0;
                    break;
                }
                fprintf(stderr, "[server] WC 错误: status=%d\n",
                        wc[i].status);
                continue;
            }
            if (wc[i].opcode == IBV_WC_RECV ||
                wc[i].opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
                total_bytes += wc[i].byte_len;
                completed++;
                /* 重新 post recv */
                struct ibv_sge sge = {
                    .addr = (uint64_t)(uintptr_t)r.buf,
                    .length = (uint32_t)buf_size,
                    .lkey = r.mr->lkey };
                struct ibv_recv_wr recv_wr = { .wr_id = (uint64_t)completed,
                                                .sg_list = &sge,
                                                .num_sge = 1 };
                struct ibv_recv_wr *bad_wr = NULL;
                ibv_post_recv(r.id->qp, &recv_wr, &bad_wr);
            }
        }
        if (n == 0) usleep(100);
    }

    printf("[server] 接收完成: %d msgs, %.2f MB\n",
           completed, total_bytes / (1024.0 * 1024.0));
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

    /* 3. 解析地址 */
    struct sockaddr_in addr = { .sin_family = AF_INET,
                                .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        struct hostent *he = gethostbyname(host);
        if (!he) {
            fprintf(stderr, "解析主机 %s 失败\n", host);
            goto cleanup;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
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

    /* 5. 分配资源 */
    r.pd = ibv_alloc_pd(r.id->verbs);
    if (!r.pd) { perror("ibv_alloc_pd"); goto cleanup; }

    r.cq = ibv_create_cq(r.id->verbs, 1024, NULL, NULL, 0);
    if (!r.cq) { perror("ibv_create_cq"); goto cleanup; }

    struct ibv_qp_init_attr qp_attr = {
        .send_cq = r.cq,
        .recv_cq = r.cq,
        .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 1024,
                  .max_recv_wr = 1024,
                  .max_send_sge = 1,
                  .max_recv_sge = 1 },
    };
    if (rdma_create_qp(r.id, r.pd, &qp_attr) != 0) {
        perror("rdma_create_qp");
        goto cleanup;
    }

    /* 6. 注册 MR */
    r.buf = aligned_alloc(4096, buf_size);
    if (!r.buf) { perror("aligned_alloc"); goto cleanup; }
    memset(r.buf, 'R', buf_size);

    r.mr = ibv_reg_mr(r.pd, r.buf, buf_size,
                      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                          IBV_ACCESS_REMOTE_WRITE);
    if (!r.mr) { perror("ibv_reg_mr"); goto cleanup; }

    r.local_mr.addr = (uint64_t)(uintptr_t)r.buf;
    r.local_mr.rkey = r.mr->rkey;

    /* 7. rdma_connect（附带 MR 信息通过 private_data） */
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

    /* 8. 等待 ESTABLISHED */
    if (wait_cm_event(r.ec, RDMA_CM_EVENT_ESTABLISHED, &event, 5000) != 0) {
        fprintf(stderr, "ESTABLISHED 超时或失败\n");
        goto cleanup;
    }

    /* 提取服务端 MR 信息 */
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

    /* 9. 数据传输 */
    int is_write = (strcmp(mode, "write") == 0);
    int actual_iters = 0;
    int inflight = 0;
    int next_poll = 0;

    double t0 = now_us();

    for (int i = 0; i < iters; i++) {
        struct ibv_sge sge = { .addr = (uint64_t)(uintptr_t)r.buf,
                                .length = (uint32_t)buf_size,
                                .lkey = r.mr->lkey };
        struct ibv_send_wr send_wr = { .wr_id = (uint64_t)i,
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

        /* 如果发送队列满，先 poll 一些完成 */
        if (ibv_post_send(r.id->qp, &send_wr, &bad_wr) != 0) {
            struct ibv_wc wc[16];
            int n = ibv_poll_cq(r.cq, 16, wc);
            if (n > 0) {
                inflight -= n;
                /* 重试 post */
                if (ibv_post_send(r.id->qp, &send_wr, &bad_wr) != 0) {
                    fprintf(stderr, "[client] ibv_post_send 失败于 iter %d (retry failed)\n", i);
                    break;
                }
                inflight++;
                actual_iters++;
            } else {
                fprintf(stderr, "[client] ibv_post_send 失败于 iter %d (qp full, no completions)\n", i);
                break;
            }
        } else {
            inflight++;
            actual_iters++;
        }

        /* 每 16 个 WR 或 inflight 超过 256 时 poll CQ */
        if (inflight >= 256 || (i == next_poll) || i == iters - 1) {
            struct ibv_wc wc[32];
            int total_polled = 0;
            int retries = 0;
            while (inflight > 512 && retries < 100) {
                int n = ibv_poll_cq(r.cq, 32, wc);
                if (n <= 0) { usleep(10); retries++; continue; }
                inflight -= n;
                total_polled += n;
                retries = 0;
                for (int j = 0; j < n; j++) {
                    if (wc[j].status != IBV_WC_SUCCESS) {
                        fprintf(stderr,
                                "[client] WC 错误: status=%d at iter=%ld\n",
                                wc[j].status, wc[j].wr_id);
                    }
                }
            }
            if (inflight > 512) {
                /* 强制 drain */
                struct ibv_wc wc2[64];
                int nd = ibv_poll_cq(r.cq, 64, wc2);
                if (nd > 0) inflight -= nd;
            }
            next_poll = i + 16;
        }
    }

    /* 等待所有 inflight 完成 */
    {
        int drain_retries = 0;
        while (inflight > 0 && drain_retries < 1000) {
            struct ibv_wc wc[64];
            int n = ibv_poll_cq(r.cq, 64, wc);
            if (n <= 0) { usleep(100); drain_retries++; continue; }
            inflight -= n;
            drain_retries = 0;
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
    printf("  总数据量:   %.2f MB\n", (double)actual_bytes / (1024.0 * 1024.0));
    printf("  耗时:       %.3f s\n", elapsed_s);
    printf("  吞吐量:     %s\n", throughput_str(throughput_bps));

    /* 断开连接，让服务端感知结束 */
    rdma_disconnect(r.id);

    ret = 0;

cleanup:
    if (event) rdma_ack_cm_event(event);
    if (r.id && r.id->qp) rdma_destroy_qp(r.id);
    if (r.cq) ibv_destroy_cq(r.cq);
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
