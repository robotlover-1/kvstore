#include "kvstore/kvstore.h"

#define MAX_READS_PER_EVENT 16

int g_epfd = -1;
static conn_t *fdmap[65536];
static long long g_last_expire = 0;

static int expire_cycle_budget(void) {
    size_t count = global_expire.count;
    if (count >= 1000000) return 4096;
    if (count >= 300000) return 2048;
    if (count >= 100000) return 1024;
    if (count >= 30000) return 512;
    if (count >= 10000) return 256;
    if (count >= 1000) return 128;
    return 32;
}

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int mod_events(conn_t *c, uint32_t events) {
    if (!c) return -1;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = c->fd;
    return epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

int queue_bytes(conn_t *c, const unsigned char *buf, size_t len) {
    size_t tail, first_chunk;
    size_t old_len;

    if (!c || !buf || len == 0) return -1;
    if (len > OUT_RING_SIZE - c->out_ring_len) return -1;

    old_len = c->out_ring_len;
    tail = c->out_ring_tail;
    first_chunk = OUT_RING_SIZE - tail;
    if (len <= first_chunk) {
        memcpy(c->out_ring + tail, buf, len);
    } else {
        memcpy(c->out_ring + tail, buf, first_chunk);
        memcpy(c->out_ring, buf + first_chunk, len - first_chunk);
    }
    c->out_ring_tail = (tail + len) & (OUT_RING_SIZE - 1);
    c->out_ring_len += len;

    /* register EPOLLOUT only when transitioning from empty→non-empty.
     * avoids redundant epoll_ctl per pipelined response while ensuring
     * cross-connection writes (replication) trigger EPOLLOUT registration. */
    if (old_len == 0) {
        mod_events(c, EPOLLIN | EPOLLOUT);
    }
    return 0;
}

void close_conn(conn_t *c) {
    if (!c) return;

    if (c->is_replica) {
        repl_remove_slave(c);
    }

    repl_ebpf_unregister_fd(c->fd);
    epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);

    if (c->fd >= 0 && c->fd < (int)(sizeof(fdmap) / sizeof(fdmap[0]))) {
        fdmap[c->fd] = NULL;
    }

    kvs_free(c);
}

static void on_accept(conn_t *lc) {
    while (1) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);

        int cfd = accept(lc->fd, (struct sockaddr *)&cli, &len);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return;
        }

        if (cfd >= (int)(sizeof(fdmap) / sizeof(fdmap[0]))) {
            close(cfd);
            continue;
        }

        set_nonblock(cfd);

        conn_t *c = (conn_t *)kvs_calloc(1, sizeof(*c));
        if (!c) {
            close(cfd);
            continue;
        }

        c->fd = cfd;
        c->repl_transport_kind = KVS_REPL_TRANSPORT_TCP;
        fdmap[cfd] = c;

        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.fd = cfd;
        epoll_ctl(g_epfd, EPOLL_CTL_ADD, cfd, &ev);
    }
}

static void on_write(conn_t *c);

static void on_read(conn_t *c) {
    int reads = 0;
    while (1) {
        if (c->in_len >= sizeof(c->inbuf)) {
            persist_group_commit();
            close_conn(c);
            return;
        }

        ssize_t n = recv(c->fd, c->inbuf + c->in_len, sizeof(c->inbuf) - c->in_len, 0);
        if (n > 0) {
            c->in_len += (size_t)n;
            /* each recv() may contain P > 1 pipelined commands.
               group all commands from this TCP segment so they
               share a single fsync via IOSQE_IO_LINK chain.
               then wait synchronously for the fsync to complete —
               no eventfd→epoll round-trip needed. */
            persist_group_begin();
            parse_resp_stream(c, c->inbuf, &c->in_len, 0);
            persist_group_commit();
            persist_drain_or_wait();
            if (++reads >= MAX_READS_PER_EVENT) break;
            continue;
        }

        if (n == 0) {
            persist_group_commit();
            if (c->out_ring_len > 0) {
                mod_events(c, EPOLLOUT);
                return;
            }
            close_conn(c);
            return;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        persist_group_commit();
        close_conn(c);
        return;
    }

    /* try immediate write after processing pipeline batch:
     * if parse_resp_stream queued multiple responses, send()
     * them now instead of waiting for next epoll_wait→EPOLLOUT */
    if (c->out_ring_len > 0) on_write(c);

    if (c->out_ring_len > 0) mod_events(c, EPOLLIN | EPOLLOUT);
    else mod_events(c, EPOLLIN);

    persist_reap_completions();
}

static void on_write(conn_t *c) {
    while (c->out_ring_len > 0) {
        size_t head = c->out_ring_head;
        size_t chunk = OUT_RING_SIZE - head;
        ssize_t w;

        if (chunk > c->out_ring_len) chunk = c->out_ring_len;

        w = send(c->fd, c->out_ring + head, chunk, 0);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_conn(c);
            return;
        }
        if (w == 0) break;

        c->out_ring_head = (head + (size_t)w) & (OUT_RING_SIZE - 1);
        c->out_ring_len -= (size_t)w;
    }

    if (c->out_ring_len > 0) mod_events(c, EPOLLIN | EPOLLOUT);
    else mod_events(c, EPOLLIN);
}

/* attempt immediate non-blocking write of queued output */
void flush_conn_output(conn_t *c) {
    if (c && c->out_ring_len > 0) on_write(c);
}

int reactor_start(void) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return -1;

    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    set_nonblock(lfd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_cfg.port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(lfd);
        return -1;
    }

    if (listen(lfd, LISTEN_BACKLOG) < 0) {
        close(lfd);
        return -1;
    }

    g_epfd = epoll_create1(0);
    if (g_epfd < 0) {
        close(lfd);
        return -1;
    }

    /* register persist uring eventfd for async AOF CQE notification */
    {
        int pefd = persist_uring_fd();
        if (pefd >= 0) {
            struct epoll_event pev;
            memset(&pev, 0, sizeof(pev));
            pev.events = EPOLLIN;
            pev.data.fd = pefd;
            epoll_ctl(g_epfd, EPOLL_CTL_ADD, pefd, &pev);
        }
    }

    conn_t *lc = (conn_t *)kvs_calloc(1, sizeof(*lc));
    if (!lc) {
        close(lfd);
        close(g_epfd);
        g_epfd = -1;
        return -1;
    }

    lc->fd = lfd;
    lc->is_listener = 1;
    fdmap[lfd] = lc;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = lfd;
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, lfd, &ev);

    g_last_expire = kvs_now_ms();

    if (g_cfg.role == ROLE_SLAVE) start_slave_thread();

    while (1) {
        struct epoll_event events[MAX_EVENTS];
        int n = epoll_wait(g_epfd, events, MAX_EVENTS, 100);

        long long now = kvs_now_ms();
        if (now - g_last_expire >= 100) {
            int budget = expire_cycle_budget();
            kvs_active_expire_cycle(budget);
            persist_autosnap_cron();
#if KVS_ENABLE_KPROBE_RDMA
            extern void repl_kprobe_fwd_health_check(void);
            repl_kprobe_fwd_health_check();
#endif
            g_last_expire = now;
        }

        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            /* persist uring eventfd: reap completed async AOF writes */
            if (fd == persist_uring_fd()) {
                uint64_t val;
                ssize_t nread = read(fd, &val, sizeof(val));
                (void)nread;
                persist_reap_completions();
                continue;
            }

            conn_t *c = fdmap[fd];
            if (!c) continue;

            if (c->is_listener) {
                on_accept(c);
                continue;
            }

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                if (c->out_ring_len > 0) {
                    on_write(c);
                    if (fdmap[fd] == c && c->out_ring_len == 0) close_conn(c);
                } else {
                    close_conn(c);
                }
                continue;
            }

            if ((events[i].events & EPOLLOUT) && c->out_ring_len > 0) {
                on_write(c);
                if (fdmap[fd] != c) continue;
            }

            if ((events[i].events & EPOLLIN) && fdmap[fd] == c) {
                on_read(c);
                if (fdmap[fd] != c) continue;
            }

            if ((events[i].events & EPOLLOUT) && fdmap[fd] == c && c->out_ring_len > 0) {
                on_write(c);
            }
        }

    }

    return 0;
}
