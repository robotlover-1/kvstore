#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include "server.h"

#define MAX_PORTS 1
#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

int accept_cb(int fd);
int recv_cb(int fd);
int send_cb(int fd);

static int epfd = 0;
static struct conn conn_list[CONNECTION_SIZE] = {0};
static struct timeval expire_last;

static int set_event(int fd, int event, int add) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = event;
    ev.data.fd = fd;
    return epoll_ctl(epfd, add ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, fd, &ev);
}

static int event_register(int fd, int event) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return -1;
    conn_list[fd].fd = fd;
    conn_list[fd].r_action.recv_callback = recv_cb;
    conn_list[fd].send_callback = send_cb;
    kvs_stream_init(&conn_list[fd].stream);
    return set_event(fd, event, 1);
}

int accept_cb(int fd) {
    struct sockaddr_in clientaddr; socklen_t len = sizeof(clientaddr);
    int clientfd = accept(fd, (struct sockaddr*)&clientaddr, &len);
    if (clientfd < 0) return -1;
    return event_register(clientfd, EPOLLIN);
}

int recv_cb(int fd) {
    unsigned char buf[BUFFER_LENGTH];
    int count = recv(fd, buf, sizeof(buf), 0);
    if (count <= 0) {
        close(fd);
        kvs_stream_free(&conn_list[fd].stream);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        return 0;
    }
    kvs_stream_feed(&conn_list[fd].stream, buf, (size_t)count);
    if (conn_list[fd].stream.out_head) set_event(fd, EPOLLOUT, 0);
    return count;
}

int send_cb(int fd) {
    kvs_stream_t *s = &conn_list[fd].stream;
    while (s->out_head) {
        kvs_out_node_t *n = s->out_head;
        ssize_t sent = send(fd, n->data + n->sent, n->len - n->sent, 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close(fd);
            kvs_stream_free(s);
            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
            return -1;
        }
        n->sent += (size_t)sent;
        if (n->sent == n->len) {
            s->out_head = n->next;
            if (!s->out_head) s->out_tail = NULL;
            s->out_queued_bytes -= n->len;
            kvs_free(n->data); kvs_free(n);
        } else break;
    }
    set_event(fd, s->out_head ? EPOLLOUT : EPOLLIN, 0);
    return 0;
}

static int r_init_server(unsigned short port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);
    if (bind(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) return -1;
    listen(sockfd, 128);
    return sockfd;
}

int reactor_start(unsigned short port, msg_handler handler) {
    (void)handler;
    epfd = epoll_create(1);
    for (int i = 0; i < MAX_PORTS; i++) {
        int sockfd = r_init_server(port + i);
        conn_list[sockfd].fd = sockfd;
        conn_list[sockfd].r_action.accept_callback = accept_cb;
        set_event(sockfd, EPOLLIN, 1);
    }
    gettimeofday(&expire_last, NULL);
    while (1) {
        struct epoll_event events[128];
        int nready = epoll_wait(epfd, events, 128, 100);
        struct timeval now; gettimeofday(&now, NULL);
        if (TIME_SUB_MS(now, expire_last) >= 100) { kvs_active_expire_cycle(20); expire_last = now; }
        for (int i = 0; i < nready; ++i) {
            int connfd = events[i].data.fd;
            if (events[i].events & EPOLLIN) conn_list[connfd].r_action.recv_callback(connfd);
            if (events[i].events & EPOLLOUT) conn_list[connfd].send_callback(connfd);
        }
    }
    return 0;
}
