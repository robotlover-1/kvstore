/*
 * verify_bpf_read_user.c — 最小验证：bpf_probe_read_user 在 kretprobe
 * 中读取 TCP 数据是否正确。
 *
 * 用法:
 *   make tests/verify_bpf_read_user
 *   sudo ./tests/verify_bpf_read_user
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define VERIFY_PORT 15901
#define BPF_OBJ     "build/replication/bpf/repl_client_capture.bpf.o"

/* BPF ctl map key */
#define CTL_PID                 1
#define CTL_LISTEN_PORT         2
#define CTL_FULLSYNC_IN_PROGRESS 3

/* BPF stats map key */
#define STAT_HIT        0
#define STAT_READ_ERR   4
#define STAT_RB_ERR     2

/* ─── ringbuf context ─── */
struct rb_ctx {
    volatile int done;
    int capture_count;
    int mismatch_count;
    const unsigned char *expect;
    size_t expect_len;
};

static int ringbuf_cb(void *ctx, void *data, size_t size)
{
    struct rb_ctx *rc = (struct rb_ctx *)ctx;
    if (size < 4) return 0;

    __u32 plen;
    memcpy(&plen, data, 4);
    if (plen == 0 || plen + 4 > size) return 0;

    unsigned char *payload = (unsigned char *)data + 4;
    rc->capture_count++;

    if (rc->expect && rc->expect_len > 0) {
        if (plen == rc->expect_len && memcmp(payload, rc->expect, plen) == 0) {
            /* match */
        } else if (plen != rc->expect_len) {
            /* TCP segmentation — not a bpf_probe_read_user error */
        } else {
            rc->mismatch_count++;
            fprintf(stderr, "MISMATCH: expected %zu bytes, got %u\n",
                    rc->expect_len, plen);
            for (size_t i = 0; i < (plen < 64 ? plen : 64); i++)
                fprintf(stderr, "%02x ", payload[i]);
            fprintf(stderr, "\n");
        }
    }

    if (rc->capture_count >= 100) rc->done = 1;
    return 0;
}

int main(void)
{
    int ret = 1;
    struct bpf_object *obj = NULL;
    struct bpf_link *lk_kp = NULL, *lk_krp = NULL;
    struct ring_buffer *rb_mgr = NULL;
    int listen_fd = -1, client_fd = -1, srv_fd = -1;
    int ctl_fd = -1, stats_fd = -1, rb_fd = -1;

    printf("=== bpf_probe_read_user integrity check ===\n");
    fflush(stdout);

    /* 1. Open and load BPF */
    obj = bpf_object__open(BPF_OBJ);
    if (!obj) { perror("bpf_object__open"); goto out; }

    if (bpf_object__load(obj) != 0) {
        fprintf(stderr, "bpf_object__load: %s\n", strerror(errno));
        goto out;
    }
    printf("[OK] BPF loaded\n");

    /* 2. Setup ctl map */
    ctl_fd = bpf_object__find_map_fd_by_name(obj, "client_ctl");
    if (ctl_fd < 0) { perror("client_ctl map"); goto out; }

    __u64 pid_val = (__u64)(unsigned int)getpid();
    __u64 port_val = (__u64)VERIFY_PORT;
    __u64 zero = 0;
    bpf_map_update_elem(ctl_fd, &(int){CTL_PID}, &pid_val, BPF_ANY);
    bpf_map_update_elem(ctl_fd, &(int){CTL_LISTEN_PORT}, &port_val, BPF_ANY);
    bpf_map_update_elem(ctl_fd, &(int){CTL_FULLSYNC_IN_PROGRESS}, &zero, BPF_ANY);
    printf("[OK] ctl map set: PID=%llu PORT=%llu\n",
           (unsigned long long)pid_val, (unsigned long long)port_val);

    /* 3. Attach kprobes */
    struct bpf_program *prog_kp = bpf_object__find_program_by_name(
        obj, "kprobe_client_recv_entry");
    struct bpf_program *prog_krp = bpf_object__find_program_by_name(
        obj, "kprobe_client_recv_return");
    if (!prog_kp || !prog_krp) {
        fprintf(stderr, "program not found\n"); goto out;
    }
    lk_kp = bpf_program__attach(prog_kp);
    lk_krp = bpf_program__attach(prog_krp);
    if (!lk_kp || !lk_krp) {
        fprintf(stderr, "kprobe attach failed (need root)\n"); goto out;
    }
    printf("[OK] kprobe + kretprobe attached\n");

    /* 4. Setup ringbuf */
    rb_fd = bpf_object__find_map_fd_by_name(obj, "client_cache_ringbuf");
    if (rb_fd < 0) { perror("ringbuf map"); goto out; }

    struct rb_ctx rc = {0};
    rb_mgr = ring_buffer__new(rb_fd, ringbuf_cb, &rc, NULL);
    if (!rb_mgr) { perror("ring_buffer__new"); goto out; }
    printf("[OK] ringbuf ready\n");

    /* 5. Create TCP server */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); goto out; }
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons(VERIFY_PORT)
    };
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); goto out;
    }
    if (listen(listen_fd, 1) < 0) { perror("listen"); goto out; }
    printf("[OK] TCP server on port %d\n", VERIFY_PORT);

    /* 6. Connect client */
    client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd < 0) { perror("client socket"); goto out; }
    if (connect(client_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); goto out;
    }
    /* accept */
    srv_fd = accept(listen_fd, NULL, NULL);
    if (srv_fd < 0) { perror("accept"); goto out; }
    printf("[OK] TCP connected\n");

    /* 7. Send + verify multiple patterns */
    static const char *patterns[] = {
        "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nhello\r\n",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\r\n",
    };
    int n_patterns = 2;
    int total_captures = 0, total_mismatch = 0;

    for (int round = 0; round < 50; round++) {
        for (int p = 0; p < n_patterns; p++) {
            size_t plen = strlen(patterns[p]);
            rc.expect = (const unsigned char *)patterns[p];
            rc.expect_len = plen;

            size_t sent = 0;
            while (sent < plen) {
                ssize_t n = send(client_fd, patterns[p] + sent,
                                 plen - sent, 0);
                if (n <= 0) { perror("send"); goto out; }
                sent += (size_t)n;
            }

            /* Poll ringbuf */
            ring_buffer__poll(rb_mgr, 50);

            /* Drain server recv */
            char buf[4096];
            while (recv(srv_fd, buf, sizeof(buf), MSG_DONTWAIT) > 0) {}

            total_captures = rc.capture_count;
            total_mismatch = rc.mismatch_count;
        }

        if (total_mismatch > 0) break;
        if (round == 0 || (round + 1) % 10 == 0) {
            printf("  round %d/%d  captures=%d mismatch=%d\n",
                   round + 1, 50, total_captures, total_mismatch);
            fflush(stdout);
        }
    }

    /* Final poll */
    ring_buffer__poll(rb_mgr, 200);

    /* 8. Read stats */
    stats_fd = bpf_object__find_map_fd_by_name(obj, "client_stats");
    __u64 hits = 0, read_err = 0, rb_err = 0;
    if (stats_fd >= 0) {
        bpf_map_lookup_elem(stats_fd, &(int){STAT_HIT}, &hits);
        bpf_map_lookup_elem(stats_fd, &(int){STAT_READ_ERR}, &read_err);
        bpf_map_lookup_elem(stats_fd, &(int){STAT_RB_ERR}, &rb_err);
    }

    printf("\n========== RESULT ==========\n");
    printf("  BPF hits:    %llu\n", (unsigned long long)hits);
    printf("  Captures:    %d\n", total_captures);
    printf("  Mismatches:  %d\n", total_mismatch);
    printf("  READ_ERR:    %llu\n", (unsigned long long)read_err);
    printf("  RB_ERR:      %llu\n", (unsigned long long)rb_err);
    printf("============================\n\n");

    if (hits > 0 && total_mismatch == 0 && read_err == 0 && rb_err == 0) {
        printf("PASS: bpf_probe_read_user is RELIABLE on this kernel.\n");
        ret = 0;
    } else if (hits == 0) {
        printf("WARN: No BPF hits — kprobe may not be triggering.\n");
        printf("      Check if PID/port filtering is working.\n");
    } else {
        printf("FAIL: Data integrity issues detected!\n");
    }

out:
    if (rb_mgr) ring_buffer__free(rb_mgr);
    if (lk_kp) bpf_link__destroy(lk_kp);
    if (lk_krp) bpf_link__destroy(lk_krp);
    if (obj) bpf_object__close(obj);
    if (srv_fd >= 0) close(srv_fd);
    if (client_fd >= 0) close(client_fd);
    if (listen_fd >= 0) close(listen_fd);
    return ret;
}
