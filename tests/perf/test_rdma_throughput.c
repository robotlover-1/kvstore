/*
 * test_rdma_throughput.c — RDMA write/send 吞吐量测试
 *
 * 基于 libibverbs，支持：
 *   RDMA_WRITE（单边，跳过远程 CPU）
 *   RDMA_SEND（双边，远程 CPU 参与）
 *
 * 用法:
 *   # 服务端（在接收方机器运行）
 *   ./test_rdma_throughput --server --ib-dev rxe0 --ib-port 1
 *
 *   # 客户端（在发送方机器运行）
 *   ./test_rdma_throughput --host <server_ip> --ib-dev rxe0 --ib-port 1 \
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
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "common.h"

/* ========== 配置 ========== */
#define DEFAULT_PORT      18516
#define DEFAULT_IB_DEV    "rxe0"
#define DEFAULT_IB_PORT   1
#define DEFAULT_GID_IDX   1
#define DEFAULT_SIZE      65536
#define DEFAULT_ITERS     5000
#define DEFAULT_MODE      "write"

/* 连接信息，通过 TCP 带外交换 */
typedef struct {
    uint64_t addr;   /* MR 虚拟地址 */
    uint32_t rkey;   /* MR remote key */
    uint32_t qp_num; /* QP number */
    uint16_t lid;    /* LID */
} conn_info_t;

/* ========== RDMA 资源 ========== */
typedef struct {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_port_attr port_attr;
    uint16_t lid;
    int gid_idx;
    union ibv_gid gid;

    void *buf;
    size_t buf_size;

    conn_info_t local;
    conn_info_t remote;
} rdma_res_t;

/* ========== 初始化 RDMA 资源 ========== */
static int rdma_init(rdma_res_t *r, const char *ib_dev_name, int ib_port, int gid_idx,
                     size_t buf_size) {
    memset(r, 0, sizeof(*r));
    r->gid_idx = gid_idx;
    r->buf_size = buf_size;

    /* 获取设备列表 */
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        perror("ibv_get_device_list");
        return -1;
    }

    /* 查找指定设备 */
    struct ibv_device *dev = NULL;
    for (int i = 0; dev_list[i]; i++) {
        if (strcmp(ibv_get_device_name(dev_list[i]), ib_dev_name) == 0) {
            dev = dev_list[i];
            break;
        }
    }
    if (!dev) {
        fprintf(stderr, "RDMA 设备 %s 未找到\n", ib_dev_name);
        ibv_free_device_list(dev_list);
        return -1;
    }

    /* 打开设备 */
    r->ctx = ibv_open_device(dev);
    if (!r->ctx) {
        perror("ibv_open_device");
        ibv_free_device_list(dev_list);
        return -1;
    }
    ibv_free_device_list(dev_list);

    /* 查询端口属性 */
    if (ibv_query_port(r->ctx, ib_port, &r->port_attr) != 0) {
        perror("ibv_query_port");
        return -1;
    }
    r->lid = r->port_attr.lid;

    /* 查询 GID */
    if (ibv_query_gid(r->ctx, ib_port, gid_idx, &r->gid) != 0) {
        perror("ibv_query_gid");
        return -1;
    }

    /* 分配保护域 */
    r->pd = ibv_alloc_pd(r->ctx);
    if (!r->pd) {
        perror("ibv_alloc_pd");
        return -1;
    }

    /* 分配内存并注册 MR */
    r->buf = aligned_alloc(4096, buf_size);
    if (!r->buf) {
        perror("aligned_alloc");
        return -1;
    }
    memset(r->buf, 0, buf_size);

    r->mr = ibv_reg_mr(r->pd, r->buf, buf_size,
                       IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!r->mr) {
        perror("ibv_reg_mr");
        return -1;
    }

    /* 创建 CQ */
    r->cq = ibv_create_cq(r->ctx, 256, NULL, NULL, 0);
    if (!r->cq) {
        perror("ibv_create_cq");
        return -1;
    }

    /* 创建 QP */
    struct ibv_qp_init_attr qp_attr = {
        .send_cq = r->cq,
        .recv_cq = r->cq,
        .qp_type = IBV_QPT_RC,
        .cap =
            {
                .max_send_wr = 256,
                .max_recv_wr = 256,
                .max_send_sge = 1,
                .max_recv_sge = 1,
            },
    };
    r->qp = ibv_create_qp(r->pd, &qp_attr);
    if (!r->qp) {
        perror("ibv_create_qp");
        return -1;
    }

    /* 填充本地连接信息 */
    r->local.addr = (uint64_t)(uintptr_t)r->buf;
    r->local.rkey = r->mr->rkey;
    r->local.qp_num = r->qp->qp_num;
    r->local.lid = r->lid;

    return 0;
}

/* ========== QP 状态转换 ========== */
static int modify_qp_to_init(struct ibv_qp *qp, int ib_port) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = (uint8_t)ib_port,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                           IBV_ACCESS_REMOTE_WRITE,
    };
    return ibv_modify_qp(qp, &attr,
                         IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
}

static int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t remote_lid,
                            int gid_idx, union ibv_gid *remote_gid) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_4096,
        .dest_qp_num = remote_qpn,
        .rq_psn = 0,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12,
        .ah_attr =
            {
                .dlid = remote_lid,
                .sl = 0,
                .src_path_bits = 0,
                .is_global = 1,
                .port_num = 1,
                .grh =
                    {
                        .dgid = *remote_gid,
                        .sgid_index = (uint8_t)gid_idx,
                        .hop_limit = 1,
                        .traffic_class = 0,
                    },
            },
    };
    memcpy(&attr.ah_attr.grh.dgid, remote_gid, sizeof(union ibv_gid));

    return ibv_modify_qp(qp, &attr,
                         IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                             IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
}

static int modify_qp_to_rts(struct ibv_qp *qp) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTS,
        .sq_psn = 0,
        .timeout = 14,
        .retry_cnt = 7,
        .rnr_retry = 7,
        .max_rd_atomic = 1,
    };
    return ibv_modify_qp(qp, &attr,
                         IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                             IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
}

/* ========== 服务端 ========== */
static int run_server(const char *ib_dev, int ib_port, int gid_idx, size_t buf_size,
                      int port) {
    rdma_res_t r;
    if (rdma_init(&r, ib_dev, ib_port, gid_idx, buf_size) != 0) return -1;

    printf("[server] RDMA 设备: %s, port %d, LID %d\n", ib_dev, ib_port, r.lid);

    /* TCP 监听，交换连接信息 */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_ANY),
                               .sin_port = htons(port)};
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return -1;
    }
    if (listen(sock, 1) < 0) {
        perror("listen");
        close(sock);
        return -1;
    }
    printf("[server] TCP 监听端口 %d，等待客户端连接...\n", port);

    int conn = accept(sock, NULL, NULL);
    if (conn < 0) {
        perror("accept");
        close(sock);
        return -1;
    }
    set_nodelay(conn);

    /* 交换连接信息 */
    if (write_full(conn, &r.local, sizeof(r.local)) != sizeof(r.local)) {
        perror("send local info");
        goto done;
    }
    if (read_full(conn, &r.remote, sizeof(r.remote)) != sizeof(r.remote)) {
        perror("recv remote info");
        goto done;
    }

    printf("[server] 远端 QP=%d, LID=%d, addr=0x%lx, rkey=%d\n",
           r.remote.qp_num, r.remote.lid, r.remote.addr, r.remote.rkey);

    /* 转换 QP 状态 */
    if (modify_qp_to_init(r.qp, ib_port) != 0) {
        perror("modify_qp_to_init");
        goto done;
    }

    /* 发送端和服务端使用同一个 GID */
    if (modify_qp_to_rtr(r.qp, r.remote.qp_num, r.remote.lid, gid_idx, &r.gid) != 0) {
        perror("modify_qp_to_rtr");
        goto done;
    }
    if (modify_qp_to_rts(r.qp) != 0) {
        perror("modify_qp_to_rts");
        goto done;
    }

    printf("[server] QP 就绪，等待数据...\n");

    /* 接收数据并统计 */
    double total_bytes = 0;
    int completed = 0;

    /* Pre-post recv WRs (one for each expected message) */
    struct ibv_sge sge = {.addr = (uint64_t)(uintptr_t)r.buf, .length = (uint32_t)buf_size, .lkey = r.mr->lkey};
    struct ibv_recv_wr recv_wr = {.wr_id = 0, .sg_list = &sge, .num_sge = 1};
    struct ibv_recv_wr *bad_wr = NULL;

    for (int i = 0; i < 256; i++) {
        if (ibv_post_recv(r.qp, &recv_wr, &bad_wr) != 0) {
            /* 可能 post 队列满了，继续 */
            break;
        }
    }

    /* 轮询 CQ，非阻塞等待 TCP done 信号 */
    int done_signal = 0;
    set_nonblock(conn);

    while (!done_signal) {
        struct ibv_wc wc[16];
        int n = ibv_poll_cq(r.cq, 16, wc);
        if (n < 0) {
            perror("ibv_poll_cq");
            break;
        }
        for (int i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "[server] WC 错误: status=%d\n", wc[i].status);
                continue;
            }
            if (wc[i].opcode == IBV_WC_RECV || wc[i].opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
                total_bytes += wc[i].byte_len;
                completed++;
                /* 重新 post recv */
                recv_wr.wr_id = (uint64_t)completed;
                ibv_post_recv(r.qp, &recv_wr, &bad_wr);
            }
        }
        /* 非阻塞检查 TCP done */
        char tmp;
        if (read(conn, &tmp, 1) > 0) {
            done_signal = 1;
        }
        if (n == 0) usleep(100);
    }

    printf("[server] 接收完成: %d msgs, %.2f MB\n",
           completed, total_bytes / (1024.0 * 1024.0));

done:
    close(conn);
    close(sock);

    if (r.qp) ibv_destroy_qp(r.qp);
    if (r.cq) ibv_destroy_cq(r.cq);
    if (r.mr) ibv_dereg_mr(r.mr);
    if (r.buf) free(r.buf);
    if (r.pd) ibv_dealloc_pd(r.pd);
    if (r.ctx) ibv_close_device(r.ctx);

    return 0;
}

/* ========== 客户端 ========== */
static int run_client(const char *host, const char *ib_dev, int ib_port, int gid_idx,
                      size_t buf_size, int iters, const char *mode, int port) {
    rdma_res_t r;
    if (rdma_init(&r, ib_dev, ib_port, gid_idx, buf_size) != 0) return -1;

    printf("[client] RDMA 设备: %s, port %d, LID %d\n", ib_dev, ib_port, r.lid);

    /* TCP 连接服务端 */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    struct hostent *he = gethostbyname(host);
    if (!he) {
        fprintf(stderr, "解析主机 %s 失败\n", host);
        close(sock);
        return -1;
    }

    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }
    set_nodelay(sock);

    /* 交换连接信息 */
    if (write_full(sock, &r.local, sizeof(r.local)) != sizeof(r.local)) {
        perror("send local info");
        goto done;
    }
    if (read_full(sock, &r.remote, sizeof(r.remote)) != sizeof(r.remote)) {
        perror("recv remote info");
        goto done;
    }

    printf("[client] 远端 QP=%d, LID=%d, addr=0x%lx, rkey=%d\n",
           r.remote.qp_num, r.remote.lid, r.remote.addr, r.remote.rkey);

    /* 转换 QP 状态 */
    if (modify_qp_to_init(r.qp, ib_port) != 0) {
        perror("modify_qp_to_init");
        goto done;
    }
    if (modify_qp_to_rtr(r.qp, r.remote.qp_num, r.remote.lid, gid_idx, &r.gid) != 0) {
        perror("modify_qp_to_rtr");
        goto done;
    }
    if (modify_qp_to_rts(r.qp) != 0) {
        perror("modify_qp_to_rts");
        goto done;
    }

    printf("[client] QP 就绪，开始传输...\n");

    int is_write = (strcmp(mode, "write") == 0);
    size_t total_bytes = (size_t)iters * buf_size;

    /* 填充发送 buffer */
    memset(r.buf, 'R', buf_size);

    double t0 = now_us();

    for (int i = 0; i < iters; i++) {
        struct ibv_sge sge = {.addr = (uint64_t)(uintptr_t)r.buf,
                              .length = (uint32_t)buf_size,
                              .lkey = r.mr->lkey};
        struct ibv_send_wr send_wr = {.wr_id = (uint64_t)i,
                                      .sg_list = &sge,
                                      .num_sge = 1,
                                      .send_flags = IBV_SEND_SIGNALED};
        struct ibv_send_wr *bad_wr = NULL;

        if (is_write) {
            send_wr.opcode = IBV_WR_RDMA_WRITE;
            send_wr.wr.rdma.remote_addr = r.remote.addr;
            send_wr.wr.rdma.rkey = r.remote.rkey;
        } else {
            send_wr.opcode = IBV_WR_SEND;
        }

        if (ibv_post_send(r.qp, &send_wr, &bad_wr) != 0) {
            fprintf(stderr, "[client] ibv_post_send 失败于 iter %d\n", i);
            break;
        }

        /* 每 64 个 WR poll 一次 CQ */
        if ((i & 63) == 63 || i == iters - 1) {
            struct ibv_wc wc[64];
            int npoll = 0;
            while (npoll == 0) {
                int n = ibv_poll_cq(r.cq, 64, wc);
                if (n < 0) {
                    perror("ibv_poll_cq");
                    goto done;
                }
                npoll += n;
                for (int j = 0; j < n; j++) {
                    if (wc[j].status != IBV_WC_SUCCESS) {
                        fprintf(stderr, "[client] WC 错误: status=%d at iter=%d\n",
                                wc[j].status, i);
                    }
                }
            }
        }
    }

    double t1 = now_us();
    double elapsed_s = (t1 - t0) / 1000000.0;
    double throughput_bps = (double)(total_bytes * 8) / elapsed_s;

    printf("\n=== RDMA 吞吐量结果 ===\n");
    printf("  模式:       %s\n", is_write ? "RDMA_WRITE" : "RDMA_SEND");
    printf("  payload:    %zu bytes\n", buf_size);
    printf("  iters:      %d\n", iters);
    printf("  总数据量:   %.2f MB\n", (double)total_bytes / (1024.0 * 1024.0));
    printf("  耗时:       %.3f s\n", elapsed_s);
    printf("  吞吐量:     %s\n", throughput_str(throughput_bps));

    /* 通知服务端完成 */
    int done_signal = 1;
    write_full(sock, &done_signal, sizeof(done_signal));

done:
    close(sock);

    if (r.qp) ibv_destroy_qp(r.qp);
    if (r.cq) ibv_destroy_cq(r.cq);
    if (r.mr) ibv_dereg_mr(r.mr);
    if (r.buf) free(r.buf);
    if (r.pd) ibv_dealloc_pd(r.pd);
    if (r.ctx) ibv_close_device(r.ctx);

    return 0;
}

/* ========== Main ========== */
static void usage(const char *prog) {
    fprintf(stderr,
            "用法:\n"
            "  服务端: %s --server [options]\n"
            "  客户端: %s --host <server_ip> [options]\n"
            "选项:\n"
            "  --host, -H     服务端 IP（客户端模式必需）\n"
            "  --server, -s   以服务端模式运行\n"
            "  --ib-dev         IB 设备名 (默认: rxe0)\n"
            "  --ib-port        IB 端口 (默认: 1)\n"
            "  --gid-idx        GID 索引 (默认: 1)\n"
            "  --port, -p       TCP 控制端口 (默认: 18516)\n"
            "  --size           传输 payload 大小 (默认: 65536)\n"
            "  --iters          传输次数 (默认: 5000)\n"
            "  --mode           write 或 send (默认: write)\n"
            "  --help, -h      显示帮助\n",
            prog, prog);
}

int main(int argc, char **argv) {
    const char *host = NULL;
    const char *ib_dev = DEFAULT_IB_DEV;
    int ib_port = DEFAULT_IB_PORT;
    int gid_idx = DEFAULT_GID_IDX;
    int port = DEFAULT_PORT;
    size_t buf_size = DEFAULT_SIZE;
    int iters = DEFAULT_ITERS;
    const char *mode = DEFAULT_MODE;
    int server_mode = 0;

    struct option long_opts[] = {{"host", required_argument, 0, 'H'},
                                 {"server", no_argument, 0, 's'},
                                 {"ib-dev", required_argument, 0, 1000},
                                 {"ib-port", required_argument, 0, 1001},
                                 {"gid-idx", required_argument, 0, 1002},
                                 {"port", required_argument, 0, 'p'},
                                 {"size", required_argument, 0, 1003},
                                 {"iters", required_argument, 0, 1004},
                                 {"mode", required_argument, 0, 1005},
                                 {"help", no_argument, 0, 'h'},
                                 {0, 0, 0, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "H:sp:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'H': host = optarg; break;
        case 's': server_mode = 1; break;
        case 1000: ib_dev = optarg; break;
        case 1001: ib_port = atoi(optarg); break;
        case 1002: gid_idx = atoi(optarg); break;
        case 'p': port = atoi(optarg); break;
        case 1003: buf_size = (size_t)atol(optarg); break;
        case 1004: iters = atoi(optarg); break;
        case 1005: mode = optarg; break;
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

    return server_mode ? run_server(ib_dev, ib_port, gid_idx, buf_size, port)
                       : run_client(host, ib_dev, ib_port, gid_idx, buf_size, iters, mode, port);
}
