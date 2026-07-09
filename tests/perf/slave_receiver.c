/*
 * slave_receiver.c — 从机 TCP 接收器
 * 监听指定端口，接收 ebpf-proxy/master 转发的数据并统计。
 *
 * 用法: ./slave_receiver <port>
 * 输出: msgs=<count> bytes=<total> (到 stdout，供测试程序解析)
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static int set_nodelay(int fd) {
    int one = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %d\n", port);
        return 1;
    }

    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_ANY),
                               .sin_port = htons(port)};
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    listen(listen_fd, 5);

    fprintf(stderr, "[slave_receiver] listening on port %d\n", port);

    int msg_count = 0;
    long long total_bytes = 0;

    while (g_running) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        set_nodelay(fd);

        char buf[65536];
        while (g_running) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            msg_count++;
            total_bytes += n;
        }
        close(fd);
    }
    close(listen_fd);

    fprintf(stderr, "[slave_receiver] done: msgs=%d bytes=%lld\n",
            msg_count, total_bytes);
    /* 输出到 stdout 供测试程序解析 */
    printf("msgs=%d bytes=%lld\n", msg_count, total_bytes);
    fflush(stdout);
    return 0;
}
