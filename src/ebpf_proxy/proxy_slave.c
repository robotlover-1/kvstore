#include "proxy_slave.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

void proxy_slave_init(proxy_slave_ctx_t *ctx, const char *host, int port) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;
    ctx->backoff_ms = PROXY_SLAVE_BACKOFF_INIT_MS;
    ctx->backoff_max_ms = PROXY_SLAVE_BACKOFF_MAX_MS;
    if (host) snprintf(ctx->host, sizeof(ctx->host), "%s", host);
    ctx->port = port;
}

int proxy_slave_connect(proxy_slave_ctx_t *ctx) {
    struct sockaddr_in addr;
    struct timeval tv;

    if (!ctx || ctx->host[0] == '\0' || ctx->port <= 0) return -1;
    if (ctx->fd > 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }

    ctx->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->fd < 0) {
        perror("ebpf-proxy: slave socket");
        return -1;
    }

    tv.tv_sec = 1; tv.tv_usec = 0;
    setsockopt(ctx->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(ctx->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)ctx->port);
    if (inet_pton(AF_INET, ctx->host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "ebpf-proxy: invalid slave host %s\n", ctx->host);
        close(ctx->fd); ctx->fd = -1; return -1;
    }

    if (connect(ctx->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "ebpf-proxy: slave connect failed %s:%d (errno=%d), "
                "backoff %ums\n", ctx->host, ctx->port, errno, ctx->backoff_ms);
        close(ctx->fd); ctx->fd = -1;
        return -1;
    }

    fprintf(stderr, "ebpf-proxy: connected to slave %s:%d fd=%d\n",
            ctx->host, ctx->port, ctx->fd);
    ctx->backoff_ms = PROXY_SLAVE_BACKOFF_INIT_MS;
    return 0;
}

void proxy_slave_disconnect(proxy_slave_ctx_t *ctx) {
    if (!ctx || ctx->fd < 0) return;
    close(ctx->fd);
    ctx->fd = -1;
}

int proxy_slave_is_connected(proxy_slave_ctx_t *ctx) {
    return ctx && ctx->fd > 0;
}

int proxy_slave_fd(proxy_slave_ctx_t *ctx) {
    return ctx ? ctx->fd : -1;
}
