/*
 * test_batch.c — 批量流水线压力测试
 *
 * 验证 kvstore 的 RESP 流水线处理能力：
 *   1. 通过单次 send 发送 N 条命令，不逐个等待响应
 *   2. 之后一次性读取所有响应并验证
 *   3. 测试纯写入流水线、纯读取流水线、混合流水线
 *
 * 编译:
 *   make test_batch
 *
 * 用法:
 *   # 终端 1: 启动 kvstore
 *   ./kvstore --port 5200 --role master
 *
 *   # 终端 2: 运行本测试
 *   ./test_batch [选项]
 *
 * 选项:
 *   --host HOST       kvstore 地址 (默认 127.0.0.1)
 *   --port PORT       kvstore 端口 (默认 5200)
 *   --count N         每条流水线的命令数 (默认 10000)
 *   -h                显示帮助
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define BUFFER_SIZE (1024 * 1024)
#define MAX_KEY_LEN 64
#define MAX_RESP_SIZE (1024 * 1024)

#define ANSI_GREEN   "\033[32m"
#define ANSI_RED     "\033[31m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_RESET   "\033[0m"

static struct {
    const char *host;
    int port;
    int count;
} g_opt = {
    .host = "127.0.0.1",
    .port = 5200,
    .count = 10000,
};

/* ==== TCP / RESP 工具 ==== */

static int tcp_connect(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    struct hostent *he = gethostbyname(host);
    if (he) memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    else if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    return fd;
}

static int send_all(int fd, const unsigned char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) return -1;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* ==== 构建批处理缓冲区 ==== */

static size_t build_set_batch(unsigned char *buf, size_t cap, int count) {
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        char key[MAX_KEY_LEN], val[MAX_KEY_LEN];
        snprintf(key, sizeof(key), "batch:k:%06d", i);
        snprintf(val, sizeof(val), "batch:v:%06d", i);
        int n = snprintf((char *)buf + pos, cap - pos,
            "*3\r\n$4\r\nHSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
            strlen(key), key, strlen(val), val);
        if (n < 0 || (size_t)n >= cap - pos) break;
        pos += (size_t)n;
    }
    return pos;
}

static size_t build_get_batch(unsigned char *buf, size_t cap, int count) {
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        char key[MAX_KEY_LEN];
        snprintf(key, sizeof(key), "batch:k:%06d", i);
        int n = snprintf((char *)buf + pos, cap - pos,
            "*2\r\n$4\r\nHGET\r\n$%zu\r\n%s\r\n", strlen(key), key);
        if (n < 0 || (size_t)n >= cap - pos) break;
        pos += (size_t)n;
    }
    return pos;
}

static size_t build_mixed_batch(unsigned char *buf, size_t cap, int count) {
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        char key[MAX_KEY_LEN], val[MAX_KEY_LEN];
        snprintf(key, sizeof(key), "batch:mix:%06d", i);
        snprintf(val, sizeof(val), "mix:v:%06d", i);
        int n = snprintf((char *)buf + pos, cap - pos,
            "*3\r\n$4\r\nHSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n"
            "*2\r\n$4\r\nHGET\r\n$%zu\r\n%s\r\n",
            strlen(key), key, strlen(val), val,
            strlen(key), key);
        if (n < 0 || (size_t)n >= cap - pos) break;
        pos += (size_t)n;
    }
    return pos;
}

/* ==== 响应验证 ==== */

static int count_ok(const unsigned char *resp, size_t len) {
    int ok = 0;
    size_t pos = 0;
    while (pos < len) {
        if (resp[pos] == '+') {
            if (pos + 5 <= len && memcmp(resp + pos, "+OK\r\n", 5) == 0) ok++;
            pos++;
            while (pos < len && resp[pos] != '\n') pos++;
            if (pos < len) pos++;
        } else if (resp[pos] == '$') {
            char *end;
            long blen = strtol((const char *)resp + pos + 1, &end, 10);
            if (!end || *end != '\r') break;
            size_t skip = (size_t)(end - (const char *)resp) + 2;
            if (blen >= 0) skip += (size_t)blen + 2;
            if (pos + skip > len) break;
            ok++;
            pos += skip;
        } else if (resp[pos] == ':') {
            ok++;
            pos++;
            while (pos < len && resp[pos] != '\n') pos++;
            if (pos < len) pos++;
        } else if (resp[pos] == '-') {
            pos++;
            while (pos < len && resp[pos] != '\n') pos++;
            if (pos < len) pos++;
        } else break;
    }
    return ok;
}

/* ==== 单条流水线测试 ==== */

static int run_pipeline(int fd, const char *label,
    unsigned char *batch, size_t blen, int expected)
{
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);

    if (send_all(fd, batch, blen) != 0) {
        fprintf(stderr, "  [FAIL] %s: send failed\n", label);
        return -1;
    }

    unsigned char resp[MAX_RESP_SIZE];
    size_t rlen = 0;
    while (rlen < sizeof(resp)) {
        ssize_t r = read(fd, resp + rlen, sizeof(resp) - rlen - 1);
        if (r <= 0) break;
        rlen += (size_t)r;
        if (count_ok(resp, rlen) >= expected) break;
    }
    gettimeofday(&t1, NULL);
    double elapsed = (double)(t1.tv_sec - t0.tv_sec)
                   + (double)(t1.tv_usec - t0.tv_usec) / 1000000.0;

    int ok = count_ok(resp, rlen);
    double qps = (double)expected / (elapsed > 0 ? elapsed : 0.001);

    if (ok == expected) {
        printf(ANSI_GREEN "  [PASS]" ANSI_RESET " %s: %d/%d, %.0f qps, %.3fs\n",
               label, ok, expected, qps, elapsed);
        return 0;
    } else {
        printf(ANSI_RED "  [FAIL]" ANSI_RESET " %s: %d/%d, %.0f qps, %.3fs\n",
               label, ok, expected, qps, elapsed);
        return -1;
    }
}

/* ==== 帮助 ==== */

static void print_usage(const char *prog) {
    printf("用法: %s [选项]\n", prog);
    printf("  批量流水线压力测试\n\n");
    printf("选项:\n");
    printf("  --host HOST   kvstore 地址 (默认 %s)\n", g_opt.host);
    printf("  --port PORT   kvstore 端口 (默认 %d)\n", g_opt.port);
    printf("  --count N     每条流水线的命令数 (默认 %d)\n", g_opt.count);
    printf("  -h            显示帮助\n");
}

/* ==== 主函数 ==== */

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc)
            g_opt.host = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            g_opt.port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc)
            g_opt.count = atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "未知选项: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    printf(ANSI_BOLD "批量流水线压力测试\n" ANSI_RESET);
    printf("  地址: %s:%d\n", g_opt.host, g_opt.port);
    printf("  每条流水线: %d 条命令\n\n", g_opt.count);

    int fd = tcp_connect(g_opt.host, g_opt.port);
    if (fd < 0) {
        fprintf(stderr, ANSI_RED "错误: 无法连接 %s:%d\n" ANSI_RESET, g_opt.host, g_opt.port);
        return 1;
    }

    unsigned char *buf = (unsigned char *)malloc(BUFFER_SIZE * 2);
    if (!buf) { close(fd); return 1; }
    int failed = 0;

    /* 测试 1: 写入流水线 */
    printf("--- 写入流水线 ---\n");
    size_t wlen = build_set_batch(buf, BUFFER_SIZE, g_opt.count);
    if (run_pipeline(fd, "HSET", buf, wlen, g_opt.count) != 0) failed++;

    /* 测试 2: 读取流水线 */
    printf("--- 读取流水线 ---\n");
    size_t rlen = build_get_batch(buf, BUFFER_SIZE, g_opt.count);
    if (run_pipeline(fd, "HGET", buf, rlen, g_opt.count) != 0) failed++;

    /* 测试 3: 混合流水线 */
    printf("--- 混合流水线 ---\n");
    size_t mlen = build_mixed_batch(buf, BUFFER_SIZE * 2, g_opt.count);
    if (run_pipeline(fd, "HSET+HGET", buf, mlen, g_opt.count * 2) != 0) failed++;

    free(buf);
    close(fd);

    printf("\n");
    if (failed == 0)
        printf(ANSI_GREEN ANSI_BOLD "  全部流水线测试通过 ✓\n" ANSI_RESET);
    else
        printf(ANSI_RED ANSI_BOLD "  %d 项测试失败 ✗\n" ANSI_RESET, failed);

    return failed;
}
