// ============================================================
// kvs_repl_kprobe.c — kprobe+RDMA WRITE 增量同步转发模块
//
// Master 侧职责:
//   1. 加载 BPF kprobe 程序, attach 到 tcp_sendmsg
//   2. 设置 PID/fd 过滤条件
//   3. ring_buffer__poll 消费 ringbuf 数据
//   4. 通过 RDMA WRITE（单边）发送到 Slave 预置 MR
//
// Slave 侧职责:
//   1. 注册 MR（带 IBV_ACCESS_REMOTE_WRITE）
//   2. 建链响应中返回 MR 信息（rkey + addr）
//   3. 轮询线程消费 MR 环形缓冲区
//
// 数据流:
//   tcp_sendmsg → kprobe → ringbuf → kprobe_ringbuf_cb
//     → RDMA WRITE(data) → RDMA WRITE(head) → Slave MR
//     → Slave 轮询 → parse_resp_stream → apply
//
// TCP 保底路径始终运行，Slave 通过 repl_offset 去重。
// ============================================================

#include "kvstore/kvstore.h"
#include "kvstore/replication/repl_kprobe.h"

#if KVS_ENABLE_KPROBE_RDMA

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#if KVS_ENABLE_RDMA
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#endif

/* ---- 常量 ---- */
#define KVS_KPROBE_RINGBUF_POLL_MS     5   /* ringbuf 轮询间隔(ms) */
#define KVS_RDMA_WRITE_SLOTS           8   /* RDMA WRITE 并发 slot 数 */
#define KVS_RDMA_WRITE_SLOT_SIZE       512 /* 每 slot 容量（与 BPF 匹配） */
#define KVS_KPROBE_RDMA_POLL_US        100 /* Slave 轮询间隔(us) */

/* ---- BPF Map FDs ---- */
static struct bpf_object *g_kprobe_obj = NULL;
static int g_kprobe_ctl_fd = -1;
static int g_kprobe_stats_fd = -1;
static int g_kprobe_ringbuf_fd = -1;
static struct ring_buffer *g_kprobe_ringbuf = NULL;
static volatile int g_kprobe_running = 0;

/* ---- RDMA WRITE Slot（Master 侧发送缓冲区） ---- */
typedef struct rdma_write_slot_s {
    unsigned char buf[KVS_RDMA_WRITE_SLOT_SIZE] __attribute__((aligned(64)));
    struct ibv_mr *mr;
    volatile int in_flight;
} rdma_write_slot_t;

static rdma_write_slot_t g_wr_slots[KVS_RDMA_WRITE_SLOTS];
static int g_wr_head = 0;           /* 下一个空闲 slot 索引 */
static int g_wr_in_flight = 0;      /* 飞行中的 WR 计数 */
static int g_wr_producer_seq = 0;   /* 生产者序号（用于 head 指针） */

/* ---- Slave MR 环形缓冲区信息（Master 侧） ---- */
static volatile struct {
    uint64_t remote_data_base;      /* Slave slots 基地址 */
    uint32_t rkey;                  /* Slave MR rkey */
    uint64_t remote_head_addr;      /* Slave producer_head 地址 */
    size_t   slot_count;
    size_t   slot_capacity;
} g_slave_mr;

/* ---- RDMA 上下文（独立 QP，与 fullsync 分离） ---- */
static pthread_mutex_t g_kprobe_rdma_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    struct rdma_event_channel *ec;
    struct rdma_cm_id *id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_comp_channel *comp_chan;
    volatile int connected;
} g_rdma_kprobe;

/* ---- Slave 侧环形缓冲区（仅在 Slave 进程使用） ---- */
static kprobe_rdma_ringbuf_t *g_slave_ringbuf = NULL;
static struct ibv_mr *g_slave_ringbuf_mr = NULL;
static pthread_t g_slave_poll_tid;
static int g_slave_poll_started = 0;
static pthread_t g_master_forward_tid;
static int g_master_forward_started = 0;

/* ---- 统计 ---- */
static unsigned long long g_total_events = 0;
static unsigned long long g_total_bytes = 0;
static unsigned long long g_rdma_writes = 0;
static unsigned long long g_rdma_errors = 0;
static volatile int g_cc_repldone_detect = 0;   /* REPLDONE 探测次数 */
static volatile int g_cc_l1_flushed = 0;        /* L1 flush 次数 */
static volatile int g_cc_l2_flushed = 0;        /* L2 flush 条目数 */

/* ============================================================
 * 内部函数声明
 * ============================================================ */
static int wr_slot_acquire(int timeout_ms, int *out);
static int wr_submit_data(int slot, size_t len);
static int wr_submit_head(int slot);
static int kprobe_ringbuf_cb(void *ctx, void *data, size_t size);
static int kprobe_load_bpf(void);
static void kprobe_unload_bpf(void);
static void *kprobe_rdma_forward_thread(void *arg);
static void *kprobe_rdma_slave_poll(void *arg);

/* ============================================================
 * BPF 加载与管理
 * ============================================================ */

static int kprobe_load_bpf(void) {
    struct bpf_program *prog;
    struct bpf_link *link;
    int rc;

    if (g_kprobe_obj) return 0;

    if (!g_cfg.repl_kprobe_obj_path[0]) {
        fprintf(stderr, "kprobe: missing repl_kprobe_obj_path config\n");
        return -1;
    }

    /* 设置 libbpf 日志级别（关闭，避免大量调试输出） */
    libbpf_set_print(NULL);

    /* 打开 BPF 对象文件 */
    g_kprobe_obj = bpf_object__open_file(g_cfg.repl_kprobe_obj_path, NULL);
    if (libbpf_get_error(g_kprobe_obj)) {
        fprintf(stderr, "kprobe: failed to open BPF object: %s\n",
            g_cfg.repl_kprobe_obj_path);
        g_kprobe_obj = NULL;
        return -1;
    }

    /* 加载 BPF 对象 */
    rc = bpf_object__load(g_kprobe_obj);
    if (rc != 0) {
        fprintf(stderr, "kprobe: failed to load BPF object: %d\n", rc);
        bpf_object__close(g_kprobe_obj);
        g_kprobe_obj = NULL;
        return -1;
    }

    /* 获取 map FDs */
    g_kprobe_ctl_fd = bpf_object__find_map_fd_by_name(g_kprobe_obj, "kprobe_ctl");
    g_kprobe_stats_fd = bpf_object__find_map_fd_by_name(g_kprobe_obj, "kprobe_stats");
    g_kprobe_ringbuf_fd = bpf_object__find_map_fd_by_name(g_kprobe_obj, "repl_ringbuf");
    if (g_kprobe_ctl_fd < 0 || g_kprobe_stats_fd < 0 || g_kprobe_ringbuf_fd < 0) {
        fprintf(stderr, "kprobe: failed to find BPF maps\n");
        kprobe_unload_bpf();
        return -1;
    }

    /* 找到并 attach kprobe 程序 */
    prog = bpf_object__find_program_by_name(g_kprobe_obj, "kprobe_kvs_repl_tcp_sendmsg");
    if (!prog) {
        fprintf(stderr, "kprobe: failed to find kprobe program\n");
        kprobe_unload_bpf();
        return -1;
    }

    link = bpf_program__attach_kprobe(prog, false /* kprobe, not kretprobe */,
                                       "tcp_sendmsg");
    if (libbpf_get_error(link)) {
        fprintf(stderr, "kprobe: failed to attach kprobe/tcp_sendmsg\n");
        kprobe_unload_bpf();
        return -1;
    }

    /* 创建 ringbuf */
    g_kprobe_ringbuf = ring_buffer__new(g_kprobe_ringbuf_fd,
        kprobe_ringbuf_cb, NULL, NULL);
    if (!g_kprobe_ringbuf) {
        fprintf(stderr, "kprobe: failed to create ring buffer\n");
        kprobe_unload_bpf();
        return -1;
    }

    fprintf(stderr, "kprobe: BPF loaded and attached to tcp_sendmsg\n");
    return 0;
}

static void kprobe_unload_bpf(void) {
    if (g_kprobe_ringbuf) {
        ring_buffer__free(g_kprobe_ringbuf);
        g_kprobe_ringbuf = NULL;
    }
    if (g_kprobe_obj) {
        bpf_object__close(g_kprobe_obj);
        g_kprobe_obj = NULL;
    }
    g_kprobe_ctl_fd = -1;
    g_kprobe_stats_fd = -1;
    g_kprobe_ringbuf_fd = -1;
}

static int kprobe_get_stat(__u32 key, __u64 *out) {
    __u64 value = 0;
    if (g_kprobe_stats_fd < 0) return -1;
    if (bpf_map_lookup_elem(g_kprobe_stats_fd, &key, &value) != 0) return -1;
    if (out) *out = value;
    return 0;
}

/* ============================================================
 * RDMA WRITE 操作
 * ============================================================ */

static int wr_slot_acquire(int timeout_ms, int *out) {
    long long deadline = timeout_ms > 0 ? kvs_now_ms() + timeout_ms : 0;
retry:
    for (int i = 0; i < KVS_RDMA_WRITE_SLOTS; i++) {
        int idx = (g_wr_head + i) % KVS_RDMA_WRITE_SLOTS;
        if (!g_wr_slots[idx].in_flight) {
            g_wr_head = (idx + 1) % KVS_RDMA_WRITE_SLOTS;
            g_wr_slots[idx].in_flight = 1;
            *out = idx;
            return 0;
        }
    }
    /* Poll CQ 回收已完成 slot */
    pthread_mutex_lock(&g_kprobe_rdma_lock);
    if (g_rdma_kprobe.cq) {
        struct ibv_wc wc;
        int polled = 0;
        while (ibv_poll_cq(g_rdma_kprobe.cq, 1, &wc) > 0) {
            polled++;
            if (wc.status == IBV_WC_SUCCESS) {
                int slot = (int)(wc.wr_id & 0xFFFF);
                if (slot < KVS_RDMA_WRITE_SLOTS) {
                    g_wr_slots[slot].in_flight = 0;
                    g_wr_in_flight--;
                }
            } else {
                fprintf(stderr, "kprobe rdma: wr_slot_acquire CQ error status=%d wr_id=0x%lx opcode=%d\n",
                    wc.status, (unsigned long)wc.wr_id, wc.opcode);
            }
        }
        if (polled > 0) {
            /* 可在此处添加周期性统计日志 */
        }
    }
    pthread_mutex_unlock(&g_kprobe_rdma_lock);
    if (timeout_ms > 0 && kvs_now_ms() >= deadline) return -1;
    usleep(100);
    goto retry;
}

/* RDMA WRITE: 写入数据到 Slave 槽位 */
static int wr_submit_data(int slot, size_t len) {
    struct ibv_sge sge;
    struct ibv_send_wr wr, *bad = NULL;

    uint64_t slot_idx = (uint64_t)(g_wr_producer_seq % g_slave_mr.slot_count);
    uint64_t remote_off = slot_idx * g_slave_mr.slot_capacity;

    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)g_wr_slots[slot].buf;
    sge.length = (uint32_t)len;
    sge.lkey = g_wr_slots[slot].mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = (uint64_t)slot;              /* 低 16 位 = slot 索引 */
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_FENCE;
    wr.wr.rdma.remote_addr = g_slave_mr.remote_data_base + remote_off;
    wr.wr.rdma.rkey = g_slave_mr.rkey;

    pthread_mutex_lock(&g_kprobe_rdma_lock);
    int rc = ibv_post_send(g_rdma_kprobe.id->qp, &wr, &bad);
    pthread_mutex_unlock(&g_kprobe_rdma_lock);
    if (rc != 0) fprintf(stderr, "kprobe rdma: wr_submit_data FAILED rc=%d errno=%d (%s)\n",
        rc, errno, strerror(errno));
    return rc;
}

/* 独立 8 字节缓冲区用于更新 producer_head，避免与数据 WR 共用 buf */
static unsigned char g_head_buf[8] __attribute__((aligned(64)));
static struct ibv_mr *g_head_mr = NULL;

/* RDMA WRITE: 更新 Slave producer_head */
static int wr_submit_head(int slot) {
    struct ibv_sge sge;
    struct ibv_send_wr wr, *bad = NULL;

    uint64_t new_head = (uint64_t)(g_wr_producer_seq + 1);
    memcpy(g_head_buf, &new_head, 8);

    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)g_head_buf;
    sge.length = 8;
    sge.lkey = g_head_mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = (uint64_t)slot | 0x10000;    /* 高位标记 head update */
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = g_slave_mr.remote_head_addr;
    wr.wr.rdma.rkey = g_slave_mr.rkey;

    pthread_mutex_lock(&g_kprobe_rdma_lock);
    int rc = ibv_post_send(g_rdma_kprobe.id->qp, &wr, &bad);
    pthread_mutex_unlock(&g_kprobe_rdma_lock);
    if (rc != 0) fprintf(stderr, "kprobe rdma: wr_submit_head FAILED rc=%d errno=%d (%s)\n",
        rc, errno, strerror(errno));
    return rc;
}

/* ============================================================
 * Ringbuf 回调 — BPF ringbuf 有数据时触发
 * ============================================================ */
/* REPLDONE 魔法值 — BPF 探测到 REPLDONE 时写入的特殊通知 */
#define KVS_KPROBE_MAGIC_REPLDONE 0xFFFFFFFF

static int kprobe_ringbuf_cb(void *ctx, void *data, size_t size) {
    (void)ctx;

    if (size < 4) return 0;

    __u32 payload_len;
    memcpy(&payload_len, data, 4);

    /* NEW: 检测 REPLDONE 通知（BPF 探测到 REPLDONE 命令时发送） */
    if (payload_len == KVS_KPROBE_MAGIC_REPLDONE) {
        g_cc_repldone_detect++;
        fprintf(stderr, "kprobe: REPLDONE detected via BPF, triggering cache flush\n");
        /* 通知用户态全量同步完成 */
        extern volatile int g_repl_fullsync_in_progress;
        g_repl_fullsync_in_progress = 0;
        repl_kprobe_fullsync_done();
        return 0;
    }

    if (payload_len == 0 || payload_len + 4 > size)
        return 0;

    unsigned char *payload = (unsigned char *)data + 4;

    g_total_events++;
    g_total_bytes += payload_len;

    /* MR 未就绪时跳过 RDMA WRITE（KPROBEMR 还没交换完） */
    if (g_slave_mr.rkey == 0 || !g_rdma_kprobe.connected) {
        if (g_total_events % 1000 == 1)
            fprintf(stderr, "kprobe rdma: MR not ready, skip (events=%llu)\n",
                g_total_events);
        return 0;
    }

    int slot;
    if (wr_slot_acquire(5000, &slot) != 0) {
        g_rdma_errors++;
        fprintf(stderr, "kprobe rdma: no available WRITE slot, dropping\n");
        return -1;
    }

    if (payload_len + 4 > KVS_RDMA_WRITE_SLOT_SIZE) {
        g_wr_slots[slot].in_flight = 0;
        g_rdma_errors++;
        return 0;
    }

    /* 构造: [4B len][payload] */
    uint32_t net_len = payload_len;
    memcpy(g_wr_slots[slot].buf, &net_len, 4);
    memcpy(g_wr_slots[slot].buf + 4, payload, payload_len);

    /* Step 1: RDMA WRITE 数据到 Slave slot */
    if (wr_submit_data(slot, (size_t)payload_len + 4) != 0) {
        g_wr_slots[slot].in_flight = 0;
        g_rdma_errors++;
        fprintf(stderr, "kprobe rdma: WRITE data failed\n");
        return -1;
    }

    /* Step 2: RDMA WRITE 更新 producer_head */
    if (wr_submit_head(slot) != 0) {
        g_rdma_errors++;
        fprintf(stderr, "kprobe rdma: WRITE head failed\n");
        return -1;
    }

    g_wr_in_flight++;
    g_wr_producer_seq++;
    g_rdma_writes++;
    if (g_rdma_writes == 1)
        fprintf(stderr, "kprobe rdma: first RDMA WRITE succeeded, data now flowing via RDMA\n");
    return 0;
}

/* ============================================================
 * Master 转发线程 — 轮询 BPF ringbuf
 * ============================================================ */
static void *kprobe_rdma_forward_thread(void *arg) {
    (void)arg;
    fprintf(stderr, "kprobe rdma: forward thread started\n");

    fprintf(stderr, "kprobe rdma: forward thread running, ringbuf=%p\n",
        (void*)g_kprobe_ringbuf);
    int poll_count = 0;
    while (g_kprobe_running && g_rdma_kprobe.connected) {
        if (g_kprobe_ringbuf) {
            int err = ring_buffer__poll(g_kprobe_ringbuf,
                KVS_KPROBE_RINGBUF_POLL_MS);
            if (err > 0) {
                poll_count += err;
            } else if (err < 0) {
                if (err != -EAGAIN)
                    fprintf(stderr, "kprobe rdma: ringbuf poll err=%d\n", err);
                usleep(1000);
            }
        } else {
            usleep(10000);
        }
    }

    fprintf(stderr, "kprobe rdma: forward thread exiting\n");
    return NULL;
}

/* ============================================================
 * Slave 轮询线程 — 消费 MR 环形缓冲区
 * ============================================================ */
static void *kprobe_rdma_slave_poll(void *arg) {
    (void)arg;

    if (!g_slave_ringbuf) {
        fprintf(stderr, "kprobe rdma: slave poll - ringbuf not initialized\n");
        return NULL;
    }

    kprobe_rdma_ringbuf_t *rb = g_slave_ringbuf;
    unsigned char stream_buf[BUFFER_CAP];
    size_t stream_len = 0;

    fprintf(stderr, "kprobe rdma: slave poll thread started, ringbuf=%p slots=%zu cap=%zu\n",
        (void*)rb, (size_t)KPROBE_RDMA_SLOT_COUNT, (size_t)KPROBE_RDMA_SLOT_CAPACITY);
    int empty_loops = 0;

    while (g_kprobe_running) {
        /* 读内存屏障 — 保证看到 Master 的最新写入 */
        __sync_synchronize();
        uint64_t head = rb->producer_head;
        uint64_t tail = rb->consumer_tail;

        if (tail == head) {
            empty_loops++;
            if (empty_loops == 1)
                fprintf(stderr, "kprobe rdma: slave poll idle (head=tail=%lu)\n",
                    (unsigned long)head);
            usleep(KVS_KPROBE_RDMA_POLL_US);
            continue;
        }

        empty_loops = 0;

        /* 消费所有未处理槽位 */
        while (tail != head) {
            size_t idx = tail % KPROBE_RDMA_SLOT_COUNT;
            size_t off = idx * KPROBE_RDMA_SLOT_CAPACITY;

            uint32_t slot_len;
            memcpy(&slot_len, rb->slots + off, 4);
            if (slot_len == 0 || slot_len > KPROBE_RDMA_SLOT_CAPACITY - 4) {
                fprintf(stderr, "kprobe rdma: slave poll bad slot_len=%u at idx=%zu\n",
                    slot_len, idx);
                tail++;
                continue;
            }

            unsigned char *slot_data = rb->slots + off + 4;

            /* 送入 RESP 解析流 */
            if (stream_len + slot_len > sizeof(stream_buf))
                stream_len = 0;
            memcpy(stream_buf + stream_len, slot_data, slot_len);
            stream_len += slot_len;
            parse_resp_stream(NULL, stream_buf, &stream_len, 1);

            tail++;
        }

        /* 更新 consumer_tail（本地写，无需 RDMA） */
        rb->consumer_tail = tail;
        __sync_synchronize();
    }

    fprintf(stderr, "kprobe rdma: slave poll thread exiting\n");
    return NULL;
}

/* ============================================================
 * RDMA QP 管理
 * ============================================================ */

/* Master 侧: 建立 RDMA QP，连接 Slave 的 kprobe-rdma listener */
static int kprobe_rdma_qp_connect(const char *host, int port) {
    struct sockaddr_in addr;
    int kprobe_port = g_cfg.rdma_port > 0 ? g_cfg.rdma_port : (port + 12);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)kprobe_port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "kprobe rdma: invalid address %s\n", host);
        return -1;
    }

    /* 创建事件通道 */
    g_rdma_kprobe.ec = rdma_create_event_channel();
    if (!g_rdma_kprobe.ec) {
        fprintf(stderr, "kprobe rdma: failed to create event channel\n");
        return -1;
    }

    /* 创建 CM ID */
    if (rdma_create_id(g_rdma_kprobe.ec, &g_rdma_kprobe.id, NULL, RDMA_PS_TCP) != 0) {
        fprintf(stderr, "kprobe rdma: failed to create cm id\n");
        return -1;
    }

    /* 地址解析 */
    if (rdma_resolve_addr(g_rdma_kprobe.id, NULL, (struct sockaddr *)&addr, 1000) != 0) {
        fprintf(stderr, "kprobe rdma: resolve addr failed\n");
        return -1;
    }

    /* 等待地址解析完成 */
    {
        struct rdma_cm_event *event = NULL;
        if (rdma_get_cm_event(g_rdma_kprobe.ec, &event) != 0) return -1;
        if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
            rdma_ack_cm_event(event);
            return -1;
        }
        rdma_ack_cm_event(event);
    }

    /* 路由解析 */
    if (rdma_resolve_route(g_rdma_kprobe.id, 1000) != 0) {
        fprintf(stderr, "kprobe rdma: resolve route failed\n");
        return -1;
    }
    {
        struct rdma_cm_event *event = NULL;
        if (rdma_get_cm_event(g_rdma_kprobe.ec, &event) != 0) return -1;
        if (event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
            rdma_ack_cm_event(event);
            return -1;
        }
        rdma_ack_cm_event(event);
    }

    /* 分配 PD */
    g_rdma_kprobe.pd = ibv_alloc_pd(g_rdma_kprobe.id->verbs);
    if (!g_rdma_kprobe.pd) {
        fprintf(stderr, "kprobe rdma: alloc pd failed\n");
        return -1;
    }

    /* 创建 CQ（comp_chan 为 NULL，使用同步 poll） */
    g_rdma_kprobe.cq = ibv_create_cq(g_rdma_kprobe.id->verbs, 64, NULL, NULL, 0);
    if (!g_rdma_kprobe.cq) {
        fprintf(stderr, "kprobe rdma: create cq failed\n");
        return -1;
    }

    /* 创建 QP */
    {
        struct ibv_qp_init_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.send_cq = g_rdma_kprobe.cq;
        attr.recv_cq = g_rdma_kprobe.cq;
        attr.qp_type = IBV_QPT_RC;
        attr.cap.max_send_wr = 64;
        attr.cap.max_recv_wr = 64;
        attr.cap.max_send_sge = 1;
        attr.cap.max_recv_sge = 1;
        if (rdma_create_qp(g_rdma_kprobe.id, g_rdma_kprobe.pd, &attr) != 0) {
            fprintf(stderr, "kprobe rdma: create qp failed\n");
            return -1;
        }
    }

    /* 注册 WRITE slot MR */
    for (int i = 0; i < KVS_RDMA_WRITE_SLOTS; i++) {
        g_wr_slots[i].mr = ibv_reg_mr(g_rdma_kprobe.pd,
            g_wr_slots[i].buf, KVS_RDMA_WRITE_SLOT_SIZE,
            IBV_ACCESS_LOCAL_WRITE);
        if (!g_wr_slots[i].mr) {
            fprintf(stderr, "kprobe rdma: reg mr slot %d failed\n", i);
            return -1;
        }
    }
    /* 注册 head 更新专用缓冲区（独立于数据 slot，避免覆盖） */
    g_head_mr = ibv_reg_mr(g_rdma_kprobe.pd, g_head_buf, 8,
        IBV_ACCESS_LOCAL_WRITE);
    if (!g_head_mr) {
        fprintf(stderr, "kprobe rdma: reg mr for head buf failed\n");
        return -1;
    }

    /* 连接 */
    {
        struct rdma_conn_param param;
        memset(&param, 0, sizeof(param));
        param.initiator_depth = 1;
        param.responder_resources = 1;
        param.retry_count = 7;
        param.rnr_retry_count = 7;

        if (rdma_connect(g_rdma_kprobe.id, &param) != 0) {
            fprintf(stderr, "kprobe rdma: connect failed\n");
            return -1;
        }
    }

    /* 等待 ESTABLISHED */
    {
        struct rdma_cm_event *event = NULL;
        if (rdma_get_cm_event(g_rdma_kprobe.ec, &event) != 0) return -1;
        if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
            fprintf(stderr, "kprobe rdma: unexpected event %d\n", event->event);
            rdma_ack_cm_event(event);
            return -1;
        }
        rdma_ack_cm_event(event);
    }

    g_rdma_kprobe.connected = 1;
    fprintf(stderr, "kprobe rdma: QP connected\n");
    return 0;
}

/* Master 侧: 连接 kprobe-rdma QP 到 Slave listener + 发送 KPROBEMR + 启动转发线程
 * host/port: Slave 地址和端口（replication 端口，函数内部 +12 得到 kprobe 端口）
 * tcp_fd:    复制自 replication TCP 连接，用于发送 KPROBEMR（后台线程独立关闭） */
int repl_kprobe_rdma_connect_mr(const char *host, int port, int tcp_fd) {
    if (!host || port <= 0) return -1;

    fprintf(stderr, "kprobe rdma: connect_mr begin host=%s port=%d tcp_fd=%d\n",
        host, port, tcp_fd);

    /* 1. 建立 RDMA QP 到 Slave listener */
    if (kprobe_rdma_qp_connect(host, port) != 0) {
        fprintf(stderr, "kprobe rdma: connect_mr - QP connect failed\n");
        close(tcp_fd);
        return -1;
    }

    /* 2. 等 slave 完成 MR 注册后发送 KPROBEMR
     *    forward 线程在 MR info 收到后才启动（见 parse_mr_info_direct），
     *    避免在 MR 就绪前处理 ringbuf events 导致大量 skip */
    usleep(200000);  /* 200ms */

    const char req[] = "KPROBEMR\r\n";
    ssize_t sent = send(tcp_fd, req, strlen(req), MSG_NOSIGNAL);
    fprintf(stderr, "kprobe rdma: KPROBEMR sent to slave via tcp_fd=%d, "
        "sent=%zd/%zu (errno=%d)\n", tcp_fd, sent, strlen(req), errno);

    close(tcp_fd);
    fprintf(stderr, "kprobe rdma: connect_mr DONE\n");
    return 0;
}

/* Slave 侧: 为 kprobe-rdma 注册 MR（由现有 listener 线程调用） */
int repl_kprobe_rdma_slave_accept(struct ibv_pd *pd,
    char *resp, size_t resp_cap)
{
    if (!pd || !resp || resp_cap < 128) return -1;

    g_slave_ringbuf = (kprobe_rdma_ringbuf_t *)
        kvs_malloc(KPROBE_RDMA_RINGBUF_SIZE);
    if (!g_slave_ringbuf) return -1;
    memset(g_slave_ringbuf, 0, KPROBE_RDMA_RINGBUF_SIZE);

    g_slave_ringbuf_mr = ibv_reg_mr(pd, g_slave_ringbuf,
        KPROBE_RDMA_RINGBUF_SIZE,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!g_slave_ringbuf_mr) {
        kvs_free(g_slave_ringbuf);
        g_slave_ringbuf = NULL;
        return -1;
    }

    g_slave_ringbuf->producer_head = 0;
    g_slave_ringbuf->consumer_tail = 0;

    /* 构造握手响应 */
    int n = snprintf(resp, resp_cap,
        "+KPROBERDMA %u %lu %zu %zu %zu\r\n",
        g_slave_ringbuf_mr->rkey,
        (unsigned long)g_slave_ringbuf,
        (size_t)KPROBE_RDMA_RINGBUF_SIZE,
        (size_t)KPROBE_RDMA_SLOT_COUNT,
        (size_t)KPROBE_RDMA_SLOT_CAPACITY);

    if (n < 0 || (size_t)n >= resp_cap) {
        ibv_dereg_mr(g_slave_ringbuf_mr);
        g_slave_ringbuf_mr = NULL;
        kvs_free(g_slave_ringbuf);
        g_slave_ringbuf = NULL;
        return -1;
    }

    /* 启动轮询线程 */
    g_kprobe_running = 1;
    if (!g_slave_poll_started) {
        if (pthread_create(&g_slave_poll_tid, NULL,
                kprobe_rdma_slave_poll, NULL) == 0) {
            pthread_detach(g_slave_poll_tid);
            g_slave_poll_started = 1;
        }
    }

    fprintf(stderr, "kprobe rdma: slave MR registered, rkey=%u addr=%lu\n",
        g_slave_ringbuf_mr->rkey, (unsigned long)g_slave_ringbuf);
    return 0;
}

/* ============================================================
 * 对外接口
 * ============================================================ */

/* Master 侧初始化 */
int repl_kprobe_rdma_master_init(void) {
    if (!g_cfg.kprobe_enabled) return 0;

    /* 分配 WRITE slots */
    for (int i = 0; i < KVS_RDMA_WRITE_SLOTS; i++) {
        memset(g_wr_slots[i].buf, 0, KVS_RDMA_WRITE_SLOT_SIZE);
        g_wr_slots[i].in_flight = 0;
        g_wr_slots[i].mr = NULL;
    }

    /* 加载 BPF */
    if (kprobe_load_bpf() != 0) {
        fprintf(stderr, "kprobe rdma: BPF load failed, kprobe disabled\n");
        g_cfg.kprobe_enabled = 0;
        return -1;
    }

    /* 设置 PID 过滤 (key 0 = enabled, key 1 = pid) */
    pid_t pid = getpid();
    __u32 key = 0;
    __u64 en = 1;
    if (g_kprobe_ctl_fd >= 0)
        bpf_map_update_elem(g_kprobe_ctl_fd, &key, &en, BPF_ANY);
    key = 1;
    __u64 pid_val = (__u64)pid;
    if (g_kprobe_ctl_fd >= 0)
        bpf_map_update_elem(g_kprobe_ctl_fd, &key, &pid_val, BPF_ANY);

    fprintf(stderr, "kprobe rdma: master init OK, PID=%d\n", (int)pid);
    return 0;
}

/* Slave 侧初始化 */
/* Slave 侧: kprobe-rdma listener 线程
 * 监听端口 g_cfg.master_port + 12（或 g_cfg.port + 12)，等待 Master 连接 */
static void *kprobe_rdma_slave_listener(void *arg) {
    (void)arg;
    struct rdma_event_channel *ec = NULL;
    struct rdma_cm_id *listen_id = NULL, *child_id = NULL;

    /* 端口: 使用 master_port（slave 已知 master 的端口） */
    int base_port = (g_cfg.role == ROLE_SLAVE && g_cfg.master_port > 0)
        ? g_cfg.master_port : g_cfg.port;
    int kprobe_port = g_cfg.rdma_port > 0 ? g_cfg.rdma_port : (base_port + 12);

    fprintf(stderr, "kprobe rdma: slave listener starting on port %d\n", kprobe_port);

    ec = rdma_create_event_channel();
    if (!ec) { fprintf(stderr, "kprobe rdma: listener ec create failed\n"); goto out; }

    if (rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP) != 0)
        { fprintf(stderr, "kprobe rdma: listener create_id failed\n"); goto out; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)kprobe_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (rdma_bind_addr(listen_id, (struct sockaddr *)&addr) != 0)
        { fprintf(stderr, "kprobe rdma: listener bind failed\n"); goto out; }

    if (rdma_listen(listen_id, 1) != 0)
        { fprintf(stderr, "kprobe rdma: listener listen failed\n"); goto out; }

    fprintf(stderr, "kprobe rdma: slave listener ready on port %d\n", kprobe_port);

    while (g_kprobe_running) {
        struct rdma_cm_event *event = NULL;
        if (rdma_get_cm_event(ec, &event) != 0) break;
        if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
            rdma_ack_cm_event(event);
            continue;
        }

        child_id = event->id;
        rdma_ack_cm_event(event);
        fprintf(stderr, "kprobe rdma: slave listener - CONNECT_REQUEST received\n");

        /* PD + CQ + QP */
        struct ibv_pd *pd = ibv_alloc_pd(child_id->verbs);
        if (!pd) { fprintf(stderr, "kprobe rdma: listener alloc_pd failed\n"); break; }

        struct ibv_cq *cq = ibv_create_cq(child_id->verbs, 16, NULL, NULL, 0);
        if (!cq) { ibv_dealloc_pd(pd); fprintf(stderr, "kprobe rdma: listener create_cq failed\n"); break; }

        struct ibv_qp_init_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.send_cq = cq;
        attr.recv_cq = cq;
        attr.qp_type = IBV_QPT_RC;
        attr.cap.max_send_wr = 16;
        attr.cap.max_recv_wr = 16;
        attr.cap.max_send_sge = 1;
        attr.cap.max_recv_sge = 1;
        if (rdma_create_qp(child_id, pd, &attr) != 0)
            { ibv_destroy_cq(cq); ibv_dealloc_pd(pd);
              fprintf(stderr, "kprobe rdma: listener create_qp failed\n"); break; }

        /* 注册 MR */
        char resp_buf[256];
        if (repl_kprobe_rdma_slave_accept(pd, resp_buf, sizeof(resp_buf)) != 0)
            { fprintf(stderr, "kprobe rdma: listener slave_accept failed\n"); break; }

        /* Accept */
        struct rdma_conn_param param;
        memset(&param, 0, sizeof(param));
        param.responder_resources = 1;
        param.initiator_depth = 1;
        param.rnr_retry_count = 3;
        if (rdma_accept(child_id, &param) != 0)
            { fprintf(stderr, "kprobe rdma: listener rdma_accept failed\n"); break; }

        /* 等待 ESTABLISHED */
        if (rdma_get_cm_event(ec, &event) != 0) break;
        if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
            fprintf(stderr, "kprobe rdma: listener expected ESTABLISHED got %d\n",
                event->event);
            rdma_ack_cm_event(event); break;
        }
        rdma_ack_cm_event(event);

        fprintf(stderr, "kprobe rdma: [OK] slave listener accepted, MR ready "
            "rkey=%u (waiting for KPROBEMR request)\n",
            g_slave_ringbuf_mr->rkey);

        /* 等待断开，同时保持 listener 存活 */
        while (g_kprobe_running) {
            if (rdma_get_cm_event(ec, &event) != 0) break;
            if (event->event == RDMA_CM_EVENT_DISCONNECTED ||
                event->event == RDMA_CM_EVENT_TIMEWAIT_EXIT) {
                rdma_ack_cm_event(event);
                break;
            }
            rdma_ack_cm_event(event);
        }
        break; /* 单次连接后退出，等待重新连接 */
    }

out:
    if (child_id) rdma_destroy_id(child_id);
    if (listen_id) rdma_destroy_id(listen_id);
    if (ec) rdma_destroy_event_channel(ec);
    fprintf(stderr, "kprobe rdma: slave listener exiting\n");
    return NULL;
}

int repl_kprobe_rdma_slave_init(void) {
    g_kprobe_running = 1;
    /* 启动 listener 线程 */
    pthread_t tid;
    if (pthread_create(&tid, NULL, kprobe_rdma_slave_listener, NULL) == 0) {
        pthread_detach(tid);
        fprintf(stderr, "kprobe rdma: slave listener thread started\n");
    } else {
        fprintf(stderr, "kprobe rdma: failed to start slave listener\n");
    }
    return 0;
}

/* Master 侧建立 RDMA QP + 交换 MR 信息
 * 通过 TCP 控制通道发送 REPLSYNC，从响应中解析 MR 信息 */
int repl_kprobe_rdma_establish(const char *host, int port) {
    if (!host || port <= 0) return -1;

    fprintf(stderr, "kprobe rdma: establishing connection to %s:%d\n", host, port);

    /* 1. 建立 RDMA QP */
    if (kprobe_rdma_qp_connect(host, port) != 0) {
        fprintf(stderr, "kprobe rdma: QP connect failed\n");
        return -1;
    }

    g_kprobe_running = 1;
    /* forward 线程在 parse_mr_info / parse_mr_info_direct 中
     * 收到 MR 信息后才启动，避免提前处理无效 ringbuf events */
    return 0;
}

/* 从握手响应中解析 Slave MR 信息 */
int repl_kprobe_rdma_parse_mr_info(const char *resp) {
    /* 格式: +KPROBERDMA <rkey> <addr> <size> <slot_count> <slot_cap>\r\n */
    unsigned long rkey, addr, size, slot_count, slot_cap;
    if (sscanf(resp, "+KPROBERDMA %lu %lu %lu %lu %lu",
               &rkey, &addr, &size, &slot_count, &slot_cap) != 5) {
        return -1;
    }
    g_slave_mr.rkey = (uint32_t)rkey;
    g_slave_mr.remote_data_base = addr + 16;  /* 跳过 head(8) + tail(8) */
    g_slave_mr.remote_head_addr = addr;        /* head 在偏移 0 */
    g_slave_mr.slot_count = (size_t)slot_count;
    g_slave_mr.slot_capacity = (size_t)slot_cap;
    __sync_synchronize();

    /* MR 就绪后启动 forward 线程 */
    if (!g_master_forward_started) {
        if (pthread_create(&g_master_forward_tid, NULL,
                kprobe_rdma_forward_thread, NULL) == 0) {
            pthread_detach(g_master_forward_tid);
            g_master_forward_started = 1;
            fprintf(stderr, "kprobe rdma: forward thread started (MR ready)\n");
        }
    }

    fprintf(stderr, "kprobe rdma: MR info parsed - rkey=%u data_base=0x%lx "
        "slots=%zu cap=%zu\n",
        g_slave_mr.rkey, (unsigned long)g_slave_mr.remote_data_base,
        g_slave_mr.slot_count, g_slave_mr.slot_capacity);
    return 0;
}

/* 设置 kprobe PID 过滤 (key 1 = KVS_KPROBE_CTL_PID) */
int repl_kprobe_rdma_set_pid(pid_t pid) {
    if (g_kprobe_ctl_fd < 0) return -1;
    __u32 key = 1;
    __u64 val = (__u64)pid;
    return bpf_map_update_elem(g_kprobe_ctl_fd, &key, &val, BPF_ANY);
}

int repl_kprobe_rdma_set_fd(int fd) {
    (void)fd;
    /* kprobe BPF 当前使用 PID 过滤，不直接支持 fd 过滤。
     * 此函数保留供未来 BPF 扩展使用。 */
    fprintf(stderr, "kprobe rdma: set_fd(%d) - no-op (PID filter active)\n", fd);
    return 0;
}

/* 清理 */
void repl_kprobe_rdma_cleanup(void) {
    g_kprobe_running = 0;

    /* 停止 kprobe (key 0 = KVS_KPROBE_CTL_ENABLED) */
    __u32 ctl_key = 0;
    __u64 ctl_val = 0;
    if (g_kprobe_ctl_fd >= 0)
        bpf_map_update_elem(g_kprobe_ctl_fd, &ctl_key, &ctl_val, BPF_ANY);
    kprobe_unload_bpf();

    /* 清理 RDMA 资源 */
    pthread_mutex_lock(&g_kprobe_rdma_lock);
    if (g_rdma_kprobe.connected) {
        if (g_rdma_kprobe.id) rdma_disconnect(g_rdma_kprobe.id);
        g_rdma_kprobe.connected = 0;
    }
    for (int i = 0; i < KVS_RDMA_WRITE_SLOTS; i++) {
        if (g_wr_slots[i].mr) {
            ibv_dereg_mr(g_wr_slots[i].mr);
            g_wr_slots[i].mr = NULL;
        }
    }
    if (g_head_mr) {
        ibv_dereg_mr(g_head_mr);
        g_head_mr = NULL;
    }
    if (g_rdma_kprobe.cq) {
        ibv_destroy_cq(g_rdma_kprobe.cq);
        g_rdma_kprobe.cq = NULL;
    }
    if (g_rdma_kprobe.pd) {
        ibv_dealloc_pd(g_rdma_kprobe.pd);
        g_rdma_kprobe.pd = NULL;
    }
    if (g_rdma_kprobe.id) {
        rdma_destroy_id(g_rdma_kprobe.id);
        g_rdma_kprobe.id = NULL;
    }
    if (g_rdma_kprobe.ec) {
        rdma_destroy_event_channel(g_rdma_kprobe.ec);
        g_rdma_kprobe.ec = NULL;
    }
    g_wr_head = 0;
    g_wr_in_flight = 0;
    g_wr_producer_seq = 0;
    /* volatile 指针转 void* 清除 memset warning */
    { volatile typeof(g_slave_mr) *vp = &g_slave_mr; memset((void *)vp, 0, sizeof(g_slave_mr)); }
    pthread_mutex_unlock(&g_kprobe_rdma_lock);

    /* 清理 Slave 侧 MR */
    if (g_slave_ringbuf_mr) {
        ibv_dereg_mr(g_slave_ringbuf_mr);
        g_slave_ringbuf_mr = NULL;
    }
    if (g_slave_ringbuf) {
        kvs_free(g_slave_ringbuf);
        g_slave_ringbuf = NULL;
    }

    g_master_forward_started = 0;
    g_slave_poll_started = 0;

    fprintf(stderr, "kprobe rdma: cleaned up\n");
}

/* 获取当前 MR 信息文本格式（用于 KPROBEMR 响应） */
int repl_kprobe_rdma_get_mr_text(char *buf, size_t cap) {
    if (!buf || cap < 64) return -1;
    if (!g_slave_ringbuf_mr || !g_slave_ringbuf) {
        return snprintf(buf, cap, "+KPROBERDMA 0 0 0 0 0\r\n");
    }
    return snprintf(buf, cap, "+KPROBERDMA %u %lu %zu %zu %zu\r\n",
        g_slave_ringbuf_mr->rkey,
        (unsigned long)g_slave_ringbuf,
        (size_t)KPROBE_RDMA_RINGBUF_SIZE,
        (size_t)KPROBE_RDMA_SLOT_COUNT,
        (size_t)KPROBE_RDMA_SLOT_CAPACITY);
}

/* 直接设置 Slave MR 信息（从 reactor 线程解析 +KPROBERDMA 后调用） */
int repl_kprobe_rdma_parse_mr_info_direct(uint32_t rkey, uint64_t addr,
    size_t total_size, size_t slot_count, size_t slot_capacity)
{
    (void)total_size;
    g_slave_mr.rkey = rkey;
    g_slave_mr.remote_data_base = addr + 16;
    g_slave_mr.remote_head_addr = addr;
    g_slave_mr.slot_count = slot_count;
    g_slave_mr.slot_capacity = slot_capacity;
    __sync_synchronize();

    /* MR 就绪后启动 forward 线程，避免处理 KPROBEMR 前的无效 events */
    g_kprobe_running = 1;
    if (!g_master_forward_started) {
        if (pthread_create(&g_master_forward_tid, NULL,
                kprobe_rdma_forward_thread, NULL) == 0) {
            pthread_detach(g_master_forward_tid);
            g_master_forward_started = 1;
            fprintf(stderr, "kprobe rdma: forward thread started (MR ready)\n");
        }
    }

    fprintf(stderr, "kprobe rdma: MR info updated - rkey=%u addr=0x%lx data_base=0x%lx head_addr=0x%lx slots=%zu cap=%zu\n",
        rkey, (unsigned long)addr,
        (unsigned long)g_slave_mr.remote_data_base,
        (unsigned long)g_slave_mr.remote_head_addr,
        slot_count, slot_capacity);
    return 0;
}

/* 获取统计信息 */
int repl_kprobe_rdma_get_stats(kvs_repl_kprobe_stats_t *stats) {
    if (!stats) return -1;
    memset(stats, 0, sizeof(*stats));

    stats->total_events = g_total_events;
    stats->total_bytes = g_total_bytes;
    stats->rdma_writes = g_rdma_writes;
    stats->rdma_errors = g_rdma_errors;
    stats->rdma_connected = g_rdma_kprobe.connected;
    stats->kprobe_initialized = (g_kprobe_obj != NULL);

    /* 从 BPF map 读取 kprobe 统计 */
    __u64 val = 0;
    if (kprobe_get_stat(0, &val) == 0)  /* KVS_KPROBE_STAT_HIT */
        stats->kprobe_hits = (unsigned long long)val;

    return 0;
}

/* 用户态写入 ringbuf 的桩函数
 * 主路径完全由 kprobe 在 tcp_sendmsg 中透明拦截，此函数保留
 * 供未来调试/测试使用。当前 kprobe 路径正常工作时不需调用。 */
int repl_kprobe_rdma_enqueue(const unsigned char *data, size_t len) {
    (void)data; (void)len;
    return 0;  /* 主路径走 kprobe 透明拦截 */
}

/* REPLDONE 探测回调 — 由 BPF ringbuf 回调或用户态 queue_snapshot 触发
 * 通知系统全量同步已完成，可以 flush 缓存数据并开始增量同步 */
void repl_kprobe_fullsync_done(void) {
    fprintf(stderr, "kprobe: fullsync done callback triggered\n");
    /* 实际 flush 操作由 queue_snapshot() 中的 repl_client_capture_flush_to_slave()
     * 完成。此函数主要负责日志和状态标记。 */
}

/* ============================================================
 * Client Capture — 用户态管理
 * ============================================================
 *
 * 数据流:
 *   BPF kretprobe/tcp_recvmsg → client_cache_ringbuf
 *     → client_ringbuf_cb() → 缓存队列 (g_cache_list)
 *     → FULLSYNC_IN_PROGRESS=1: 追加到队列 (L1→L2 spill)
 *     → FULLSYNC_IN_PROGRESS=0: 直接转发到 slave TCP (新路径)
 *     → queue_snapshot 完成后: repl_client_capture_flush_to_slave()
 */

/* 缓存队列节点 */
typedef struct client_cache_node_s {
    unsigned char *data;
    size_t len;
    struct client_cache_node_s *next;
} client_cache_node_t;

/* L2 磁盘缓存配置 */
#define CACHE_L1_MAX_BYTES  (4 * 1024 * 1024)   /* L1 内存上限 4MB，超过则 spill 到磁盘 */
#define CACHE_L2_PATH       "/tmp/kvstore_fs_cache"

/* Client Capture 全局状态 */
static struct bpf_object *g_client_obj = NULL;
static int g_client_ctl_fd = -1;
static int g_client_stats_fd = -1;
static int g_client_ringbuf_fd = -1;
static struct ring_buffer *g_client_ringbuf = NULL;
static volatile int g_client_running = 0;
static struct bpf_link *g_client_links[4] = {NULL};
static int g_client_link_count = 0;

/* L1 缓存队列 (内存) */
static client_cache_node_t *g_cache_head = NULL;
static client_cache_node_t *g_cache_tail = NULL;
static pthread_mutex_t g_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_cache_count = 0;
static volatile unsigned long long g_cache_bytes = 0;

/* L2 磁盘缓存 (当 L1 超过 CACHE_L1_MAX_BYTES 时启用) */
static int g_cache_l2_fd = -1;          /* L2 磁盘文件 fd */
static char g_cache_l2_path[512] = {0}; /* L2 磁盘文件路径 */
static unsigned long long g_cache_l2_bytes = 0; /* L2 已写入字节数 */

/* 前向声明 */
static int cache_spill_to_l2(const unsigned char *data, size_t len);

/* ── eBPF+tcp 新路径: INCR 模式直接转发到 slave ──
 * 当前禁用: bpf_probe_read_user 在内核 5.15 上可靠性不足，
 * 增量数据改由 repl_broadcast (TCP) 可靠发送。保留此函数供未来修复后启用。 */
__attribute__((unused))
static int forward_to_slave(const unsigned char *data, size_t len) {
    extern int g_repl_capture_slave_fd;
    if (g_repl_capture_slave_fd < 0) return -1;

    /* 过滤复制控制命令 — BPF 捕获了 slave→master 的控制消息（REPLSYNC/REPLACK），
     * 若转发回 slave 会导致解析错误和 buffer 堆积 */
    if (len >= 7) {
        const char *p = (const char *)data;
        size_t scan = len < 128 ? len : 128;
        /* 简单扫描 — RESP 命令名格式: $N\r\n<CMD>\r\n，只需匹配 CMD 部分 */
        for (size_t i = 0; i + 8 <= scan; i++) {
            if ((memcmp(p + i, "REPLSYNC", 8) == 0) ||
                (memcmp(p + i, "REPLACK", 7) == 0)) {
                return 0;  /* 静默丢弃 */
            }
        }
    }

    size_t total_sent = 0;
    int err_count = 0;

    while (total_sent < len) {
        ssize_t sent = send(g_repl_capture_slave_fd, data + total_sent,
                            len - total_sent, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (++err_count > 100) {
                    static long long last_stuck_warn = 0;
                    long long now = kvs_now_ms();
                    if (now - last_stuck_warn > 5000) {
                        last_stuck_warn = now;
                        fprintf(stderr, "client_capture: forward stuck (EAGAIN x%d) fd=%d\n",
                                err_count, g_repl_capture_slave_fd);
                    }
                    return -1;
                }
                usleep(1000);
                continue;
            }
            static long long last_err_ms = 0;
            long long now = kvs_now_ms();
            if (now - last_err_ms > 5000) {
                last_err_ms = now;
                fprintf(stderr, "client_capture: forward send error fd=%d errno=%d\n",
                        g_repl_capture_slave_fd, errno);
            }
            return -1;
        }
        total_sent += (size_t)sent;
        err_count = 0;
    }
    return 0;
}

/* Ringbuf 回调 — BPF 有客户端数据到达时触发 */
static int client_ringbuf_cb(void *ctx, void *data, size_t size) {
    (void)ctx;
    if (size < 4) return 0;

    __u32 payload_len;
    memcpy(&payload_len, data, 4);
    if (payload_len == 0 || payload_len + 4 > size) return 0;

    unsigned char *payload = (unsigned char *)data + 4;

    /* 过滤复制控制命令 — BPF 捕获了 slave→master 的数据（REPLSYNC/REPLACK），
     * 无论是增量转发还是全量缓存，都静默丢弃，防止 slave 收到后解析失败 */
    if (payload_len >= 7) {
        size_t scan = payload_len < 128 ? payload_len : 128;
        for (size_t i = 0; i + 7 <= scan; i++) {
            if ((scan - i >= 8 && memcmp(payload + i, "REPLSYNC", 8) == 0) ||
                memcmp(payload + i, "REPLACK", 7) == 0) {
                return 0;  /* 静默丢弃 */
            }
        }
    }

    if (g_repl_fullsync_in_progress) {
        /* ═══ STATE_FULL: 全量同步中 → 缓存 ═══ */

        /* L1 内存已满 → spill 到 L2 磁盘 */
        if (g_cache_bytes >= CACHE_L1_MAX_BYTES) {
            cache_spill_to_l2(payload, payload_len);
            return 0;
        }

        /* L1 内存缓存 */
        client_cache_node_t *node = (client_cache_node_t *)
            kvs_malloc(sizeof(client_cache_node_t));
        if (!node) return -1;

        node->data = (unsigned char *)kvs_malloc(payload_len);
        if (!node->data) { kvs_free(node); return -1; }
        memcpy(node->data, payload, payload_len);
        node->len = payload_len;
        node->next = NULL;

        pthread_mutex_lock(&g_cache_lock);
        if (g_cache_tail) {
            g_cache_tail->next = node;
        } else {
            g_cache_head = node;
        }
        g_cache_tail = node;
        g_cache_count++;
        g_cache_bytes += payload_len;
        pthread_mutex_unlock(&g_cache_lock);
    }
    /* STATE_INCR: 增量同步 — 数据由 repl_broadcast 可靠发送（TCP），
     * eBPF 转发因 bpf_probe_read_user 不可靠而禁用 */
    return 0;
}

/* ── L2 磁盘缓存 ── */
static int cache_spill_to_l2(const unsigned char *data, size_t len) {
    /* 首次 spill: 创建 L2 文件 */
    if (g_cache_l2_fd < 0) {
        snprintf(g_cache_l2_path, sizeof(g_cache_l2_path), "%s.%d",
                 CACHE_L2_PATH, getpid());
        g_cache_l2_fd = open(g_cache_l2_path,
                             O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (g_cache_l2_fd < 0) {
            fprintf(stderr, "client_capture: L2 file create failed: %s\n",
                    strerror(errno));
            return -1;
        }
        fprintf(stderr, "client_capture: L2 cache opened: %s\n", g_cache_l2_path);
    }

    /* 写入格式: [4B len][payload] */
    uint32_t net_len = (uint32_t)len;
    unsigned char hdr[4];
    memcpy(hdr, &net_len, 4);

    if (write(g_cache_l2_fd, hdr, 4) != 4) goto write_err;
    if (write(g_cache_l2_fd, data, len) != (ssize_t)len) goto write_err;

    g_cache_l2_bytes += len;
    return 0;

write_err:
    fprintf(stderr, "client_capture: L2 write failed: %s\n", strerror(errno));
    return -1;
}

/* 从 L2 磁盘读取所有缓存数据，通过 repl_send_chunked 发送到 slave
 * 返回: 读取的条目数 */
static int cache_flush_l2_to_slave(conn_t *c) {
    if (g_cache_l2_fd < 0) return 0;

    /* 关闭写端，打开读端 */
    close(g_cache_l2_fd);
    g_cache_l2_fd = open(g_cache_l2_path, O_RDONLY);
    if (g_cache_l2_fd < 0) {
        fprintf(stderr, "client_capture: L2 reopen failed: %s\n", strerror(errno));
        return 0;
    }

    int count = 0;
    unsigned char hdr[4];
    unsigned char buf[BUFFER_CAP];

    while (read(g_cache_l2_fd, hdr, 4) == 4) {
        uint32_t len;
        memcpy(&len, hdr, 4);
        if (len == 0 || len > BUFFER_CAP) break;

        ssize_t r = read(g_cache_l2_fd, buf, len);
        if (r != (ssize_t)len) break;

        if (repl_send_chunked(c, buf, len) != 0) {
            fprintf(stderr, "client_capture: L2 chunk send failed\n");
            break;
        }
        count++;
    }

    close(g_cache_l2_fd);
    g_cache_l2_fd = -1;
    unlink(g_cache_l2_path);
    g_cache_l2_path[0] = '\0';
    g_cache_l2_bytes = 0;

    g_cc_l2_flushed += count;
    fprintf(stderr, "client_capture: L2 flushed %d entries\n", count);
    return count;
}

/* Client capture ringbuf 轮询线程 */
static pthread_t g_client_poll_tid;
static int g_client_poll_started = 0;

static void *client_poll_thread(void *arg) {
    (void)arg;
    fprintf(stderr, "client_capture: poll thread started\n");

    while (g_client_running && g_client_ringbuf) {
        int err = ring_buffer__poll(g_client_ringbuf, 50);  /* 50ms 超时 */
        if (err < 0 && err != -EAGAIN) {
            fprintf(stderr, "client_capture: ringbuf poll err=%d\n", err);
            usleep(10000);
        }
    }

    fprintf(stderr, "client_capture: poll thread exiting\n");
    return NULL;
}

/* 初始化 client capture BPF */
int repl_client_capture_init(void) {
    if (g_client_obj) return 0;  /* 已经初始化 */

    if (!g_cfg.repl_kprobe_obj_path[0]) {
        fprintf(stderr, "client_capture: missing repl_kprobe_obj_path config\n");
        return -1;
    }

    /* 替换对象文件路径: 使用 client_capture BPF 对象 */
    /* 目前与 kprobe 共用对象文件路径，后续可独立配置 */
    char obj_path[512];
    size_t base_len = strlen(g_cfg.repl_kprobe_obj_path);
    if (base_len > 4 && strcmp(g_cfg.repl_kprobe_obj_path + base_len - 12,
                                "repl_kprobe.bpf.o") == 0) {
        /* 在同一目录下查找 client_capture 对象 */
        snprintf(obj_path, sizeof(obj_path), "%.*srepl_client_capture.bpf.o",
                 (int)(base_len - 12), g_cfg.repl_kprobe_obj_path);
    } else {
        snprintf(obj_path, sizeof(obj_path), "%s",
                 "build/replication/bpf/repl_client_capture.bpf.o");
    }

    /* 设置 libbpf 日志级别 — 打印错误信息 */
    libbpf_set_print(NULL);

    fprintf(stderr, "client_capture: opening BPF object: %s\n", obj_path);

    /* 打开 BPF 对象 */
    g_client_obj = bpf_object__open_file(obj_path, NULL);
    if (libbpf_get_error(g_client_obj)) {
        long err = libbpf_get_error(g_client_obj);
        fprintf(stderr, "client_capture: failed to open BPF object: %s (err=%ld)\n",
                obj_path, err);
        g_client_obj = NULL;
        return -1;
    }

    /* 加载 BPF 对象 */
    if (bpf_object__load(g_client_obj) != 0) {
        int load_err = -errno;
        if (load_err == 0) load_err = -1;
        fprintf(stderr, "client_capture: failed to load BPF object (errno=%d %s)\n",
                errno, strerror(errno));
        bpf_object__close(g_client_obj);
        g_client_obj = NULL;
        return -1;
    }

    /* ═══ Attach kprobe/kretprobe 到 tcp_recvmsg ═══
     * libbpf 0.5.0 的 bpf_object__load() 不会自动 attach，
     * 必须显式调用 bpf_program__attach_kprobe()。 */
    {
        struct bpf_program *prog;

        prog = bpf_object__find_program_by_name(g_client_obj,
                                                "kprobe_client_recv_entry");
        if (prog) {
            struct bpf_link *link = bpf_program__attach_kprobe(
                prog, false /* kprobe */, "tcp_recvmsg");
            if (!libbpf_get_error(link) && g_client_link_count < 4) {
                g_client_links[g_client_link_count++] = link;
                fprintf(stderr, "client_capture: attached kprobe/tcp_recvmsg\n");
            } else {
                fprintf(stderr, "client_capture: failed to attach kprobe/tcp_recvmsg (err=%ld)\n",
                        libbpf_get_error(link));
            }
        }

        prog = bpf_object__find_program_by_name(g_client_obj,
                                                "kprobe_client_recv_return");
        if (prog) {
            struct bpf_link *link = bpf_program__attach_kprobe(
                prog, true /* kretprobe */, "tcp_recvmsg");
            if (!libbpf_get_error(link) && g_client_link_count < 4) {
                g_client_links[g_client_link_count++] = link;
                fprintf(stderr, "client_capture: attached kretprobe/tcp_recvmsg\n");
            } else {
                fprintf(stderr, "client_capture: failed to attach kretprobe/tcp_recvmsg (err=%ld)\n",
                        libbpf_get_error(link));
            }
        }

        if (g_client_link_count == 0) {
            fprintf(stderr, "client_capture: no BPF programs attached\n");
            repl_client_capture_cleanup();
            return -1;
        }
    }

    /* 获取 map FDs */
    g_client_ctl_fd = bpf_object__find_map_fd_by_name(g_client_obj, "client_ctl");
    g_client_stats_fd = bpf_object__find_map_fd_by_name(g_client_obj, "client_stats");
    g_client_ringbuf_fd = bpf_object__find_map_fd_by_name(g_client_obj, "client_cache_ringbuf");
    if (g_client_ctl_fd < 0 || g_client_stats_fd < 0 || g_client_ringbuf_fd < 0) {
        fprintf(stderr, "client_capture: failed to find BPF maps\n");
        repl_client_capture_cleanup();
        return -1;
    }

    /* 设置 PID 过滤 = 当前进程 */
    __u32 pid_key = 1;  /* PID */
    __u64 pid_val = (__u64)getpid();
    bpf_map_update_elem(g_client_ctl_fd, &pid_key, &pid_val, 0);

    /* 启用 BPF 程序 */
    __u32 enable_key = 0;  /* ENABLED */
    __u64 enable_val = 1;
    bpf_map_update_elem(g_client_ctl_fd, &enable_key, &enable_val, 0);

    /* 创建 ringbuf */
    g_client_ringbuf = ring_buffer__new(g_client_ringbuf_fd,
                                         client_ringbuf_cb, NULL, NULL);
    if (!g_client_ringbuf) {
        fprintf(stderr, "client_capture: failed to create ring buffer\n");
        repl_client_capture_cleanup();
        return -1;
    }

    /* 启动 ringbuf 轮询线程 */
    g_client_running = 1;
    if (pthread_create(&g_client_poll_tid, NULL, client_poll_thread, NULL) != 0) {
        fprintf(stderr, "client_capture: failed to create poll thread\n");
        g_client_running = 0;
        repl_client_capture_cleanup();
        return -1;
    }
    pthread_detach(g_client_poll_tid);
    g_client_poll_started = 1;

    /* 激活 eBPF+tcp 新增量路径 — repl_broadcast 将跳过实时发送 */
    extern volatile int g_repl_client_capture_active;
    extern int g_repl_capture_slave_fd;
    g_repl_client_capture_active = 1;
    g_repl_capture_slave_fd = -1;

    fprintf(stderr, "client_capture: initialized (pid=%d), capture active\n", getpid());
    return 0;
}

/* 设置全量同步状态 — 通知 BPF 当前是否在全量同步中 */
void repl_client_capture_set_fullsync(int in_progress) {
    if (g_client_ctl_fd < 0) return;
    __u32 key = 3;  /* FULLSYNC_IN_PROGRESS */
    __u64 val = in_progress ? 1 : 0;
    bpf_map_update_elem(g_client_ctl_fd, &key, &val, 0);
    fprintf(stderr, "client_capture: fullsync_in_progress=%d\n", in_progress);
}

/* Flush 缓存的客户端数据到 slave — 通过 TCP 发送
 * 返回: 刷新的条目数（0=无缓存数据, >0=已刷新N条） */
int repl_client_capture_flush_to_slave(conn_t *c) {
    if (!c) return 0;
    int total_count = 0;

    /* ── Step 1: Flush L1 内存队列 ── */
    pthread_mutex_lock(&g_cache_lock);
    client_cache_node_t *node = g_cache_head;
    g_cache_head = NULL;
    g_cache_tail = NULL;
    int l1_count = g_cache_count;
    unsigned long long l1_bytes = g_cache_bytes;
    g_cache_count = 0;
    g_cache_bytes = 0;
    pthread_mutex_unlock(&g_cache_lock);

    if (node) {
        g_cc_l1_flushed++;
        fprintf(stderr, "client_capture: flushing L1 (%d entries, %llu bytes)\n",
                l1_count, l1_bytes);

        client_cache_node_t *cur = node;
        int sent_ok = 1;
        while (cur) {
            if (repl_send_chunked(c, cur->data, cur->len) != 0) {
                fprintf(stderr, "client_capture: L1 flush send failed\n");
                sent_ok = 0;
                break;
            }
            client_cache_node_t *next = cur->next;
            kvs_free(cur->data);
            kvs_free(cur);
            cur = next;
            total_count++;
        }
        /* 清理剩余（发送失败） */
        if (!sent_ok) {
            while (cur) {
                client_cache_node_t *next = cur->next;
                kvs_free(cur->data);
                kvs_free(cur);
                cur = next;
            }
        }
    }

    /* ── Step 2: Flush L2 磁盘缓存 ── */
    int l2_count = cache_flush_l2_to_slave(c);
    total_count += l2_count;

    if (total_count == 0) {
        fprintf(stderr, "client_capture: no cached data to flush\n");
        return 0;
    }

    fprintf(stderr, "client_capture: flush complete (L1=%d L2=%d total=%d)\n",
            l1_count, l2_count, total_count);
    return total_count;
}

/* 清理 client capture 资源 */
void repl_client_capture_cleanup(void) {
    g_client_running = 0;

    /* 先销毁 BPF links（必须在 bpf_object__close 之前） */
    for (int i = 0; i < g_client_link_count; i++) {
        if (g_client_links[i]) {
            bpf_link__destroy(g_client_links[i]);
            g_client_links[i] = NULL;
        }
    }
    g_client_link_count = 0;

    if (g_client_ringbuf) {
        ring_buffer__free(g_client_ringbuf);
        g_client_ringbuf = NULL;
    }
    if (g_client_obj) {
        bpf_object__close(g_client_obj);
        g_client_obj = NULL;
    }
    g_client_ctl_fd = -1;
    g_client_stats_fd = -1;
    g_client_ringbuf_fd = -1;

    /* 清理残留缓存节点 */
    pthread_mutex_lock(&g_cache_lock);
    client_cache_node_t *cur = g_cache_head;
    while (cur) {
        client_cache_node_t *next = cur->next;
        kvs_free(cur->data);
        kvs_free(cur);
        cur = next;
    }
    g_cache_head = NULL;
    g_cache_tail = NULL;
    g_cache_count = 0;
    g_cache_bytes = 0;
    pthread_mutex_unlock(&g_cache_lock);

    /* 清理 L2 磁盘缓存 */
    if (g_cache_l2_fd >= 0) {
        close(g_cache_l2_fd);
        g_cache_l2_fd = -1;
    }
    if (g_cache_l2_path[0]) {
        unlink(g_cache_l2_path);
        g_cache_l2_path[0] = '\0';
    }
    g_cache_l2_bytes = 0;

    /* 重置捕获标志 */
    extern volatile int g_repl_client_capture_active;
    g_repl_client_capture_active = 0;

    fprintf(stderr, "client_capture: cleaned up\n");
}

/* ── client_capture 统计查询（供 INFO 命令使用）── */
int repl_client_capture_get_stats(unsigned long long *hits,
    unsigned long long *cached, int *repldone_detect,
    int *l1_flushed, int *l2_flushed)
{
    /* BPF stats: read HIT counter from BPF map */
    unsigned long long bpf_hits = 0;
    if (g_client_stats_fd >= 0) {
        __u32 key = 0;  /* HIT */
        __u64 val = 0;
        if (bpf_map_lookup_elem(g_client_stats_fd, &key, &val) == 0)
            bpf_hits = (unsigned long long)val;
    }
    if (hits)    *hits    = bpf_hits;
    if (cached)  *cached  = (unsigned long long)g_cache_count;
    if (repldone_detect) *repldone_detect = g_cc_repldone_detect;
    if (l1_flushed) *l1_flushed = g_cc_l1_flushed;
    if (l2_flushed) *l2_flushed = g_cc_l2_flushed;
    return (g_client_obj != NULL && g_client_running) ? 1 : 0;
}

#else /* !KVS_ENABLE_KPROBE_RDMA */

/* 编译禁用时的桩实现 */
int repl_kprobe_rdma_master_init(void) { return 0; }
int repl_kprobe_rdma_slave_init(void) { return 0; }
int repl_kprobe_rdma_establish(const char *h, int p)
    { (void)h; (void)p; return -1; }
int repl_kprobe_rdma_slave_accept(struct ibv_pd *pd, char *r, size_t c)
    { (void)pd; (void)r; (void)c; return -1; }
void repl_kprobe_rdma_cleanup(void) {}
int repl_kprobe_rdma_get_stats(kvs_repl_kprobe_stats_t *s)
    { if (s) memset(s, 0, sizeof(*s)); return 0; }
int repl_kprobe_rdma_set_pid(pid_t p) { (void)p; return -1; }
int repl_kprobe_rdma_set_fd(int f) { (void)f; return -1; }
int repl_kprobe_rdma_parse_mr_info(const char *r) { (void)r; return -1; }
int repl_kprobe_rdma_enqueue(const unsigned char *d, size_t l)
    { (void)d; (void)l; return -1; }
void repl_kprobe_fullsync_done(void) {}
int repl_client_capture_init(void) { return -1; }
void repl_client_capture_set_fullsync(int p) { (void)p; }
int repl_client_capture_flush_to_slave(conn_t *c) { (void)c; return 0; }
void repl_client_capture_cleanup(void) {}
int repl_client_capture_get_stats(unsigned long long *hits,
    unsigned long long *cached, int *repldone_detect,
    int *l1_flushed, int *l2_flushed)
{
    if (hits) *hits = 0; if (cached) *cached = 0;
    if (repldone_detect) *repldone_detect = 0;
    if (l1_flushed) *l1_flushed = 0; if (l2_flushed) *l2_flushed = 0;
    return 0;
}

#endif /* KVS_ENABLE_KPROBE_RDMA */
