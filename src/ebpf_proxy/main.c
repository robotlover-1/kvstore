#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "proxy_cache.h"
#include "proxy_slave.h"

/* ---- 状态 ---- */
typedef enum {
    STATE_FORWARDING = 0,
    STATE_BUFFERING  = 1,
} proxy_state_t;

static volatile int g_shutdown = 0;
static proxy_state_t g_state = STATE_FORWARDING;
static proxy_slave_ctx_t g_slave;
static cache_ctx_t g_cache;

/* BPF map fds */
static int g_client_ctl_fd = -1;
static int g_proxy_cfg_fd = -1;
static int g_client_stats_fd = -1;

/* ringbuf */
static struct bpf_object *g_bpf_obj = NULL;
static struct ring_buffer *g_rb = NULL;

/* kprobe links */
static struct bpf_link *g_kprobe_link = NULL;
static struct bpf_link *g_kretprobe_link = NULL;

/* config */
static char g_pin_path[256] = "/sys/fs/bpf/kvstore_repl_sockmap";
static char g_obj_path[256] = "build/replication/bpf/repl_client_capture.bpf.o";
static int g_master_pid = 0;
static int g_master_port = 0;
static unsigned int g_slave_ip = 0;
static int g_slave_port = 0;

/* ---- 信号处理 ---- */
static void signal_handler(int sig) {
    (void)sig;
    g_shutdown = 1;
}

/* 完整发送，处理 EINTR 和 partial send。
 * 正常路径返回 0，失败返回 -1。 */
static int proxy_send_full(int fd, const unsigned char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

/* 快速判断 payload 是否为复制控制命令（REPLSYNC/REPLACK/REPLDONE）。
 * 正常 RESP 命令以 '*'/'$'/':'/'+'/'-' 开头，首字节快速拒绝。 */
static int is_repl_control_payload(const unsigned char *payload, size_t plen) {
    if (plen < 7 || payload[0] != 'R')
        return 0;

    if (plen >= 8 && memcmp(payload, "REPLSYNC", 8) == 0)
        return 1;
    if (memcmp(payload, "REPLACK", 7) == 0)
        return 1;
    if (plen >= 8 && memcmp(payload, "REPLDONE", 8) == 0)
        return 1;

    return 0;
}

/* ---- 辅助函数 ---- */
static int open_pinned_map(const char *name, int *fd_out) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_pin_path, name);
    *fd_out = bpf_obj_get(path);
    return *fd_out >= 0 ? 0 : -1;
}

static int read_client_ctl_u64(__u32 key, __u64 *val) {
    if (g_client_ctl_fd < 0) return -1;
    return bpf_map_lookup_elem(g_client_ctl_fd, &key, val);
}

/* 从 proxy_cfg hash map 读 u64 值 */
static int read_proxy_cfg_u64(const char *key_str, __u64 *val) {
    char key[32] = {0};
    if (g_proxy_cfg_fd < 0) return -1;
    snprintf(key, sizeof(key), "%s", key_str);
    return bpf_map_lookup_elem(g_proxy_cfg_fd, key, val);
}

/* 写 client_ctl */
static int write_client_ctl_u64(__u32 key, __u64 val) {
    if (g_client_ctl_fd < 0) return -1;
    return bpf_map_update_elem(g_client_ctl_fd, &key, &val, BPF_ANY);
}

/* 轮询等待 proxy_cfg["master_pid"] 非零 */
static int wait_for_master_pid(int timeout_ms) {
    __u64 val = 0;
    int waited = 0;
    while (waited < timeout_ms) {
        if (read_proxy_cfg_u64("master_pid", &val) == 0 && val != 0) {
            g_master_pid = (int)val;
            return 0;
        }
        usleep(500000); /* 500ms */
        waited += 500;
    }
    return -1;
}

/* 从 proxy_cfg 读取 slave 地址 */
static int read_slave_addr(void) {
    __u64 addr = 0, port = 0;
    if (read_proxy_cfg_u64("slave_addr", &addr) != 0) return -1;
    if (read_proxy_cfg_u64("slave_port", &port) != 0) return -1;
    g_slave_ip = (unsigned int)addr;
    g_slave_port = (int)port;
    return (g_slave_ip != 0 && g_slave_port > 0) ? 0 : -1;
}

/* ---- ringbuf 回调 ---- */
static int ringbuf_callback(void *ctx, void *data, size_t len) {
    (void)ctx;
    if (len < 4) return 0;

    __u32 payload_len;
    memcpy(&payload_len, data, 4);
    unsigned char *payload = (unsigned char *)data + 4;
    size_t plen = (size_t)payload_len;

    if (payload_len == 0xFFFFFFFF) {
        /* magic flush signal */
        fprintf(stderr, "ebpf-proxy: REPLDONE detected, flushing cache...\n");
        int sent = cache_flush(&g_cache, proxy_slave_fd(&g_slave));
        if (sent >= 0) {
            g_state = STATE_FORWARDING;
            fprintf(stderr, "ebpf-proxy: cache flushed (%d items), "
                    "state=FORWARDING\n", sent);
        }
        return 0;
    }

    /* 过滤 slave→master 复制控制命令，防止回传 slave */
    if (is_repl_control_payload(payload, plen)) {
        return 0;
    }

    if (g_state == STATE_FORWARDING) {
        if (proxy_slave_is_connected(&g_slave)) {
            if (proxy_send_full(proxy_slave_fd(&g_slave), payload, plen) != 0) {
                cache_append(&g_cache, payload, plen);
            }
        } else {
            cache_append(&g_cache, payload, plen);
        }
    } else {
        cache_append(&g_cache, payload, plen);
    }
    return 0;
}

/* 主循环 */
static void main_loop(void) {
    while (!g_shutdown) {
        int rc = ring_buffer__poll(g_rb, 5 /* ms */);
        if (rc < 0 && !g_shutdown) {
            fprintf(stderr, "ebpf-proxy: ring_buffer__poll error: %d\n", rc);
            break;
        }

        /* 检查 fullsync 状态变化
         * client_ctl[3] 由 master 进程在 queue_snapshot/REPLDONE 时写入:
         *   1 = 全量同步开始 → 切 BUFFERING
         *   0 = 全量同步结束 → flush 缓存 → 切 FORWARDING */
        __u64 fs_val = 0;
        if (read_client_ctl_u64(3, &fs_val) == 0) {
            if (fs_val == 1 && g_state == STATE_FORWARDING) {
                g_state = STATE_BUFFERING;
                fprintf(stderr, "ebpf-proxy: fullsync start (client_ctl[3]=1), "
                        "state=BUFFERING\n");
            } else if (fs_val == 0 && g_state == STATE_BUFFERING) {
                fprintf(stderr, "ebpf-proxy: fullsync end (client_ctl[3]=0), "
                        "flushing cache...\n");
                if (proxy_slave_is_connected(&g_slave)) {
                    int sent = cache_flush(&g_cache, proxy_slave_fd(&g_slave));
                    fprintf(stderr, "ebpf-proxy: cache flushed (%d items), "
                            "state=FORWARDING\n", sent);
                } else {
                    fprintf(stderr, "ebpf-proxy: slave not connected, "
                            "deferring cache flush\n");
                }
                g_state = STATE_FORWARDING;
            }
        }

        /* 检查 slave 连接 */
        if (!proxy_slave_is_connected(&g_slave)) {
            if (read_slave_addr() == 0) {
                char host[64];
                snprintf(host, sizeof(host), "%u.%u.%u.%u",
                         g_slave_ip & 0xFF, (g_slave_ip >> 8) & 0xFF,
                         (g_slave_ip >> 16) & 0xFF, (g_slave_ip >> 24) & 0xFF);
                proxy_slave_init(&g_slave, host, g_slave_port);

                /* 指数退避 */
                for (int attempt = 1; !g_shutdown; attempt++) {
                    if (proxy_slave_connect(&g_slave) == 0) break;
                    unsigned int delay = g_slave.backoff_ms;
                    if (delay < PROXY_SLAVE_BACKOFF_INIT_MS)
                        delay = PROXY_SLAVE_BACKOFF_INIT_MS;
                    if (delay > 5000) delay = 5000;
                    fprintf(stderr, "ebpf-proxy: reconnect attempt %d, "
                            "sleeping %ums\n", attempt, delay);
                    /* 在退避期间继续 poll ringbuf，防止 ringbuf 满丢数据 */
                    int poll_iters = (int)(delay / 5);
                    for (int i = 0; i < poll_iters && !g_shutdown; i++) {
                        ring_buffer__poll(g_rb, 5);
                    }
                    g_slave.backoff_ms *= 2;
                    if (g_slave.backoff_ms > g_slave.backoff_max_ms)
                        g_slave.backoff_ms = g_slave.backoff_max_ms;
                }
            }
        }

        /* 如果 slave 在线且是 FORWARDING 且有缓存数据，尝试 flush */
        if (g_state == STATE_FORWARDING &&
            proxy_slave_is_connected(&g_slave) &&
            g_cache.head) {
            cache_flush(&g_cache, proxy_slave_fd(&g_slave));
        }
    }
}

/* 退出清理 */
static void cleanup(void) {
    fprintf(stderr, "ebpf-proxy: shutting down...\n");

    /* 1. detach kprobes — 停止新数据进入 ringbuf */
    if (g_kprobe_link) { bpf_link__destroy(g_kprobe_link); g_kprobe_link = NULL; }
    if (g_kretprobe_link) { bpf_link__destroy(g_kretprobe_link); g_kretprobe_link = NULL; }

    /* 2. drain ringbuf — 消费所有剩余条目再释放 */
    if (g_rb) {
        int drained = 0;
        for (int i = 0; i < 50; i++) {  /* 最多 5s (50 × 100ms) */
            int n = ring_buffer__poll(g_rb, 100);
            if (n <= 0) break;
            drained += n;
        }
        if (drained > 0)
            fprintf(stderr, "ebpf-proxy: drained %d remaining ringbuf entries\n", drained);
    }

    /* 3. flush 剩余缓存 */
    if (g_cache.head && proxy_slave_is_connected(&g_slave)) {
        fprintf(stderr, "ebpf-proxy: flushing remaining cache...\n");
        cache_flush(&g_cache, proxy_slave_fd(&g_slave));
    }
    cache_destroy(&g_cache);

    /* 4. 断开 slave */
    proxy_slave_disconnect(&g_slave);

    /* 5. 释放 BPF 资源 */
    if (g_rb) { ring_buffer__free(g_rb); g_rb = NULL; }
    if (g_bpf_obj) { bpf_object__close(g_bpf_obj); g_bpf_obj = NULL; }
    if (g_client_ctl_fd >= 0) { close(g_client_ctl_fd); g_client_ctl_fd = -1; }
    if (g_proxy_cfg_fd >= 0) { close(g_proxy_cfg_fd); g_proxy_cfg_fd = -1; }
    if (g_client_stats_fd >= 0) { close(g_client_stats_fd); g_client_stats_fd = -1; }

    fprintf(stderr, "ebpf-proxy: cleanup complete\n");
}

/* 加载并 attach BPF */
static int load_and_attach_bpf(void) {
    struct bpf_program *prog;

    g_bpf_obj = bpf_object__open_file(g_obj_path, NULL);
    if (libbpf_get_error(g_bpf_obj)) {
        fprintf(stderr, "ebpf-proxy: bpf_object__open_file failed\n");
        return -1;
    }

    if (bpf_object__load(g_bpf_obj) != 0) {
        fprintf(stderr, "ebpf-proxy: bpf_object__load failed\n");
        return -1;
    }

    /* pin maps */
    struct bpf_map *map;
    bpf_object__for_each_map(map, g_bpf_obj) {
        const char *name = bpf_map__name(map);
        if (!name) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", g_pin_path, name);
        unlink(path);
        int rc = bpf_map__pin(map, path);
        if (rc != 0) {
            fprintf(stderr, "ebpf-proxy: pin map '%s' to %s failed: %s\n",
                    name, path, strerror(errno));
        } else {
            fprintf(stderr, "ebpf-proxy: pinned map '%s' to %s\n", name, path);
        }
    }

    /* 打开自己的 pinned maps */
    if (open_pinned_map("client_ctl", &g_client_ctl_fd) != 0)
        fprintf(stderr, "ebpf-proxy: open client_ctl failed\n");
    if (open_pinned_map("proxy_cfg", &g_proxy_cfg_fd) != 0)
        fprintf(stderr, "ebpf-proxy: open proxy_cfg failed\n");
    if (open_pinned_map("client_stats", &g_client_stats_fd) != 0)
        fprintf(stderr, "ebpf-proxy: open client_stats failed\n");

    /* attach kprobe entry */
    prog = bpf_object__find_program_by_name(g_bpf_obj, "kprobe_client_recv_entry");
    if (!prog) { fprintf(stderr, "ebpf-proxy: find kprobe entry failed\n"); return -1; }
    g_kprobe_link = bpf_program__attach(prog);
    if (libbpf_get_error(g_kprobe_link)) {
        fprintf(stderr, "ebpf-proxy: attach kprobe entry failed\n");
        return -1;
    }

    /* attach kretprobe return */
    prog = bpf_object__find_program_by_name(g_bpf_obj, "kprobe_client_recv_return");
    if (!prog) { fprintf(stderr, "ebpf-proxy: find kretprobe return failed\n"); return -1; }
    g_kretprobe_link = bpf_program__attach(prog);
    if (libbpf_get_error(g_kretprobe_link)) {
        fprintf(stderr, "ebpf-proxy: attach kretprobe return failed\n");
        return -1;
    }

    /* 创建 ringbuf reader */
    struct bpf_map *rb_map = bpf_object__find_map_by_name(g_bpf_obj, "client_cache_ringbuf");
    if (!rb_map) { fprintf(stderr, "ebpf-proxy: find ringbuf map failed\n"); return -1; }
    g_rb = ring_buffer__new(bpf_map__fd(rb_map), ringbuf_callback, NULL, NULL);
    if (!g_rb) { fprintf(stderr, "ebpf-proxy: ring_buffer__new failed\n"); return -1; }

    fprintf(stderr, "ebpf-proxy: BPF loaded, kprobes attached\n");
    return 0;
}

/* 打印使用说明 */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --pin-path PATH   BPF map pin path (default: /sys/fs/bpf/kvstore_repl_sockmap)\n"
        "  --obj-path PATH   BPF object path (default: build/replication/bpf/repl_client_capture.bpf.o)\n"
        "  --help            Show this help\n",
        prog);
}

/* 解析命令行 */
static int parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]); return 1;
        } else if (!strcmp(argv[i], "--pin-path") && i + 1 < argc) {
            snprintf(g_pin_path, sizeof(g_pin_path), "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--obj-path") && i + 1 < argc) {
            snprintf(g_obj_path, sizeof(g_obj_path), "%s", argv[++i]);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]); return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    int rc = parse_args(argc, argv);
    if (rc != 0) return rc;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    /* 提高 memlock 限制 */
    struct rlimit rlim = { RLIM_INFINITY, RLIM_INFINITY };
    setrlimit(RLIMIT_MEMLOCK, &rlim);

    cache_init(&g_cache);

    /* 确保 pin 目录存在 */
    mkdir(g_pin_path, 0755);

    /* 加载 BPF 并 pin maps */
    if (load_and_attach_bpf() != 0) {
        fprintf(stderr, "ebpf-proxy: BPF init failed\n");
        return 1;
    }

    /* 等待 master 写配置（持续重试，master 可能后启动） */
    fprintf(stderr, "ebpf-proxy: waiting for master config...\n");
    if (wait_for_master_pid(300000) != 0) {
        fprintf(stderr, "ebpf-proxy: timeout waiting for master pid\n");
        cleanup();
        return 1;
    }

    /* 读 master port */
    __u64 port_val = 0;
    if (read_proxy_cfg_u64("master_port", &port_val) == 0) {
        g_master_port = (int)port_val;
    }

    /* 写 client_ctl（BPF kprobe 需要） */
    write_client_ctl_u64(1, (__u64)g_master_pid);
    write_client_ctl_u64(2, (__u64)g_master_port);

    fprintf(stderr, "ebpf-proxy: master pid=%d port=%d\n",
            g_master_pid, g_master_port);

    /* 尝试连接 slave */
    if (read_slave_addr() == 0) {
        char host[64];
        snprintf(host, sizeof(host), "%u.%u.%u.%u",
                 g_slave_ip & 0xFF, (g_slave_ip >> 8) & 0xFF,
                 (g_slave_ip >> 16) & 0xFF, (g_slave_ip >> 24) & 0xFF);
        proxy_slave_init(&g_slave, host, g_slave_port);
        proxy_slave_connect(&g_slave);
    }

    fprintf(stderr, "ebpf-proxy: entering main loop, state=FORWARDING\n");
    main_loop();
    cleanup();
    return 0;
}
