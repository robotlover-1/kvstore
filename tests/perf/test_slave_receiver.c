/*
 * test_slave_receiver.c — 独立 Slave 接收进程
 *
 * 运行在 Slave 机器上，接受 Master 的转发连接，接收并统计。
 *
 * 用法:
 *   ./test_slave_receiver --port 15801
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile int g_running = 1;

static void sig_handler(int sig) { (void)sig; g_running = 0; }

int main(int argc, char **argv) {
    int port = 15801;

    struct option long_opts[] = {
        {"port", required_argument, 0, 'p'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "p:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'h':
            printf("用法: %s [--port PORT]\n", argv[0]);
            return 0;
        default: return 1;
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_ANY),
                               .sin_port = htons(port)};
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(listen_fd, 5) < 0) {
        perror("listen"); return 1;
    }

    printf("[slave] listening on 0.0.0.0:%d\n", port);
    fflush(stdout);

    long long total_msgs = 0;
    long long total_bytes = 0;
    int connections = 0;

    while (g_running) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        getpeername(fd, (struct sockaddr *)&peer, &peer_len);
        printf("[slave] connection #%d from %s:%d\n",
               ++connections, inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
        fflush(stdout);

        int one_i = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one_i, sizeof(one_i));

        int conn_msgs = 0;
        long long conn_bytes = 0;
        char buf[65536];

        while (1) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            conn_msgs++;
            conn_bytes += n;
        }

        close(fd);
        total_msgs += conn_msgs;
        total_bytes += conn_bytes;
        printf("[slave] connection #%d closed: %d msgs, %.2f MB\n",
               connections, conn_msgs,
               (double)conn_bytes / (1024.0 * 1024.0));
        fflush(stdout);
    }

    printf("[slave] exiting. total: %lld msgs, %.2f MB, %d connections\n",
           total_msgs, (double)total_bytes / (1024.0 * 1024.0), connections);

    close(listen_fd);
    return 0;
}
