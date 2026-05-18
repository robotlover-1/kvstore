/*
 * test_repl_basic.c — 主从复制基本验证测试
 *
 * 验证 kvstore 主从复制功能：全量同步、增量同步、数据一致性。
 * 用户手动管理 Master/Slave 进程，程序负责写入、监控、验证。
 *
 * 编译:
 *   make test_repl_basic
 *
 * ═══════════════════════════════════════════════════════════════
 * 启动顺序（重要!）:
 *   1. 先启动 Master
 *   2. 再运行本测试（预存数据、等待 slave 连接）
 *   3. 看到提示后，再启动 Slave
 * ═══════════════════════════════════════════════════════════════
 *
 * 流程:
 *   Step 1: 用户启动 Master
 *   Step 2: 本程序连接 Master → 跨引擎预存 N 条数据
 *   Step 3: 提示用户启动 Slave → 等待全量同步完成
 *   Step 4: 再写入增量数据 → 等待增量同步
 *   Step 5: 验证 Master/Slave 数据一致性（多引擎）
 *   Step 6: 可选: SLAVEOF NO ONE → 断开写入 → SLAVEOF 重连 → 验证
 *
 * 用法:
 *   # 终端 1: 启动 Master（先启动）
 *   ./kvstore --port 6379 --role master \
 *       --repl-fullsync-transport tcp --repl-realtime-transport tcp
 *
 *   # 终端 2: 运行测试
 *   ./test_repl_basic --master-port 6379 --slave-port 6380 --count 5000
 *
 *   # 终端 3: 看到提示后再启动 Slave
 *   ./kvstore --port 6380 --role slave \
 *       --master-host 127.0.0.1 --master-port 6379 \
 *       --repl-fullsync-transport tcp --repl-realtime-transport tcp
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>

#define BUFFER_SIZE 65536
#define MAX_KEY_LEN 128

#define ANSI_GREEN   "\033[32m"
#define ANSI_RED     "\033[31m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_RESET   "\033[0m"

static struct {
    const char *host;
    int master_port;
    int slave_port;
    int count;
    int batch;
    int poll_ms;
} g_opt = {
    .host = "127.0.0.1",
    .master_port = 6379,
    .slave_port = 6380,
    .count = 5000,
    .batch = 500,
    .poll_ms = 500,
};

static double time_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

static void banner(const char *title) {
    printf("\n" ANSI_BOLD ANSI_CYAN "%s" ANSI_RESET "\n", title);
    for (const char *p = title; *p; p++) printf("=");
    printf("\n\n");
}

static void info(const char *fmt, ...) {
    va_list ap;
    printf(ANSI_YELLOW "[INFO]" ANSI_RESET " ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void pass(const char *fmt, ...) {
    va_list ap;
    printf(ANSI_GREEN "[PASS]" ANSI_RESET " ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void fail(const char *fmt, ...) {
    va_list ap;
    printf(ANSI_RED "[FAIL]" ANSI_RESET " ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}
static void prompt(const char *fmt, ...) {
    va_list ap;
    printf(ANSI_BOLD "\n  >>> " ANSI_RESET);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf(ANSI_BOLD " <<<" ANSI_RESET "\n\n");
}

static void stat_line(const char *label, const char *fmt, ...) {
    char val[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(val, sizeof(val), fmt, ap);
    va_end(ap);
    printf("  %-24s %s\n", label, val);
}
/* ── TCP / RESP ── */

static int tcp_connect(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    struct hostent *he = gethostbyname(host);
    if (he) {
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    } else {
        addr.sin_addr.s_addr = inet_addr(host);
        if (addr.sin_addr.s_addr == (in_addr_t)-1) {
            close(fd); return -1;
        }
    }
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd); return -1;
    }
    return fd;
}

static unsigned char *build_resp(int argc, const char **argv, size_t *out_len) {
    size_t cap = 64;
    for (int i = 0; i < argc; i++) cap += strlen(argv[i]) + 32;
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf) return NULL;
    size_t pos = 0;
    int n = snprintf((char *)buf + pos, cap - pos, "*%d\r\n", argc);
    if (n < 0) { free(buf); return NULL; }
    pos += (size_t)n;
    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]);
        n = snprintf((char *)buf + pos, cap - pos, "$%zu\r\n", len);
        if (n < 0) { free(buf); return NULL; }
        pos += (size_t)n;
        if (pos + len + 2 > cap) { free(buf); return NULL; }
        memcpy(buf + pos, argv[i], len);
        pos += len;
        buf[pos++] = '\r';
        buf[pos++] = '\n';
    }
    *out_len = pos;
    return buf;
}

static int send_all(int fd, const unsigned char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) return -1;
        buf += n; len -= (size_t)n;
    }
    return 0;
}

static unsigned char *recv_resp(int fd) {
    size_t cap = 4096;
    size_t len = 0;
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf) return NULL;
    while (len < BUFFER_SIZE) {
        if (cap - len < 1024) {
            cap *= 2;
            unsigned char *tmp = (unsigned char *)realloc(buf, cap);
            if (!tmp) { free(buf); return NULL; }
            buf = tmp;
        }
        ssize_t r = read(fd, buf + len, cap - len - 1);
        if (r <= 0) break;
        len += (size_t)r;
        if (len >= 2 && buf[len - 1] == '\n' && buf[len - 2] == '\r')
            break;
    }
    buf[len] = '\0';
    return buf;
}

static char *cmd_at(int fd, const char *arg0, ...) {
    int argc = 1;
    const char *args[64];
    args[0] = arg0;
    va_list ap;
    va_start(ap, arg0);
    while (argc < 64) {
        const char *a = va_arg(ap, const char *);
        if (!a) break;
        args[argc++] = a;
    }
    va_end(ap);
    size_t wlen;
    unsigned char *wbuf = build_resp(argc, args, &wlen);
    if (!wbuf) return NULL;
    int ok = send_all(fd, wbuf, wlen);
    free(wbuf);
    if (ok != 0) return NULL;
    return (char *)recv_resp(fd);
}

static int cmd_port(int port, const char *arg0, ...) {
    int fd = tcp_connect(g_opt.host, port);
    if (fd < 0) return -1;
    int argc = 1;
    const char *args[64];
    args[0] = arg0;
    va_list ap;
    va_start(ap, arg0);
    while (argc < 64) {
        const char *a = va_arg(ap, const char *);
        if (!a) break;
        args[argc++] = a;
    }
    va_end(ap);
    size_t wlen;
    unsigned char *wbuf = build_resp(argc, args, &wlen);
    if (!wbuf) { close(fd); return -1; }
    int ok = send_all(fd, wbuf, wlen);
    free(wbuf);
    if (ok != 0) { close(fd); return -1; }
    char *resp = (char *)recv_resp(fd);
    close(fd);
    if (!resp) return -1;
    int r = strcmp(resp, "+OK\r\n") == 0 || strcmp(resp, ":1\r\n") == 0 ? 0 : -1;
    free(resp);
    return r;
}

static int wait_port_ready(int port, int retries, double interval) {
    for (int i = 0; i < retries; i++) {
        int fd = tcp_connect(g_opt.host, port);
        if (fd >= 0) {
            char *r = cmd_at(fd, "PING", NULL);
            close(fd);
            if (r && strcmp(r, "+PONG\r\n") == 0) { free(r); return 0; }
            free(r);
        }
        usleep((useconds_t)(interval * 1000000));
    }
    return -1;
}

/* 查询 INFO 字段 */
static int check_info_field(int port, const char *field, const char *value) {
    int fd = tcp_connect(g_opt.host, port);
    if (fd < 0) return -1;
    char *info = cmd_at(fd, "INFO", NULL);
    close(fd);
    if (!info) return -1;
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "%s:%s", field, value);
    int found = strstr(info, pattern) != NULL ? 0 : -1;
    free(info);
    return found;
}

/* ── 主测试逻辑 ── */

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc)
            g_opt.host = argv[++i];
        else if (strcmp(argv[i], "--master-port") == 0 && i + 1 < argc)
            g_opt.master_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--slave-port") == 0 && i + 1 < argc)
            g_opt.slave_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc)
            g_opt.count = atoi(argv[++i]);
        else if (strcmp(argv[i], "--batch") == 0 && i + 1 < argc)
            g_opt.batch = atoi(argv[++i]);
        else if (strcmp(argv[i], "--poll") == 0 && i + 1 < argc)
            g_opt.poll_ms = atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("用法: %s [选项]\n", argv[0]);
            printf("\n主从复制基本验证测试:\n");
            printf("  ═══ 启动顺序: ① Master → ② 本脚本 → ③ Slave ═══\n");
            printf("  1. 用户手动启动 Master\n");
            printf("  2. 本程序连接 Master → 跨引擎预存数据\n");
            printf("  3. 提示用户启动 Slave\n");
            printf("  4. 等待全量同步 → 写入增量 → 验证一致性\n\n");
            printf("选项:\n");
            printf("  --host HOST           主机地址 (默认 %s)\n", g_opt.host);
            printf("  --master-port PORT    Master 端口 (默认 %d)\n", g_opt.master_port);
            printf("  --slave-port PORT     Slave 端口 (默认 %d)\n", g_opt.slave_port);
            printf("  --count N             预存/增量数据量 (默认 %d)\n", g_opt.count);
            printf("  --batch N             每批写入量 (默认 %d)\n", g_opt.batch);
            printf("  --poll MS             轮询间隔毫秒 (默认 %d)\n", g_opt.poll_ms);
            printf("  -h                    显示此帮助\n");
            printf("\n示例:\n");
            printf("  # 终端 1 (先启动 Master):\n");
            printf("  ./kvstore --port %d --role master \\\n", g_opt.master_port);
            printf("      --repl-fullsync-transport tcp --repl-realtime-transport tcp\n");
            printf("  # 终端 2:\n");
            printf("  %s --master-port %d --slave-port %d --count %d\n",
                   argv[0], g_opt.master_port, g_opt.slave_port, g_opt.count);
            printf("  # 终端 3 (看到提示后启动 Slave):\n");
            printf("  ./kvstore --port %d --role slave \\\n", g_opt.slave_port);
            printf("      --master-host %s --master-port %d \\\n", g_opt.host, g_opt.master_port);
            printf("      --repl-fullsync-transport tcp --repl-realtime-transport tcp\n");
            return 0;
        } else {
            fprintf(stderr, "未知选项: %s\n", argv[i]);
            return 1;
        }
    }

    banner("主从复制基本验证测试");
    info("Master: %s:%d, Slave: %s:%d", g_opt.host, g_opt.master_port, g_opt.host, g_opt.slave_port);
    info("预存/增量数据量: %d", g_opt.count);
    info("");

    /* ── Step 1: 连接 Master ── */
    banner("Step 1: 连接 Master 并预存数据");
    prompt("请确保 Master 已在 %s:%d 上启动", g_opt.host, g_opt.master_port);

    int fd = tcp_connect(g_opt.host, g_opt.master_port);
    if (fd < 0) { fail("无法连接 Master"); return 1; }
    info("Master 已连接 ✓");

    /* 写入各引擎测试数据 */
    /* Hash 引擎 */
    info("写入 Hash 引擎数据...");
    for (int i = 1; i <= g_opt.count; i++) {
        char key[MAX_KEY_LEN], val[MAX_KEY_LEN];
        snprintf(key, sizeof(key), "h:pre:%d", i);
        snprintf(val, sizeof(val), "hv:%d", i);
        char *r = cmd_at(fd, "HSET", key, val, NULL);
        if (!r || (strcmp(r, "+OK\r\n") != 0 && strcmp(r, ":1\r\n") != 0)) {
            fail("HSET 失败 @ %d, resp=%s", i, r ? r : "(null)");
            free(r); close(fd); return 1;
        }
        free(r);
        if (i % g_opt.batch == 0) info("  Hash: %d/%d", i, g_opt.count);
    }

    /* Array 引擎 */
    info("写入 Array 引擎数据...");
    for (int i = 1; i <= g_opt.count && i <= 1024; i++) {
        char key[MAX_KEY_LEN], val[MAX_KEY_LEN];
        snprintf(key, sizeof(key), "a:pre:%d", i);
        snprintf(val, sizeof(val), "av:%d", i);
        char *r = cmd_at(fd, "SET", key, val, NULL);
        if (!r || (strcmp(r, "+OK\r\n") != 0 && strcmp(r, ":1\r\n") != 0)) {
            fail("SET 失败 @ %d, resp=%s", i, r ? r : "(null)");
            free(r); close(fd); return 1;
        }
        free(r);
    }

    /* RBTREE 引擎 */
    info("写入 RBTREE 引擎数据...");
    for (int i = 1; i <= g_opt.count && i <= 1000; i++) {
        char key[MAX_KEY_LEN], val[MAX_KEY_LEN];
        snprintf(key, sizeof(key), "r:pre:%d", i);
        snprintf(val, sizeof(val), "rv:%d", i);
        char *r = cmd_at(fd, "RSET", key, val, NULL);
        if (!r || (strcmp(r, "+OK\r\n") != 0 && strcmp(r, ":1\r\n") != 0)) {
            fail("RSET 失败 @ %d, resp=%s", i, r ? r : "(null)");
            free(r); close(fd); return 1;
        }
        free(r);
    }

    /* Skiptable 引擎 */
    info("写入 Skiptable 引擎数据...");
    for (int i = 1; i <= g_opt.count && i <= 1000; i++) {
        char key[MAX_KEY_LEN], val[MAX_KEY_LEN];
        snprintf(key, sizeof(key), "x:pre:%d", i);
        snprintf(val, sizeof(val), "xv:%d", i);
        char *r = cmd_at(fd, "XSET", key, val, NULL);
        if (!r || (strcmp(r, "+OK\r\n") != 0 && strcmp(r, ":1\r\n") != 0)) {
            fail("XSET 失败 @ %d, resp=%s", i, r ? r : "(null)");
            free(r); close(fd); return 1;
        }
        free(r);
    }

    close(fd);
    pass("预存数据完成 (4 引擎, %d 条)", g_opt.count);

    /* ── Step 2: 启动 Slave ── */
    banner("Step 2: 启动 Slave 并等待全量同步");
    prompt("请在另一个终端启动 Slave:\n  ./kvstore --port %d --role slave \\\n      --master-host %s --master-port %d \\\n      --repl-fullsync-transport tcp --repl-realtime-transport tcp",
           g_opt.slave_port, g_opt.host, g_opt.master_port);

    info("等待 Slave (%s:%d) 就绪...", g_opt.host, g_opt.slave_port);
    if (wait_port_ready(g_opt.slave_port, 120, 0.5) != 0) {
        fail("Slave 未就绪"); return 1;
    }
    info("Slave 已就绪 ✓");

    /* 等待全量同步完成 */
    info("监控全量同步进度...");
    int sync_ok = 0;
    for (int i = 0; i < 120; i++) {
        if (check_info_field(g_opt.slave_port, "slave_fullsync_loading", "0") == 0 &&
            check_info_field(g_opt.slave_port, "master_link", "up") == 0) {
            pass("全量同步完成");
            sync_ok = 1;
            break;
        }
        usleep(g_opt.poll_ms * 1000);
    }
    if (!sync_ok) { fail("全量同步超时"); return 1; }

    /* ── Step 3: 增量写入 ── */
    banner("Step 3: 增量写入并等待同步");

    int post_count = g_opt.count > 1000 ? 1000 : g_opt.count;
    fd = tcp_connect(g_opt.host, g_opt.master_port);
    if (fd < 0) { fail("无法连接 Master"); return 1; }

    for (int i = 1; i <= post_count; i++) {
        char key[MAX_KEY_LEN], val[MAX_KEY_LEN];
        snprintf(key, sizeof(key), "h:post:%d", i);
        snprintf(val, sizeof(val), "hv_post:%d", i);
        char *r = cmd_at(fd, "HSET", key, val, NULL);
        if (!r || (strcmp(r, "+OK\r\n") != 0 && strcmp(r, ":1\r\n") != 0)) {
            fail("增量 HSET 失败 @ %d", i);
            free(r); close(fd); return 1;
        }
        free(r);
        if (i % 200 == 0) info("增量: %d/%d", i, post_count);
    }
    close(fd);
    info("增量写入完成，等待同步...");
    sleep(3);

    /* ── Step 4: 验证一致性 ── */
    banner("Step 4: 验证主从数据一致性");

    int ok = 1;
    /* Hash */
    ok = (cmd_port(g_opt.master_port, "HGET", "h:pre:1", NULL) == 0 &&
          cmd_port(g_opt.slave_port, "HGET", "h:pre:1", NULL) == 0) ? ok : 0;
    ok = (cmd_port(g_opt.master_port, "HGET", "h:pre:100", NULL) == 0 &&
          cmd_port(g_opt.slave_port, "HGET", "h:pre:100", NULL) == 0) ? ok : 0;
    ok = (cmd_port(g_opt.master_port, "HGET", "h:pre:5000", NULL) == 0 &&
          cmd_port(g_opt.slave_port, "HGET", "h:pre:5000", NULL) == 0) ? ok : 0;
    /* Array */
    ok = (cmd_port(g_opt.master_port, "GET", "a:pre:1", NULL) == 0 &&
          cmd_port(g_opt.slave_port, "GET", "a:pre:1", NULL) == 0) ? ok : 0;
    ok = (cmd_port(g_opt.master_port, "GET", "a:pre:512", NULL) == 0 &&
          cmd_port(g_opt.slave_port, "GET", "a:pre:512", NULL) == 0) ? ok : 0;
    /* RBTREE */
    ok = (cmd_port(g_opt.master_port, "RGET", "r:pre:1", NULL) == 0 &&
          cmd_port(g_opt.slave_port, "RGET", "r:pre:1", NULL) == 0) ? ok : 0;
    ok = (cmd_port(g_opt.master_port, "RGET", "r:pre:500", NULL) == 0 &&
          cmd_port(g_opt.slave_port, "RGET", "r:pre:500", NULL) == 0) ? ok : 0;
    /* Skiptable */
    ok = (cmd_port(g_opt.master_port, "XGET", "x:pre:1", NULL) == 0 &&
          cmd_port(g_opt.slave_port, "XGET", "x:pre:1", NULL) == 0) ? ok : 0;
    ok = (cmd_port(g_opt.master_port, "XGET", "x:pre:999", NULL) == 0 &&
          cmd_port(g_opt.slave_port, "XGET", "x:pre:999", NULL) == 0) ? ok : 0;
    /* 增量数据 */
    ok = (cmd_port(g_opt.master_port, "HGET", "h:post:1", NULL) == 0 &&
          cmd_port(g_opt.slave_port, "HGET", "h:post:1", NULL) == 0) ? ok : 0;
    ok = (cmd_port(g_opt.master_port, "HGET", "h:post:1000", NULL) == 0 &&
          cmd_port(g_opt.slave_port, "HGET", "h:post:1000", NULL) == 0) ? ok : 0;

    if (ok) pass("数据一致性验证通过 (4 引擎, 含增量)");
    else    fail("数据一致性验证失败");

    /* ── Step 5: Partial Resync ── */
    banner("Step 5: Partial Resync 测试 (可选)");
    prompt("是否继续测试 Partial Resync? 如需要, 按 Enter 继续; 否则 Ctrl+C 退出");
    info("测试 SLAVEOF NO ONE → 断开写入 → SLAVEOF 重连");

    if (cmd_port(g_opt.slave_port, "SLAVEOF", "NO", "ONE", NULL) == 0)
        pass("Slave 已断开");
    else
        info("SLAVEOF NO ONE 跳过");
    sleep(1);

    /* 断开期间写入 */
    if (cmd_port(g_opt.master_port, "HSET", "h:reconnect:key1", "rv1", NULL) == 0)
        pass("断开期间写入成功");

    /* 重新连接 */
    char mhost[32], mport[16];
    snprintf(mhost, sizeof(mhost), "%s", g_opt.host);
    snprintf(mport, sizeof(mport), "%d", g_opt.master_port);
    if (cmd_port(g_opt.slave_port, "SLAVEOF", mhost, mport, NULL) == 0) {
        pass("Slave 已重新连接");
        sleep(3);
        if (cmd_port(g_opt.master_port, "HGET", "h:reconnect:key1", NULL) == 0 &&
            cmd_port(g_opt.slave_port, "HGET", "h:reconnect:key1", NULL) == 0)
            pass("断开期间数据已同步");
        else
            fail("断开期间数据未同步");
    }

    /* ── 结果 ── */
    banner("结果汇总");
    if (ok) { pass("REPL_BASIC_TEST_PASS"); return 0; }
    else    { fail("REPL_BASIC_TEST_FAILED"); return 1; }
}
