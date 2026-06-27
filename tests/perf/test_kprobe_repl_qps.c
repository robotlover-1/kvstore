/*
 * test_kprobe_repl_qps.c — kprobe 主从转发 QPS 对比
 *
 * 场景: Master 收到客户端请求 → echo 响应 + 转发给 Slave
 *
 * 三种模式:
 *   none:   纯 echo，不转发（基准 QPS）
 *   sync:   echo + 同步转发 slave（转发在主请求路径上）
 *   kprobe: echo + kprobe 截获 → ringbuf → 异步转发 slave
 *
 * 用法:
 *   sudo ./test_kprobe_repl_qps --mode none    --payload 64 --count 10000
 *   sudo ./test_kprobe_repl_qps --mode sync    --payload 64 --count 10000
 *   sudo ./test_kprobe_repl_qps --mode kprobe  --payload 64 --count 10000
 *   sudo ./test_kprobe_repl_qps --mode all     --payload 64 --count 10000
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"

#define CLIENT_PORT  15800
#define SLAVE_PORT   15801
#define BPF_OBJ      "./kprobe_capture.bpf.o"

/* 与 BPF 程序保持一致的 ctl key */
#define CTL_ENABLED   0
#define CTL_PID       1
#define CTL_PORT      2
#define CTL_MASTER_FD 3

static const char *g_bpf_obj  = BPF_OBJ;
static const char *g_slave_host = "127.0.0.1";
static int g_slave_port = SLAVE_PORT;
static int g_payload_size = 64;
static int g_req_count    = 10000;

/* ========== Slave 接收线程 ========== */
typedef struct {
    int port;
    int listen_fd;
    pthread_t thread;
    volatile int running;
    volatile int ready;
    int msg_count;
    long long total_bytes;
} slave_t;

static void *slave_thread(void *arg) {
    slave_t *s = (slave_t *)arg;

    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_ANY),
                               .sin_port = htons(s->port)};
    bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(s->listen_fd, 1);

    s->ready = 1;

    while (s->running) {
        int fd = accept(s->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        set_nodelay(fd);

        char buf[65536];
        while (1) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            s->msg_count++;
            s->total_bytes += n;
        }
        close(fd);
    }
    close(s->listen_fd);
    return NULL;
}

static int slave_start(slave_t *s, int port) {
    memset(s, 0, sizeof(*s));
    s->port = port;
    s->running = 1;
    if (pthread_create(&s->thread, NULL, slave_thread, s) != 0) return -1;
    while (!__atomic_load_n(&s->ready, __ATOMIC_ACQUIRE)) usleep(1000);
    return 0;
}

static void slave_stop(slave_t *s) {
    s->running = 0;
    if (s->listen_fd > 0) shutdown(s->listen_fd, SHUT_RDWR);
    pthread_join(s->thread, NULL);
}

/* ========== ringbuf 消费者（转发到 slave） ========== */
typedef struct {
    struct bpf_object *obj;
    int slave_fd;
    volatile int running;
    volatile int ready;
    int fwd_count;
} ringbuf_fwd_t;

static int ringbuf_cb(void *ctx, void *data, size_t len) {
    ringbuf_fwd_t *rf = (ringbuf_fwd_t *)ctx;
    if (len < 4) return 0;

    __u32 plen;
    __builtin_memcpy(&plen, data, 4);
    if (plen == 0 || plen > len - 4) return 0;

    if (write_full(rf->slave_fd, (const char *)data + 4, plen) < 0)
        return 1; /* stop */
    rf->fwd_count++;
    return 0;
}

static void *ringbuf_fwd_thread(void *arg) {
    ringbuf_fwd_t *rf = (ringbuf_fwd_t *)arg;

    struct bpf_map *rb_map = bpf_object__find_map_by_name(rf->obj, "ringbuf");
    if (!rb_map) { fprintf(stderr, "[fwd] ringbuf map not found\n"); return NULL; }

    struct ring_buffer *rb = ring_buffer__new(bpf_map__fd(rb_map), ringbuf_cb, rf, NULL);
    if (!rb) { fprintf(stderr, "[fwd] ring_buffer__new failed\n"); return NULL; }

    rf->ready = 1;
    fprintf(stderr, "[fwd] ready, polling ringbuf\n");

    while (rf->running) {
        int n = ring_buffer__poll(rb, 5);
        if (n < 0) { fprintf(stderr, "[fwd] poll error\n"); break; }
    }

    fprintf(stderr, "[fwd] exiting, forwarded=%d\n", rf->fwd_count);
    ring_buffer__free(rb);
    return NULL;
}

/* ========== kprobe 加载 ========== */
static void *load_kprobe_bpf(void) {
    struct rlimit rl = {RLIM_INFINITY, RLIM_INFINITY};
    setrlimit(RLIMIT_MEMLOCK, &rl);

    struct bpf_object *obj = bpf_object__open(g_bpf_obj);
    if (!obj) { fprintf(stderr, "kprobe: open failed\n"); return NULL; }

    /* 禁用 fentry/fexit/tp 程序，只保留 kprobe/kretprobe 的 autoload */
    { struct bpf_program *_p;
      _p = bpf_object__find_program_by_name(obj, "fentry_recv");
      if (_p) bpf_program__set_autoload(_p, false);
      _p = bpf_object__find_program_by_name(obj, "fexit_recv");
      if (_p) bpf_program__set_autoload(_p, false);
      _p = bpf_object__find_program_by_name(obj, "tp_sys_enter_read");
      if (_p) bpf_program__set_autoload(_p, false);
      _p = bpf_object__find_program_by_name(obj, "tp_sys_exit_read");
      if (_p) bpf_program__set_autoload(_p, false); }

    /* libbpf 1.1 + kernel 6.1: 手动 attach kprobe/kretprobe */
    if (bpf_object__load(obj) != 0) {
        fprintf(stderr, "kprobe: load failed\n");
        bpf_object__close(obj);
        return NULL;
    }

    struct bpf_program *prog;
    struct bpf_link *le = NULL, *lr = NULL;

    prog = bpf_object__find_program_by_name(obj, "kp_recv_entry");
    if (prog && bpf_program__fd(prog) >= 0) {
        le = bpf_program__attach_kprobe(prog, false, "tcp_recvmsg");
        if (libbpf_get_error(le)) {
            fprintf(stderr, "kprobe: kprobe attach failed: %ld\n", libbpf_get_error(le));
            le = NULL;
        }
    }
    prog = bpf_object__find_program_by_name(obj, "kp_recv_return");
    if (prog && bpf_program__fd(prog) >= 0) {
        lr = bpf_program__attach_kprobe(prog, true, "tcp_recvmsg");
        if (libbpf_get_error(lr)) {
            fprintf(stderr, "kprobe: kretprobe attach failed: %ld\n", libbpf_get_error(lr));
            lr = NULL;
        }
    }

    if (!le || !lr) {
        fprintf(stderr, "kprobe: attach failed\n");
        if (le) bpf_link__destroy(le);
        if (lr) bpf_link__destroy(lr);
        bpf_object__close(obj);
        return NULL;
    }

    /* 设置 PID 过滤 */
    struct bpf_map *ctl = bpf_object__find_map_by_name(obj, "ctl");
    if (ctl) {
        __u32 k0 = 0, k1 = 1;
        __u64 v1 = 1, vpid = (__u64)getpid();
        bpf_map_update_elem(bpf_map__fd(ctl), &k0, &v1, BPF_ANY);
        bpf_map_update_elem(bpf_map__fd(ctl), &k1, &vpid, BPF_ANY);
    }

    fprintf(stderr, "[kprobe] loaded, manual attach kprobe/kretprobe (PID=%d)\n", getpid());
    return obj;
}

/* ========== tracepoint 加载 ========== */
static void *load_tp_bpf(void) {
    struct rlimit rl = {RLIM_INFINITY, RLIM_INFINITY};
    setrlimit(RLIMIT_MEMLOCK, &rl);

    struct bpf_object *obj = bpf_object__open(g_bpf_obj);
    if (!obj) { fprintf(stderr, "tp: open failed\n"); return NULL; }

    /* 禁用非 tp 程序；tp 程序保持 autoload 以获取 fd，然后手动 attach */
    { struct bpf_program *_p;
      _p = bpf_object__find_program_by_name(obj, "fentry_recv");
      if (_p) bpf_program__set_autoload(_p, false);
      _p = bpf_object__find_program_by_name(obj, "fexit_recv");
      if (_p) bpf_program__set_autoload(_p, false);
      _p = bpf_object__find_program_by_name(obj, "fentry_recv");
      if (_p) bpf_program__set_autoload(_p, false);
      _p = bpf_object__find_program_by_name(obj, "fexit_recv");
      if (_p) bpf_program__set_autoload(_p, false);
    }

    if (bpf_object__load(obj) != 0) {
        fprintf(stderr, "tp: load failed\n");
        bpf_object__close(obj);
        return NULL;
    }

    /* libbpf 0.5.0 不支持 SEC("tp/...") 自动 attach，需手动 attach */
    struct bpf_program *prog;
    struct bpf_link *le = NULL, *lx = NULL;

    prog = bpf_object__find_program_by_name(obj, "tp_sys_enter_read");
    if (prog) {
        le = bpf_program__attach_tracepoint(prog, "syscalls", "sys_enter_read");
        if (libbpf_get_error(le)) {
            fprintf(stderr, "tp: attach sys_enter_read failed: %ld\n", libbpf_get_error(le));
            le = NULL;
        }
    }

    prog = bpf_object__find_program_by_name(obj, "tp_sys_exit_read");
    if (prog) {
        lx = bpf_program__attach_tracepoint(prog, "syscalls", "sys_exit_read");
        if (libbpf_get_error(lx)) {
            fprintf(stderr, "tp: attach sys_exit_read failed: %ld\n", libbpf_get_error(lx));
            lx = NULL;
        }
    }

    if (!le || !lx) {
        if (le) bpf_link__destroy(le);
        if (lx) bpf_link__destroy(lx);
        bpf_object__close(obj);
        return NULL;
    }

    /* CTL_PID(1) = pid（低32位），master_fd 初始为0，由 master 线程 accept 后更新 */
    struct bpf_map *ctl = bpf_object__find_map_by_name(obj, "ctl");
    if (ctl) {
        __u32 k1 = 1;
        /* 低32位=pid, 高32位=~0U (sentinel, 过滤所有 fd, accept 后更新为真实 fd) */
        __u64 vpid = ((__u64)~0U << 32) | (__u64)getpid();
        bpf_map_update_elem(bpf_map__fd(ctl), &k1, &vpid, BPF_ANY);
    }

    fprintf(stderr, "[tp] loaded, attached to sys_enter_read+sys_exit_read (PID=%d)\n", getpid());
    return obj;
}

/* ========== fentry/fexit 加载（内核 6.1+，trampoline）========== */
static void *load_fentry_bpf(void) {
    struct rlimit rl = {RLIM_INFINITY, RLIM_INFINITY};
    setrlimit(RLIMIT_MEMLOCK, &rl);

    struct bpf_object *obj = bpf_object__open(g_bpf_obj);
    if (!obj) { fprintf(stderr, "fentry: open failed\n"); return NULL; }

    /* 禁用 kprobe/kretprobe/tp 程序，只保留 fentry/fexit 的 autoload */
    { struct bpf_program *_p;
      _p = bpf_object__find_program_by_name(obj, "kp_recv_entry");
      if (_p) bpf_program__set_autoload(_p, false);
      _p = bpf_object__find_program_by_name(obj, "kp_recv_return");
      if (_p) bpf_program__set_autoload(_p, false);
      _p = bpf_object__find_program_by_name(obj, "tp_sys_enter_read");
      if (_p) bpf_program__set_autoload(_p, false);
      _p = bpf_object__find_program_by_name(obj, "tp_sys_exit_read");
      if (_p) bpf_program__set_autoload(_p, false); }

    if (bpf_object__load(obj) != 0) {
        fprintf(stderr, "fentry: load failed\n");
        bpf_object__close(obj);
        return NULL;
    }

    /* auto-attach fentry/fexit via trampoline */
    struct bpf_program *prog;
    struct bpf_link *le = NULL, *lx = NULL;

    prog = bpf_object__find_program_by_name(obj, "fentry_recv");
    if (prog && bpf_program__fd(prog) >= 0) {
        le = bpf_program__attach(prog);
        if (libbpf_get_error(le)) {
            fprintf(stderr, "fentry: attach fentry_recv failed: %ld\n", libbpf_get_error(le));
            le = NULL;
        }
    }
    prog = bpf_object__find_program_by_name(obj, "fexit_recv");
    if (prog && bpf_program__fd(prog) >= 0) {
        lx = bpf_program__attach(prog);
        if (libbpf_get_error(lx)) {
            fprintf(stderr, "fentry: attach fexit_recv failed: %ld\n", libbpf_get_error(lx));
            lx = NULL;
        }
    }

    if (!le || !lx) {
        fprintf(stderr, "fentry: attach failed\n");
        if (le) bpf_link__destroy(le);
        if (lx) bpf_link__destroy(lx);
        bpf_object__close(obj);
        return NULL;
    }

    /* 设置 PID 过滤 */
    struct bpf_map *ctl = bpf_object__find_map_by_name(obj, "ctl");
    if (ctl) {
        __u32 k0 = 0, k1 = 1;
        __u64 v1 = 1, vpid = (__u64)getpid();
        bpf_map_update_elem(bpf_map__fd(ctl), &k0, &v1, BPF_ANY);
        bpf_map_update_elem(bpf_map__fd(ctl), &k1, &vpid, BPF_ANY);
    }

    fprintf(stderr, "[fentry] loaded, attached fentry+fexit (PID=%d)\n", getpid());
    return obj;
}

/* ========== Master echo 线程 ========== */
typedef struct {
    int listen_fd;
    int client_fd;
    int slave_fd;         /* sync 模式用 */
    ringbuf_fwd_t *rf;    /* kprobe/tp 模式用 */
    int ctl_map_fd;       /* bp: tp 模式: ctl map fd，用于设置 CTL_MASTER_FD */
    volatile int running;
    volatile int ready;
    int echo_count;
} master_t;

static void *master_echo_sync(void *arg) {
    /* sync 模式: echo + 同步转发 slave */
    master_t *m = (master_t *)arg;

    m->ready = 1; /* ready before accept, so client can connect */
    m->client_fd = accept(m->listen_fd, NULL, NULL);
    if (m->client_fd < 0) { perror("master: accept"); return NULL; }
    set_nodelay(m->client_fd);

    char buf[65536];
    while (1) {
        ssize_t n = read(m->client_fd, buf, sizeof(buf));
        if (n <= 0) break;
        m->echo_count++;

        /* echo 响应客户端 */
        if (write_full(m->client_fd, buf, (size_t)n) < 0) break;

        /* 同步转发 slave — 在主路径上 */
        if (m->slave_fd >= 0) {
            if (write_full(m->slave_fd, buf, (size_t)n) < 0) break;
        }
    }

    close(m->client_fd);
    return NULL;
}

static void *master_echo_bpf(void *arg) {
    /* kprobe/tp 模式: echo only，转发由 BPF→ringbuf→异步线程做 */
    master_t *m = (master_t *)arg;

    m->ready = 1; /* ready before accept, so client can connect */
    m->client_fd = accept(m->listen_fd, NULL, NULL);
    if (m->client_fd < 0) { perror("master: accept"); return NULL; }
    set_nodelay(m->client_fd);

    /* tp 模式: 把 client_fd 编码进 CTL_PID 高32位 (低32位=pid, 高32位=fd) */
    if (m->ctl_map_fd >= 0) {
        __u32 k = CTL_PID;
        __u64 v = ((__u64)(unsigned long)m->client_fd << 32) | (__u64)getpid();
        bpf_map_update_elem(m->ctl_map_fd, &k, &v, BPF_ANY);
    }

    char buf[65536];
    while (1) {
        ssize_t n = read(m->client_fd, buf, sizeof(buf));
        if (n <= 0) break;
        m->echo_count++;

        /* echo 响应客户端（转发由 BPF→ringbuf→异步线程做） */
        if (write_full(m->client_fd, buf, (size_t)n) < 0) break;
    }

    close(m->client_fd);
    return NULL;
}

/* ========== QPS 客户端 ========== */
typedef struct {
    double qps;
    double avg_us;
    double p50_us;
    double p99_us;
    double min_us;
    double max_us;
    int completed;
} qps_result_t;

static qps_result_t run_client(void) {
    qps_result_t r;
    memset(&r, 0, sizeof(r));

    double *lats = calloc((size_t)g_req_count, sizeof(double));
    if (!lats) return r;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    set_nodelay(fd);

    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
                               .sin_port = htons(CLIENT_PORT)};

    for (int retry = 0; retry < 50; retry++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) break;
        if (errno == ECONNREFUSED) { usleep(50000); continue; }
        fprintf(stderr, "client: connect error %d: %s\n", errno, strerror(errno));
        close(fd); free(lats); return r;
    }
    fprintf(stderr, "[client] connected after retries\n");

    char *req = calloc(1, (size_t)g_payload_size);
    char *rsp = calloc(1, (size_t)g_payload_size);
    if (!req || !rsp) { close(fd); free(lats); free(req); free(rsp); return r; }
    memset(req, 'A', (size_t)g_payload_size);

    /* warmup */
    int warmup = g_req_count / 10;
    if (warmup < 100) warmup = 100;
    if (warmup > 500) warmup = 500;
    for (int i = 0; i < warmup; i++) {
        if (write_full(fd, req, (size_t)g_payload_size) < 0) {
            fprintf(stderr, "client: warmup write error at %d: %s\n", i, strerror(errno));
            goto done;
        }
        if (read_full(fd, rsp, (size_t)g_payload_size) != (ssize_t)g_payload_size) {
            fprintf(stderr, "client: warmup read error at %d: %s\n", i, strerror(errno));
            goto done;
        }
    }
    fprintf(stderr, "[client] warmup done\n");

    /* 测试 */
    fprintf(stderr, "[client] starting test loop, %d reqs\n", g_req_count);
    double t0 = now_us();
    int ok = 0;
    for (int i = 0; i < g_req_count; i++) {
        double lt0 = now_us();
        if (write_full(fd, req, (size_t)g_payload_size) < 0) {
            fprintf(stderr, "[client] test write err at %d: %s\n", i, strerror(errno));
            g_req_count = i; break;
        }
        if (read_full(fd, rsp, (size_t)g_payload_size) != (ssize_t)g_payload_size) {
            fprintf(stderr, "[client] test read err at %d: %s\n", i, strerror(errno));
            g_req_count = i; break;
        }
        lats[i] = now_us() - lt0;
        ok = i + 1;
    }
    double elapsed = now_us() - t0;
    fprintf(stderr, "[client] test done, ok=%d elapsed=%.0fus qps=%.0f\n",
            ok, elapsed, (double)ok / elapsed * 1000000.0);
    fflush(stderr);

    r.completed = g_req_count;
    if (elapsed <= 0) elapsed = 1.0;
    qsort(lats, (size_t)g_req_count, sizeof(double), cmp_double);
    r.qps    = (double)g_req_count / elapsed * 1000000.0;
    r.min_us = lats[0];
    r.max_us = lats[g_req_count - 1];
    r.p50_us = lats[g_req_count / 2];
    r.p99_us = lats[(int)(g_req_count * 0.99)];

    double sum = 0;
    for (int i = 0; i < g_req_count; i++) sum += lats[i];
    r.avg_us = sum / g_req_count;

done:
    close(fd);
    free(req); free(rsp); free(lats);
    return r;
}

/* ========== 打印 ========== */
static void print_row(const char *mode, const qps_result_t *r, int slave_msgs, int fwd_msgs) {
    fprintf(stderr, "RESULT %-10s  %-6d  %10.0f  %7s  %7s  %7s  %7s  %7s  %7d  %7d\n",
           mode, g_payload_size, r->qps,
           latency_str(r->avg_us), latency_str(r->p50_us), latency_str(r->p99_us),
           latency_str(r->min_us), latency_str(r->max_us),
           slave_msgs, fwd_msgs);
    fflush(stderr);
    printf("%-10s  %-6d  %10.0f  %7s  %7s  %7s  %7s  %7s  %7d  %7d\n",
           mode, g_payload_size, r->qps,
           latency_str(r->avg_us), latency_str(r->p50_us), latency_str(r->p99_us),
           latency_str(r->min_us), latency_str(r->max_us),
           slave_msgs, fwd_msgs);
    fflush(stdout);
    /* Also write result to file */
    FILE *rf = fopen("/tmp/perf_kprobe_result.txt", "w");
    if (rf) {
        fprintf(rf, "pid=%d %-10s  %-6d  %10.0f  %7s  %7s  %7s  %7s  %7s  %7d  %7d\n",
               getpid(), mode, g_payload_size, r->qps,
               latency_str(r->avg_us), latency_str(r->p50_us), latency_str(r->p99_us),
               latency_str(r->min_us), latency_str(r->max_us),
               slave_msgs, fwd_msgs);
        fclose(rf);
    }
}

static void print_header(void) {
    printf("%-10s  %-6s  %10s  %7s  %7s  %7s  %7s  %7s  %7s  %7s\n",
           "mode", "size", "qps", "avg", "p50", "p99", "min", "max",
           "slave", "fwd");
    printf("----------  ------  ----------  -------  -------  -------  -------  -------  -------  -------\n");
}

/* ========== run_one ========== */
static qps_result_t run_one_mode(const char *mode_name, const char *mode,
                                  slave_t *slave, int *out_slave_msgs, int *out_fwd_msgs) {
    *out_slave_msgs = 0;
    *out_fwd_msgs = 0;
    qps_result_t r;
    memset(&r, 0, sizeof(r));

    int use_kprobe = (strcmp(mode, "kprobe") == 0);
    int use_tp     = (strcmp(mode, "tp") == 0);
    int use_fentry = (strcmp(mode, "fentry") == 0);
    int use_sync   = (strcmp(mode, "sync") == 0);
    int use_bpf    = use_kprobe || use_tp || use_fentry;

    /* BPF */
    struct bpf_object *bpf_obj = NULL;
    ringbuf_fwd_t rf;
    pthread_t fwd_tid;
    memset(&rf, 0, sizeof(rf));

    if (use_kprobe) {
        bpf_obj = load_kprobe_bpf();
        if (!bpf_obj) { fprintf(stderr, "[%s] kprobe load failed, skip\n", mode_name); return r; }
    }
    if (use_tp) {
        bpf_obj = load_tp_bpf();
        if (!bpf_obj) { fprintf(stderr, "[%s] tp load failed, skip\n", mode_name); return r; }
    }
    if (use_fentry) {
        bpf_obj = load_fentry_bpf();
        if (!bpf_obj) {
            fprintf(stderr, "[%s] fentry load failed, falling back to kprobe\n", mode_name);
            bpf_obj = load_kprobe_bpf();
            use_kprobe = 1;
            use_fentry = 0;
            if (!bpf_obj) {
                fprintf(stderr, "[%s] kprobe fallback also failed, skip\n", mode_name);
                return r;
            }
        }
    }

    /* Master listen */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_ANY),
                               .sin_port = htons(CLIENT_PORT)};
    bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(listen_fd, 1);

    /* 连接 slave */
    int slave_fd = -1;
    if (use_sync || use_bpf) {
        slave_fd = socket(AF_INET, SOCK_STREAM, 0);
        set_nodelay(slave_fd);

        struct hostent *he = gethostbyname(g_slave_host);
        if (!he) {
            fprintf(stderr, "slave: gethostbyname(%s) failed\n", g_slave_host);
        } else {
            struct sockaddr_in sa = {.sin_family = AF_INET,
                                     .sin_port = htons(g_slave_port)};
            memcpy(&sa.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
            if (connect(slave_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
                perror("connect slave");
                close(slave_fd);
                slave_fd = -1;
            } else {
                /* 限制发送缓冲区，快速暴露慢消费导致的 write 阻塞 */
                int sndbuf = 262144; /* 256KB */
                setsockopt(slave_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
            }
        }
    }

    /* 启动 ringbuf 转发线程（kprobe/tp 模式） */
    if (use_bpf && slave_fd >= 0) {
        rf.obj = bpf_obj;
        rf.slave_fd = slave_fd;
        rf.running = 1;
        pthread_create(&fwd_tid, NULL, ringbuf_fwd_thread, &rf);
        while (!__atomic_load_n(&rf.ready, __ATOMIC_ACQUIRE)) usleep(1000);
    }

    /* 启动 master 线程 */
    master_t master;
    memset(&master, 0, sizeof(master));
    master.listen_fd = listen_fd;
    master.slave_fd = slave_fd;
    master.rf = &rf;
    master.ctl_map_fd = -1;
    if (use_tp && bpf_obj) {
        struct bpf_map *ctl = bpf_object__find_map_by_name(bpf_obj, "ctl");
        master.ctl_map_fd = ctl ? bpf_map__fd(ctl) : -1;
    }
    master.running = 1;

    pthread_t master_tid;
    if (use_bpf || !use_sync)
        pthread_create(&master_tid, NULL, master_echo_bpf, &master);
    else
        pthread_create(&master_tid, NULL, master_echo_sync, &master);
    while (!__atomic_load_n(&master.ready, __ATOMIC_ACQUIRE)) usleep(1000);

    printf("[%-7s] master ready, running client...\n", mode_name);
    fflush(stdout);

    /* 跑客户端 */
    r = run_client();

    /* 收尾 */
    shutdown(listen_fd, SHUT_RDWR);
    pthread_join(master_tid, NULL);

    if (use_bpf && rf.running) {
        rf.running = 0;
        if (slave_fd >= 0) shutdown(slave_fd, SHUT_WR);
        pthread_join(fwd_tid, NULL);
    }
    if (slave_fd >= 0) close(slave_fd);

    close(listen_fd);

    *out_slave_msgs = slave->msg_count;
    *out_fwd_msgs   = rf.fwd_count;

    /* BPF 模式: dump BPF stats 用于调试 */
    if ((use_tp || use_kprobe) && bpf_obj) {
        struct bpf_map *stats_map = bpf_object__find_map_by_name(bpf_obj, "stats");
        if (stats_map) {
            __u32 keys[] = {0, 1, 2, 3, 4, 5, 6};
            __u64 vals[7];
            for (int si = 0; si < 7; si++) {
                vals[si] = 0;
                bpf_map_lookup_elem(bpf_map__fd(stats_map), &keys[si], &vals[si]);
            }
            fprintf(stderr, "[bpf-stats] hit=%lu skip_pid=%lu skip_fd=%lu rb_err=%lu bytes=%lu msg_null=%lu\n",
                    (unsigned long)vals[0], (unsigned long)vals[2],
                    (unsigned long)vals[5], (unsigned long)vals[3],
                    (unsigned long)vals[4], (unsigned long)vals[6]);
        }
    }

    if (bpf_obj) bpf_object__close(bpf_obj);

    fprintf(stderr, "[run_one_mode] returning qps=%.0f completed=%d\n",
            r.qps, r.completed);
    fflush(stderr);

    return r;
}

/* ========== Main ========== */
static void usage(const char *prog) {
    fprintf(stderr,
        "用法: sudo %s [options]\n"
        "  --mode, -m    none | sync | kprobe | fentry | tp | all (默认: all)\n"
        "  --payload, -p 负载大小 (默认: 64)\n"
        "  --count, -c   请求数 (默认: 10000)\n"
        "  --bpf-obj, -b BPF 对象路径 (默认: ./kprobe_capture.bpf.o)\n"
        "  --help, -h    帮助\n", prog);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    const char *mode_str = "all";

    struct option long_opts[] = {
        {"mode", required_argument, 0, 'm'},
        {"payload", required_argument, 0, 'p'},
        {"count", required_argument, 0, 'c'},
        {"bpf-obj", required_argument, 0, 'b'},
        {"slave-host", required_argument, 0, 'H'},
        {"slave-port", required_argument, 0, 'P'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "m:p:c:b:H:P:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'm': mode_str = optarg; break;
        case 'p': g_payload_size = atoi(optarg); break;
        case 'c': g_req_count = atoi(optarg); break;
        case 'b': g_bpf_obj = optarg; break;
        case 'H': g_slave_host = optarg; break;
        case 'P': g_slave_port = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }

    printf("=== kprobe 主从转发 QPS 对比 ===\n");
    printf("payload=%d bytes, count=%d\n", g_payload_size, g_req_count);
    printf("模式: %s\n", mode_str);
    printf("none  = 纯echo不转发   sync = echo+同步转发   kprobe = echo+kprobe异步转发   fentry = echo+fentry异步转发   tp = echo+tracepoint异步转发\n\n");

    print_header();

    const char *modes[] = {"none", "sync", "kprobe", "fentry", "tp"};
    int do_all = (strcmp(mode_str, "all") == 0);

    for (int i = 0; i < 5; i++) {
        if (!do_all && strcmp(mode_str, modes[i]) != 0) continue;

        /* 启动 slave */
        slave_t slave;
        slave_start(&slave, SLAVE_PORT);

        int slave_msgs = 0, fwd_msgs = 0;
        qps_result_t r = run_one_mode(modes[i], modes[i], &slave, &slave_msgs, &fwd_msgs);
        print_row(modes[i], &r, slave_msgs, fwd_msgs);

        slave_stop(&slave);
        sleep(1); /* 等端口释放 */
    }

    printf("\n完成。\n");
    return 0;
}
