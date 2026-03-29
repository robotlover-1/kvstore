#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "server.h"

#define CONNECTION_SIZE 1024
#define MAX_PORTS 20

static struct conn conn_list[CONNECTION_SIZE];
static int epfd = 0;

typedef int (*msg_handler)(char *msg, int length, char *response);
static msg_handler kvs_handler;

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_event(int fd, int event, int add) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = event;
    ev.data.fd = fd;
    return epoll_ctl(epfd, add ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, fd, &ev);
}

static void close_conn(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return;
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    kvs_stream_destroy(&conn_list[fd].stream);
    memset(&conn_list[fd], 0, sizeof(conn_list[fd]));
}

static int send_cb(int fd);
static int recv_cb(int fd);

static int event_register(int fd, int event) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return -1;
    conn_list[fd].fd = fd;
    conn_list[fd].r_action.recv_callback = recv_cb;
    conn_list[fd].send_callback = send_cb;
    kvs_stream_init(&conn_list[fd].stream);
    set_nonblock(fd);
    return set_event(fd, event, 1);
}

static int accept_cb(int fd) {
    while (1) {
        struct sockaddr_in clientaddr;
        socklen_t len = sizeof(clientaddr);
        int clientfd = accept(fd, (struct sockaddr *)&clientaddr, &len);
        if (clientfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        event_register(clientfd, EPOLLIN | EPOLLET);
    }
}

static int recv_cb(int fd) {
    unsigned char tmp[KVS_READ_CHUNK];
    while (1) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n == 0) {
            close_conn(fd);
            return 0;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_conn(fd);
            return -1;
        }
        if (kvs_stream_feed(&conn_list[fd].stream, tmp, (size_t)n) != 0) {
            close_conn(fd);
            return -1;
        }
    }
    if (kvs_stream_has_output(&conn_list[fd].stream)) {
        set_event(fd, EPOLLIN | EPOLLOUT | EPOLLET, 0);
    }
    return 0;
}

static int send_cb(int fd) {
    kvs_stream_t *s = &conn_list[fd].stream;
    while (kvs_stream_has_output(s)) {
        const unsigned char *ptr = kvs_stream_output_ptr(s);
        size_t len = kvs_stream_output_len(s);
        ssize_t n = send(fd, ptr, len, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_conn(fd);
            return -1;
        }
        kvs_stream_consume_output(s, (size_t)n);
    }
    set_event(fd, kvs_stream_has_output(s) ? (EPOLLIN | EPOLLOUT | EPOLLET) : (EPOLLIN | EPOLLET), 0);
    return 0;
}

static int r_init_server(unsigned short port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    struct sockaddr_in servaddr;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);
    if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) != 0) return -1;
    if (listen(sockfd, 128) != 0) return -1;
    set_nonblock(sockfd);
    return sockfd;
}

int reactor_start(unsigned short port, msg_handler handler) {
    int i;
    kvs_handler = handler;
    (void)kvs_handler;
    epfd = epoll_create(1);
    for (i = 0; i < MAX_PORTS; ++i) {
        int sockfd = r_init_server(port + i);
        conn_list[sockfd].fd = sockfd;
        conn_list[sockfd].r_action.accept_callback = accept_cb;
        set_event(sockfd, EPOLLIN | EPOLLET, 1);
    }
    while (1) {
        struct epoll_event events[1024];
        int nready = epoll_wait(epfd, events, 1024, -1);
        for (i = 0; i < nready; ++i) {
            int fd = events[i].data.fd;
            if (conn_list[fd].r_action.accept_callback == accept_cb) {
                accept_cb(fd);
                continue;
            }
            if (events[i].events & EPOLLIN) conn_list[fd].r_action.recv_callback(fd);
            if (fd < CONNECTION_SIZE && conn_list[fd].fd && (events[i].events & EPOLLOUT)) conn_list[fd].send_callback(fd);
        }
    }
}
