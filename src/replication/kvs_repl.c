#include "kvstore/kvstore.h"

static void *slave_thread(void *arg) {
    (void)arg;
    for (;;) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { sleep(1); continue; }
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)g_cfg.master_port);
        inet_pton(AF_INET, g_cfg.master_host, &addr.sin_addr);
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); sleep(1); continue; }
        unsigned char cmd[128];
        size_t n = resp_build_cmd1(cmd, sizeof(cmd), "REPLSYNC");
        send(fd, cmd, n, 0);
        unsigned char buf[BUFFER_CAP];
        size_t blen = 0;
        for (;;) {
            ssize_t r = recv(fd, buf + blen, sizeof(buf) - blen, 0);
            if (r <= 0) break;
            blen += (size_t)r;
            parse_resp_stream(NULL, buf, &blen, 1);
        }
        close(fd);
        sleep(1);
    }
    return NULL;
}

int start_slave_thread(void) {
    pthread_t tid;
    if (pthread_create(&tid, NULL, slave_thread, NULL) != 0) return -1;
    pthread_detach(tid);
    return 0;
}
