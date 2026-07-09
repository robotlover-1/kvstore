/*
 * test_ebpf_proxy_qps.c — eBPF proxy 独立进程架构的 QPS 对比测试
 *
 * 测试三种模式:
 *   none:  纯 echo，不转发（基准 QPS）
 *   sync:  echo + 同步转发 slave（转发在主请求路径上）
 *   ebpf:  echo only + ebpf-proxy（独立进程）kprobe 截获 → ringbuf → 异步转发 slave
 *
 * 架构（ebpf 模式）:
 *   本测试进程:
 *     - slave 线程 (TCP server, 接收转发数据并计数)
 *     - master echo 线程 (accept → read → echo)
 *     - QPS 客户端 (发送请求，测量 QPS)
 *   ebpf-proxy 子进程:
 *     - 加载 BPF kprobe/kretprobe on tcp_recvmsg
 *     - ringbuf poll → TCP send to slave
 *     - PID 过滤: 只截获 master echo 线程的 read() 调用
 *
 * 用法:
 *   sudo ./test_ebpf_proxy_qps --mode none    --payload 64 --count 50000
 *   sudo ./test_ebpf_proxy_qps --mode sync    --payload 64 --count 50000
 *   sudo ./test_ebpf_proxy_qps --mode ebpf    --payload 64 --count 50000
 *   sudo ./test_ebpf_proxy_qps --mode all     --payload 64 --count 50000
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"

/* ---- 配置 ---- */
#define MASTER_PORT   15900
#define SLAVE_PORT    15901
#define BPF_PIN_PATH  "/sys/fs/bpf/kvstore_repl_qps_test"
#define EBPF_PROXY_BIN "./build/ebpf_proxy"
#define CLIENT_CAPTURE_OBJ "build/replication/bpf/repl_client_capture.bpf.o"

static int g_payload_size = 64;
static int g_req_count    = 50000;
static const char *g_ebpf_proxy_bin = EBPF_PROXY_BIN;
static const char *g_client_capture_obj = CLIENT_CAPTURE_OBJ;
static const char *g_slave_host = "127.0.0.1";
static int g_no_local_slave = 0;

/* ---- Slave 进程（独立 PID，避免 BPF 反馈循环）----
 * Slave 必须在独立进程中运行，因为 BPF kprobe 按 PID 过滤。
 * 如果 slave 和 master 在同一进程，slave 的 read() 也会触发 kprobe，
 * 导致转发数据被重新捕获 → 无限放大。 */
typedef struct {
    int port;
    pid_t pid;
    int pipe_fd;  /* 父进程读端，接收 slave 统计 */
    int msg_count;
    long long total_bytes;
} slave_ctx_t;

static volatile sig_atomic_t g_slave_running = 1;
static void slave_sig_handler(int sig) { (void)sig; g_slave_running = 0; }

static void slave_process_main(int port, int pipe_wr) {
    /* 子进程：TCP server */
    signal(SIGTERM, slave_sig_handler);
    signal(SIGINT, slave_sig_handler);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("slave: socket"); _exit(1); }

    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
                               .sin_port = htons(port)};
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("slave: bind"); _exit(1);
    }
    listen(listen_fd, 1);

    /* 通知父进程已就绪 */
    char ready = 'R';
    if (write(pipe_wr, &ready, 1) != 1) { _exit(1); }
    fprintf(stderr, "[slave-%d] listening on port %d\n", getpid(), port);

    int msg_count = 0;
    long long total_bytes = 0;

    while (g_slave_running) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        set_nodelay(fd);
        fprintf(stderr, "[slave-%d] client connected fd=%d\n", getpid(), fd);

        char buf[65536];
        while (g_slave_running) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            msg_count++;
            total_bytes += n;
        }
        close(fd);
    }
    close(listen_fd);

    /* 写统计到管道 */
    fprintf(stderr, "[slave-%d] exiting, msgs=%d bytes=%lld\n",
            getpid(), msg_count, total_bytes);
    if (write(pipe_wr, &msg_count, sizeof(msg_count)) != sizeof(msg_count)) {}
    if (write(pipe_wr, &total_bytes, sizeof(total_bytes)) != sizeof(total_bytes)) {}
    close(pipe_wr);
    _exit(0);
}

static int slave_start(slave_ctx_t *s, int port) {
    memset(s, 0, sizeof(*s));
    s->port = port;

    int pipe_fds[2];
    if (pipe(pipe_fds) < 0) { perror("pipe"); return -1; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork slave"); close(pipe_fds[0]); close(pipe_fds[1]); return -1; }

    if (pid == 0) {
        /* 子进程 */
        close(pipe_fds[0]);  /* 关闭读端 */
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        slave_process_main(port, pipe_fds[1]);
        /* never returns */
    }

    /* 父进程 */
    close(pipe_fds[1]);  /* 关闭写端 */
    s->pid = pid;
    s->pipe_fd = pipe_fds[0];

    /* 等待子进程就绪信号 */
    char ready;
    if (read(pipe_fds[0], &ready, 1) != 1) {
        fprintf(stderr, "slave: failed to get ready signal\n");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(pipe_fds[0]);
        return -1;
    }
    return 0;
}

static void slave_stop(slave_ctx_t *s) {
    if (s->pid <= 0) return;

    /* 先发信号，再连接触发 accept/read 返回 */
    kill(s->pid, SIGTERM);
    usleep(50000);

    /* 连接触发阻塞的 accept */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
                               .sin_port = htons(s->port)};
    connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    close(fd);

    /* 等待子进程退出（最多 3 秒） */
    for (int i = 0; i < 30; i++) {
        if (waitpid(s->pid, NULL, WNOHANG) > 0) break;
        usleep(100000);
    }
    /* 强制 kill */
    if (waitpid(s->pid, NULL, WNOHANG) == 0) {
        kill(s->pid, SIGKILL);
        waitpid(s->pid, NULL, 0);
    }

    /* 读取统计 */
    int n;
    long long b;
    if (read(s->pipe_fd, &n, sizeof(n)) == sizeof(n))
        s->msg_count = n;
    if (read(s->pipe_fd, &b, sizeof(b)) == sizeof(b))
        s->total_bytes = b;
    close(s->pipe_fd);
    s->pid = 0;
}

/* ---- ebpf-proxy 子进程管理 ---- */
static pid_t g_proxy_pid = 0;

static int proxy_start(void) {
    /* 清理旧 pin 目录 */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s 2>/dev/null; mkdir -p %s",
             BPF_PIN_PATH, BPF_PIN_PATH);
    system(cmd);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork ebpf-proxy");
        return -1;
    }
    if (pid == 0) {
        /* child: exec ebpf-proxy */
        execl(g_ebpf_proxy_bin, g_ebpf_proxy_bin,
              "--pin-path", BPF_PIN_PATH,
              "--obj-path", g_client_capture_obj,
              NULL);
        perror("exec ebpf-proxy");
        _exit(1);
    }

    g_proxy_pid = pid;
    fprintf(stderr, "[test] ebpf-proxy PID=%d started\n", pid);

    /* 等待 ebpf-proxy 加载 BPF 并 pin maps */
    usleep(500000);
    return 0;
}

static void proxy_stop(void) {
    if (g_proxy_pid > 0) {
        kill(g_proxy_pid, SIGTERM);
        /* 等待最多 3 秒 */
        for (int i = 0; i < 30; i++) {
            if (waitpid(g_proxy_pid, NULL, WNOHANG) > 0) break;
            usleep(100000);
        }
        /* 如果还没退出，强制 kill */
        if (waitpid(g_proxy_pid, NULL, WNOHANG) == 0) {
            kill(g_proxy_pid, SIGKILL);
            waitpid(g_proxy_pid, NULL, 0);
        }
        g_proxy_pid = 0;
    }
    /* 清理 pin */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s 2>/dev/null", BPF_PIN_PATH);
    system(cmd);
}

/* 写 proxy_cfg map: master_pid, master_port, slave_addr, slave_port */
static int proxy_write_config(int master_pid, int slave_port) {
    char path[512];
    snprintf(path, sizeof(path), "%s/proxy_cfg", BPF_PIN_PATH);

    /* 等待 map 被 pin（ebpf-proxy 可能需要一点时间） */
    int fd = -1;
    for (int i = 0; i < 20; i++) {
        fd = bpf_obj_get(path);
        if (fd >= 0) break;
        usleep(100000);
    }
    if (fd < 0) {
        fprintf(stderr, "[test] proxy_cfg map not found at %s\n", path);
        return -1;
    }

    /* master_pid */
    char key[32] = "master_pid";
    __u64 val = (__u64)master_pid;
    bpf_map_update_elem(fd, key, &val, BPF_ANY);

    /* master_port */
    strcpy(key, "master_port");
    val = MASTER_PORT;
    bpf_map_update_elem(fd, key, &val, BPF_ANY);

    /* slave_addr: use inet_addr (returns network byte order) */
    strcpy(key, "slave_addr");
    val = (__u64)(unsigned int)inet_addr(g_slave_host);
    if (val == (__u64)(unsigned int)-1)
        val = (127U << 24) | 1U;  /* fallback: 127.0.0.1 */
    bpf_map_update_elem(fd, key, &val, BPF_ANY);

    /* slave_port */
    strcpy(key, "slave_port");
    val = (__u64)slave_port;
    bpf_map_update_elem(fd, key, &val, BPF_ANY);

    close(fd);
    fprintf(stderr, "[test] wrote proxy_cfg: master_pid=%d port=%d slave=%s:%d\n",
            master_pid, MASTER_PORT, g_slave_host, slave_port);
    return 0;
}

/* ---- Master echo 线程 ---- */
typedef struct {
    int port;
    int slave_fd;       /* sync 模式: 转发到此 fd */
    volatile int running;
    volatile int ready;
    int echo_count;
    int listen_fd;
    pthread_t thread;
} master_ctx_t;

static void *master_echo_sync(void *arg) {
    master_ctx_t *m = (master_ctx_t *)arg;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
                               .sin_port = htons(m->port)};
    bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(listen_fd, 1);

    m->listen_fd = listen_fd;
    m->ready = 1;

    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("master: accept");
        close(listen_fd);
        return NULL;
    }
    set_nodelay(client_fd);

    char buf[65536];
    while (1) {
        ssize_t n = read(client_fd, buf, sizeof(buf));
        if (n <= 0) break;
        m->echo_count++;

        /* echo */
        if (write_full(client_fd, buf, (size_t)n) < 0) break;

        /* sync 转发到 slave */
        if (m->slave_fd >= 0) {
            if (write_full(m->slave_fd, buf, (size_t)n) < 0) break;
        }
    }

    close(client_fd);
    close(listen_fd);
    return NULL;
}

static void *master_echo_none(void *arg) {
    master_ctx_t *m = (master_ctx_t *)arg;
    m->slave_fd = -1;  /* 确保不转发 */
    return master_echo_sync(m);
}

static int master_start(master_ctx_t *m, int port, int slave_fd,
                         void *(*thread_func)(void *)) {
    memset(m, 0, sizeof(*m));
    m->port = port;
    m->slave_fd = slave_fd;
    m->running = 1;
    if (pthread_create(&m->thread, NULL, thread_func, m) != 0) return -1;
    while (!__atomic_load_n(&m->ready, __ATOMIC_ACQUIRE)) usleep(1000);
    return 0;
}

static void master_stop(master_ctx_t *m) {
    m->running = 0;
    if (m->listen_fd > 0) shutdown(m->listen_fd, SHUT_RDWR);
    pthread_join(m->thread, NULL);
}

/* ---- QPS 客户端 ---- */
typedef struct {
    double qps;
    double avg_us;
    double p50_us;
    double p99_us;
    double min_us;
    double max_us;
    int completed;
} qps_result_t;

/* QPS 客户端 —— 可以在独立进程中运行（避免其 read() 触发 BPF kprobe） */
#define CLIENT_RESULT_FILE "/tmp/ebpf_qps_client_result.txt"

static void client_process_main(void) {
    /* 子进程：运行 QPS 客户端，结果写文件 */
    double *lats = calloc((size_t)g_req_count, sizeof(double));
    if (!lats) _exit(1);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    set_nodelay(fd);

    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
                               .sin_port = htons(MASTER_PORT)};

    for (int retry = 0; retry < 50; retry++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) break;
        if (errno == ECONNREFUSED) { usleep(50000); continue; }
        _exit(1);
    }
    fprintf(stderr, "[client-%d] connected\n", getpid());

    char *req = calloc(1, (size_t)g_payload_size);
    char *rsp = calloc(1, (size_t)g_payload_size);
    if (!req || !rsp) { close(fd); _exit(1); }
    memset(req, 'A', (size_t)g_payload_size);

    /* warmup */
    int warmup = g_req_count / 10;
    if (warmup < 100) warmup = 100;
    if (warmup > 500) warmup = 500;
    for (int i = 0; i < warmup; i++) {
        if (write_full(fd, req, (size_t)g_payload_size) < 0) goto done;
        if (read_full(fd, rsp, (size_t)g_payload_size) != (ssize_t)g_payload_size)
            goto done;
    }
    fprintf(stderr, "[client-%d] warmup done (%d)\n", getpid(), warmup);

    /* 测量 */
    double t0 = now_us();
    int ok = 0;
    for (int i = 0; i < g_req_count; i++) {
        double lt0 = now_us();
        if (write_full(fd, req, (size_t)g_payload_size) < 0) break;
        if (read_full(fd, rsp, (size_t)g_payload_size) != (ssize_t)g_payload_size)
            break;
        lats[i] = now_us() - lt0;
        ok = i + 1;
    }
    double elapsed = now_us() - t0;

    fprintf(stderr, "[client-%d] done: %d/%d elapsed=%.0fus qps=%.0f\n",
            getpid(), ok, g_req_count, elapsed,
            elapsed > 0 ? (double)ok / elapsed * 1000000.0 : 0);

    /* 写结果到文件 */
    FILE *rf = fopen(CLIENT_RESULT_FILE, "w");
    if (rf) {
        qsort(lats, (size_t)ok, sizeof(double), cmp_double);
        double sum = 0;
        for (int i = 0; i < ok; i++) sum += lats[i];
        fprintf(rf, "%.6f %.6f %.6f %.6f %.6f %.6f %d\n",
                ok > 0 ? (double)ok / elapsed * 1000000.0 : 0,
                ok > 0 ? sum / ok : 0,
                ok > 0 ? lats[ok / 2] : 0,
                ok > 0 ? lats[(int)(ok * 0.99)] : 0,
                ok > 0 ? lats[0] : 0,
                ok > 0 ? lats[ok - 1] : 0,
                ok);
        fclose(rf);
    }

done:
    close(fd);
    free(req); free(rsp); free(lats);
    _exit(0);
}

static qps_result_t run_qps_client(void) {
    qps_result_t r;
    memset(&r, 0, sizeof(r));

    unlink(CLIENT_RESULT_FILE);

    pid_t pid = fork();
    if (pid < 0) { perror("fork client"); return r; }
    if (pid == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        client_process_main();
        /* never returns */
    }

    /* 等待子进程 */
    waitpid(pid, NULL, 0);

    /* 读结果 */
    FILE *rf = fopen(CLIENT_RESULT_FILE, "r");
    if (rf) {
        fscanf(rf, "%lf %lf %lf %lf %lf %lf %d",
               &r.qps, &r.avg_us, &r.p50_us, &r.p99_us,
               &r.min_us, &r.max_us, &r.completed);
        fclose(rf);
        unlink(CLIENT_RESULT_FILE);
    }
    return r;
}

/* ---- 结果打印 ---- */
static void print_header(void) {
    printf("%-10s  %-6s  %10s  %7s  %7s  %7s  %7s  %7s\n",
           "mode", "size", "qps", "avg", "p50", "p99", "min", "max");
    printf("----------  ------  ----------  -------  -------  -------  -------  -------\n");
}

static void print_result(const char *mode, int payload, const qps_result_t *r) {
    fprintf(stderr, "RESULT %-10s  %-6d  %10.0f  %7s  %7s  %7s  %7s  %7s\n",
           mode, payload, r->qps,
           latency_str(r->avg_us), latency_str(r->p50_us), latency_str(r->p99_us),
           latency_str(r->min_us), latency_str(r->max_us));
    printf("%-10s  %-6d  %10.0f  %7s  %7s  %7s  %7s  %7s\n",
           mode, payload, r->qps,
           latency_str(r->avg_us), latency_str(r->p50_us), latency_str(r->p99_us),
           latency_str(r->min_us), latency_str(r->max_us));
    fflush(stdout);
}

/* ---- 单模式运行 ---- */
static qps_result_t run_one_mode(const char *mode_name, const char *mode,
                                  slave_ctx_t *slave, int *out_slave_msgs) {
    *out_slave_msgs = 0;
    qps_result_t r;
    memset(&r, 0, sizeof(r));

    int use_sync = (strcmp(mode, "sync") == 0);
    int use_ebpf = (strcmp(mode, "ebpf") == 0);

    /* — 连接 slave（sync 模式需要）— */
    int slave_fd = -1;
    if (use_sync) {
        slave_fd = socket(AF_INET, SOCK_STREAM, 0);
        set_nodelay(slave_fd);
        struct sockaddr_in sa = {.sin_family = AF_INET,
                                 .sin_port = htons(SLAVE_PORT)};
        if (inet_pton(AF_INET, g_slave_host, &sa.sin_addr) != 1) {
            sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        }
        if (connect(slave_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            perror("connect slave");
            close(slave_fd);
            slave_fd = -1;
        } else {
            int snd = 262144;
            setsockopt(slave_fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
        }
    }

    /* — ebpf 模式: 启动 ebpf-proxy 子进程 — */
    if (use_ebpf) {
        if (proxy_start() != 0) {
            fprintf(stderr, "[%s] ebpf-proxy start failed\n", mode_name);
            if (slave_fd >= 0) close(slave_fd);
            return r;
        }
        /* 写配置给 ebpf-proxy */
        if (proxy_write_config(getpid(), SLAVE_PORT) != 0) {
            fprintf(stderr, "[%s] proxy_write_config failed\n", mode_name);
            proxy_stop();
            if (slave_fd >= 0) close(slave_fd);
            return r;
        }
        /* 等待 ebpf-proxy 连接 slave */
        usleep(500000);
    }

    /* — 启动 master echo — */
    master_ctx_t master;
    void *(*thread_func)(void *) = use_sync ? master_echo_sync : master_echo_none;
    if (master_start(&master, MASTER_PORT, slave_fd, thread_func) != 0) {
        fprintf(stderr, "[%s] master start failed\n", mode_name);
        if (use_ebpf) proxy_stop();
        if (slave_fd >= 0) close(slave_fd);
        return r;
    }

    /* — 运行客户端（fork 独立进程，避免 client 的 read() 被 BPF 误捕获）— */
    printf("[%-7s] master ready, running client...\n", mode_name);
    fflush(stdout);

    /* pipe 传回 QPS 结果 */
    int qps_pipe[2];
    if (pipe(qps_pipe) < 0) { perror("qps pipe"); master_stop(&master); if (slave_fd >= 0) close(slave_fd); if (use_ebpf) proxy_stop(); return r; }

    pid_t client_pid = fork();
    if (client_pid < 0) {
        perror("fork client");
        master_stop(&master);
        if (slave_fd >= 0) close(slave_fd);
        if (use_ebpf) proxy_stop();
        close(qps_pipe[0]); close(qps_pipe[1]);
        return r;
    }

    if (client_pid == 0) {
        /* 子进程: 运行 QPS 客户端 */
        close(qps_pipe[0]);  /* 关闭读端 */
        r = run_qps_client();
        /* 通过管道传回结果 */
        write(qps_pipe[1], &r, sizeof(r));
        close(qps_pipe[1]);
        _exit(0);
    }

    /* 父进程: 等待客户端完成 */
    close(qps_pipe[1]);  /* 关闭写端 */
    read(qps_pipe[0], &r, sizeof(r));
    close(qps_pipe[0]);
    waitpid(client_pid, NULL, 0);

    /* — 清理 — */
    /* 先停 master：不再有新的 read() → ringbuf 不再有新数据 */
    master_stop(&master);
    if (slave_fd >= 0) close(slave_fd);

    if (use_ebpf) {
        /* master 已停，不再有新 read()。等 ebpf-proxy drain ringbuf：
         * 先不休 proxy，让它继续 poll + forward，直到 slave 收齐数据。 */
        int echo_count = master.echo_count;
        fprintf(stderr, "[ebpf] master echoed %d, waiting for slave to catch up...\n",
                echo_count);

        /* 本地 slave: 轮询 pipe 中的 msg_count。远程 slave: 走外部脚本 poll。 */
        if (!g_no_local_slave) {
            /* 本地 slave 在子进程中，无法直接读计数。
             * 等足够时间让 ringbuf drain（poll 间隔 100ms × 20 = 2s 上限） */
            usleep(1000000);
        }
        /* 远程 slave: 外部脚本负责 poll，这里 sleep 等其收齐 */
        if (g_no_local_slave) {
            usleep(2000000);  /* 跨机: 多等一会 */
        }

        /* 再停 ebpf-proxy（cleanup 会 drain 剩余 ringbuf + flush 缓存 + 转发到 slave） */
        proxy_stop();
    }

    /* 注意：slave 统计在 slave_stop() 中读取（main 中调用），此处不读 */
    (void)out_slave_msgs;

    return r;
}

/* ---- 用法 ---- */
static void usage(const char *prog) {
    fprintf(stderr,
        "用法: sudo %s [options]\n"
        "  --mode, -m    none | sync | ebpf | all (默认: all)\n"
        "  --payload, -p 负载大小 (默认: 64)\n"
        "  --count, -c   请求数 (默认: 50000)\n"
        "  --proxy-bin   ebpf-proxy 路径 (默认: ./build/ebpf_proxy)\n"
        "  --bpf-obj     client_capture BPF 路径 (默认: build/replication/bpf/repl_client_capture.bpf.o)\n"
        "  --help, -h    帮助\n", prog);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    const char *mode_str = "all";

    struct option long_opts[] = {
        {"mode", required_argument, 0, 'm'},
        {"payload", required_argument, 0, 'p'},
        {"count", required_argument, 0, 'c'},
        {"proxy-bin", required_argument, 0, 1000},
        {"bpf-obj", required_argument, 0, 1001},
        {"slave-host", required_argument, 0, 1002},
        {"no-local-slave", no_argument, 0, 1003},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "m:p:c:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'm': mode_str = optarg; break;
        case 'p': g_payload_size = atoi(optarg); break;
        case 'c': g_req_count = atoi(optarg); break;
        case 1000: g_ebpf_proxy_bin = optarg; break;
        case 1001: g_client_capture_obj = optarg; break;
        case 1002: g_slave_host = optarg; break;
        case 1003: g_no_local_slave = 1; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }

    printf("=== eBPF proxy 独立进程架构 QPS 对比 ===\n");
    printf("payload=%d bytes, count=%d\n", g_payload_size, g_req_count);
    printf("ebpf-proxy: %s\n", g_ebpf_proxy_bin);
    printf("BPF obj: %s\n\n", g_client_capture_obj);

    print_header();

    const char *modes[] = {"none", "sync", "ebpf"};
    int do_all = (strcmp(mode_str, "all") == 0);

    int n_modes = (int)(sizeof(modes) / sizeof(modes[0]));
    for (int i = 0; i < n_modes; i++) {
        if (!do_all && strcmp(mode_str, modes[i]) != 0) continue;

        /* 启动 slave（本地或跳过由外部管理） */
        slave_ctx_t slave;
        memset(&slave, 0, sizeof(slave));
        if (!g_no_local_slave)
            slave_start(&slave, SLAVE_PORT);

        int slave_msgs = 0;
        qps_result_t r = run_one_mode(modes[i], modes[i], &slave, &slave_msgs);
        print_result(modes[i], g_payload_size, &r);

        if (!g_no_local_slave) {
            slave_stop(&slave);
            fprintf(stderr, "[%-7s] slave final: %d msgs, %lld bytes\n",
                    modes[i], slave.msg_count, slave.total_bytes);
        }
        sleep(1);
    }

    printf("\n完成。\n");
    return 0;
}
