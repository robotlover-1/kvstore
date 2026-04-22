#include "kvstore/kvstore.h"

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
    if (!c || !buf || len == 0) return -1;

    out_node_t *n = (out_node_t *)kvs_malloc(sizeof(*n));
    if (!n) return -1;

    n->data = (unsigned char *)kvs_malloc(len);
    if (!n->data) {
        kvs_free(n);
        return -1;
    }

    memcpy(n->data, buf, len);
    n->len = len;
    n->sent = 0;
    n->next = NULL;

    if (c->out_tail) c->out_tail->next = n;
    else c->out_head = n;
    c->out_tail = n;

    mod_events(c, EPOLLIN | EPOLLOUT);
    return 0;
}

void close_conn(conn_t *c) {
    if (!c) return;

    if (c->is_replica) {
        repl_remove_slave(c);
    }

    epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);

    if (c->fd >= 0 && c->fd < (int)(sizeof(fdmap) / sizeof(fdmap[0]))) {
        fdmap[c->fd] = NULL;
    }

    out_node_t *n = c->out_head;
    while (n) {
        out_node_t *next = n->next;
        kvs_free(n->data);
        kvs_free(n);
        n = next;
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
        fdmap[cfd] = c;

        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.fd = cfd;
        epoll_ctl(g_epfd, EPOLL_CTL_ADD, cfd, &ev);
    }
}

static void on_read(conn_t *c) {
    while (1) {
        if (c->in_len >= sizeof(c->inbuf)) {
            close_conn(c);
            return;
        }

        ssize_t n = recv(c->fd, c->inbuf + c->in_len, sizeof(c->inbuf) - c->in_len, 0);
        if (n > 0) {
            c->in_len += (size_t)n;
            parse_resp_stream(c, c->inbuf, &c->in_len, 0);
            continue;
        }

        if (n == 0) {
            if (c->out_head) {
                mod_events(c, EPOLLOUT);
                return;
            }
            close_conn(c);
            return;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        close_conn(c);
        return;
    }

    if (c->out_head) mod_events(c, EPOLLIN | EPOLLOUT);
    else mod_events(c, EPOLLIN);
}

static void on_write(conn_t *c) {
    while (c->out_head) {
        out_node_t *n = c->out_head;

        ssize_t w = send(c->fd, n->data + n->sent, n->len - n->sent, 0);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_conn(c);
            return;
        }
        if (w == 0) break;

        n->sent += (size_t)w;
        if (n->sent == n->len) {
            c->out_head = n->next;
            if (!c->out_head) c->out_tail = NULL;
            kvs_free(n->data);
            kvs_free(n);
        } else {
            break;
        }
    }

    if (c->out_head) mod_events(c, EPOLLIN | EPOLLOUT);
    else mod_events(c, EPOLLIN);
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
            g_last_expire = now;
        }

        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            conn_t *c = fdmap[fd];
            if (!c) continue;

            if (c->is_listener) {
                on_accept(c);
                continue;
            }

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                if (c->out_head) {
                    on_write(c);
                    if (fdmap[fd] == c && !c->out_head) close_conn(c);
                } else {
                    close_conn(c);
                }
                continue;
            }

            if ((events[i].events & EPOLLOUT) && c->out_head) {
                on_write(c);
                if (fdmap[fd] != c) continue;
            }

            if ((events[i].events & EPOLLIN) && fdmap[fd] == c) {
                on_read(c);
                if (fdmap[fd] != c) continue;
            }

            if ((events[i].events & EPOLLOUT) && fdmap[fd] == c && c->out_head) {
                on_write(c);
            }
        }
    }

    return 0;
}
