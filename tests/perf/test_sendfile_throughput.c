/*
 * test_sendfile_throughput.c — sendfile 吞吐量测试
 *
 * 用法:
 *   # 服务端（接收方）
 *   ./test_sendfile_throughput --server
 *
 *   # 客户端（发送方）
 *   ./test_sendfile_throughput --host <server_ip> --size 65536 --iters 10000
 *
 * 依赖: 无（glibc 内置 sendfile）
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.h"

#define DEFAULT_PORT 18517
#define DEFAULT_SIZE 65536
#define DEFAULT_ITERS 5000
#define TEST_FILE "/tmp/perf_test_file"

/* ========== 服务端 ========== */
static int run_server(int port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }

    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_ANY),
                               .sin_port = htons(port)};
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 1) < 0) {
        perror("listen");
        close(listen_fd);
        return -1;
    }

    printf("[server] 监听端口 %d，等待客户端...\n", port);

    int conn = accept(listen_fd, NULL, NULL);
    if (conn < 0) {
        perror("accept");
        close(listen_fd);
        return -1;
    }
    set_nodelay(conn);

    /* 接收并丢弃数据，统计 */
    char buf[65536];
    size_t total = 0;
    double t0 = now_us();

    while (1) {
        ssize_t n = read(conn, buf, sizeof(buf));
        if (n <= 0) break;
        total += (size_t)n;
    }

    double t1 = now_us();
    double elapsed_s = (t1 - t0) / 1000000.0;
    double throughput_bps = (double)(total * 8) / elapsed_s;

    printf("\n=== sendfile 吞吐量结果（服务端）===\n");
    printf("  总接收:     %.2f MB\n", (double)total / (1024.0 * 1024.0));
    printf("  耗时:       %.3f s\n", elapsed_s);
    printf("  吞吐量:     %s\n", throughput_str(throughput_bps));

    close(conn);
    close(listen_fd);
    return 0;
}

/* ========== 客户端 ========== */
static int run_client(const char *host, size_t buf_size, int iters, int port) {
    /* 创建/检测测试文件 */
    int fd = open(TEST_FILE, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || (size_t)st.st_size < buf_size) {
        if (fd >= 0) close(fd);

        /* 创建足够大的测试文件 */
        fd = open(TEST_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            perror("create test file");
            return -1;
        }

        size_t file_size = (size_t)iters * buf_size;
        char block[65536];
        memset(block, 'S', sizeof(block));

        size_t remaining = file_size;
        while (remaining > 0) {
            size_t chunk = remaining > sizeof(block) ? sizeof(block) : remaining;
            if (write_full(fd, block, chunk) < 0) {
                perror("write test file");
                close(fd);
                return -1;
            }
            remaining -= chunk;
        }
        close(fd);

        fd = open(TEST_FILE, O_RDONLY);
        if (fd < 0) {
            perror("open test file");
            return -1;
        }
    }

    /* 连接服务端 */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        close(fd);
        return -1;
    }

    struct hostent *he = gethostbyname(host);
    if (!he) {
        fprintf(stderr, "解析主机 %s 失败\n", host);
        close(sock);
        close(fd);
        return -1;
    }

    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        close(fd);
        return -1;
    }
    set_nodelay(sock);

    /* sendfile 传输 */
    size_t total_sent = 0;
    double t0 = now_us();

    off_t offset = 0;
    for (int i = 0; i < iters; i++) {
        ssize_t n = sendfile(sock, fd, &offset, buf_size);
        if (n < 0) {
            perror("sendfile");
            break;
        }
        if (n == 0) {
            /* 文件到头，重置 offset */
            offset = 0;
            n = sendfile(sock, fd, &offset, buf_size);
            if (n <= 0) break;
        }
        total_sent += (size_t)n;
    }

    /* 确保数据已发送 */
    shutdown(sock, SHUT_WR);

    double t1 = now_us();
    double elapsed_s = (t1 - t0) / 1000000.0;
    double throughput_bps = (double)(total_sent * 8) / elapsed_s;

    printf("\n=== sendfile 吞吐量结果（客户端）===\n");
    printf("  payload:    %zu bytes\n", buf_size);
    printf("  iters:      %d\n", iters);
    printf("  总发送:     %.2f MB\n", (double)total_sent / (1024.0 * 1024.0));
    printf("  耗时:       %.3f s\n", elapsed_s);
    printf("  吞吐量:     %s\n", throughput_str(throughput_bps));

    /* 等待服务端确认收到 */
    char dummy;
    if (read(sock, &dummy, 1) < 0) { /* ignore */ }

    close(sock);
    close(fd);
    return 0;
}

/* ========== Main ========== */
static void usage(const char *prog) {
    fprintf(stderr,
            "用法:\n"
            "  服务端: %s --server\n"
            "  客户端: %s --host <server_ip> [options]\n"
            "选项:\n"
            "  --host, -H     服务端 IP（客户端模式必需）\n"
            "  --server, -s   以服务端模式运行\n"
            "  --port, -p     端口 (默认: 18517)\n"
            "  --size           传输 payload 大小 (默认: 65536)\n"
            "  --iters          传输次数 (默认: 5000)\n"
            "  --help, -h      显示帮助\n",
            prog, prog);
}

int main(int argc, char **argv) {
    const char *host = NULL;
    int server_mode = 0;
    int port = DEFAULT_PORT;
    size_t buf_size = DEFAULT_SIZE;
    int iters = DEFAULT_ITERS;

    struct option long_opts[] = {{"host", required_argument, 0, 'H'},
                                 {"server", no_argument, 0, 's'},
                                 {"port", required_argument, 0, 'p'},
                                 {"size", required_argument, 0, 1000},
                                 {"iters", required_argument, 0, 1001},
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
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }

    if (!server_mode && !host) {
        fprintf(stderr, "错误: 客户端模式需要 --host\n");
        usage(argv[0]);
        return 1;
    }

    return server_mode ? run_server(port) : run_client(host, buf_size, iters, port);
}
