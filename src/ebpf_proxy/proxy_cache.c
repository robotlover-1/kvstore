#include "proxy_cache.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <stdio.h>

void cache_init(cache_ctx_t *ctx) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
}

int cache_append(cache_ctx_t *ctx, const unsigned char *data, size_t len) {
    if (!ctx || !data || len == 0) return -1;

    cache_node_t *node = (cache_node_t *)malloc(sizeof(cache_node_t) + len);
    if (!node) return -1;
    node->next = NULL;
    node->len = len;
    memcpy(node->data, data, len);

    /* 上限检查：丢弃最旧节点 */
    while (ctx->total_bytes + len > PROXY_CACHE_MAX_BYTES && ctx->head) {
        cache_node_t *old = ctx->head;
        ctx->head = old->next;
        if (!ctx->head) ctx->tail = NULL;
        ctx->total_bytes -= old->len;
        ctx->node_count--;
        ctx->dropped++;
        free(old);
    }

    /* 追加 */
    if (!ctx->head) {
        ctx->head = node;
        ctx->tail = node;
    } else {
        ctx->tail->next = node;
        ctx->tail = node;
    }
    ctx->total_bytes += len;
    ctx->node_count++;
    if (ctx->total_bytes > ctx->max_bytes) ctx->max_bytes = ctx->total_bytes;
    return 0;
}

int cache_flush(cache_ctx_t *ctx, int fd) {
    if (!ctx || fd < 0) return -1;
    int sent = 0;
    cache_node_t *node = ctx->head;
    while (node) {
        cache_node_t *next = node->next;
        ssize_t n = send(fd, node->data, node->len, MSG_NOSIGNAL);
        if (n != (ssize_t)node->len) {
            fprintf(stderr, "ebpf-proxy: cache_flush send failed: %zd/%zu\n",
                    n, node->len);
            /* 发送失败时停止 flush，保留剩余节点 */
            ctx->head = node;
            return sent;
        }
        sent++;
        ctx->total_bytes -= node->len;
        ctx->node_count--;
        free(node);
        node = next;
    }
    ctx->head = NULL;
    ctx->tail = NULL;
    ctx->total_bytes = 0;
    ctx->node_count = 0;
    return sent;
}

void cache_destroy(cache_ctx_t *ctx) {
    if (!ctx) return;
    cache_node_t *node = ctx->head;
    while (node) {
        cache_node_t *next = node->next;
        free(node);
        node = next;
    }
    memset(ctx, 0, sizeof(*ctx));
}

void cache_stats(cache_ctx_t *ctx, unsigned long long *dropped_out,
                 size_t *max_bytes_out) {
    if (!ctx) return;
    if (dropped_out) *dropped_out = ctx->dropped;
    if (max_bytes_out) *max_bytes_out = ctx->max_bytes;
}
