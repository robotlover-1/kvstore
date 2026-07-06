#ifndef PROXY_SLAVE_H
#define PROXY_SLAVE_H

#include <stdint.h>

#define PROXY_SLAVE_BACKOFF_INIT_MS  100
#define PROXY_SLAVE_BACKOFF_MAX_MS   5000

typedef struct {
    int fd;
    char host[64];
    int port;
    unsigned int backoff_ms;       /* 当前退避间隔 */
    unsigned int backoff_max_ms;   /* 最大退避间隔 5000ms */
} proxy_slave_ctx_t;

/* 初始化 slave 上下文 */
void proxy_slave_init(proxy_slave_ctx_t *ctx, const char *host, int port);

/* 连接 slave。返回 0 成功，-1 失败。调用方负责管理退避策略 */
int proxy_slave_connect(proxy_slave_ctx_t *ctx);

/* 断开连接 */
void proxy_slave_disconnect(proxy_slave_ctx_t *ctx);

/* 检查是否已连接 */
int proxy_slave_is_connected(proxy_slave_ctx_t *ctx);

/* 获取 fd（-1 表示未连接） */
int proxy_slave_fd(proxy_slave_ctx_t *ctx);

#endif /* PROXY_SLAVE_H */
