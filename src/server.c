#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "server.h"

/* Single threaded event loop. One thread waits on epoll for sockets that
 * are ready, reads whatever arrived, runs every complete command, and
 * writes the replies. Because only one thread ever touches the data, no
 * locks are needed. */

server_t server;

#define READ_CHUNK 16384
#define MAX_OUTPUT_BYTES (256u * 1024 * 1024)
#define CRON_INTERVAL_MS 100
#define MAX_EVENTS 256

static client_t *closing_list;
static client_t **pending;
static size_t npending, cap_pending;

/* ---------- clients ---------- */

static void epoll_set(client_t *c, int op, uint32_t events) {
    struct epoll_event ev = {.events = events, .data.ptr = c};
    epoll_ctl(server.epfd, op, c->fd, &ev);
}

static client_t *client_create(int fd) {
    client_t *c = xcalloc(1, sizeof *c);
    c->fd = fd;
    epoll_set(c, EPOLL_CTL_ADD, EPOLLIN);
    server.nclients++;
    server.total_conns++;
    return c;
}

/* A client can be closed while we are still working through a batch of
 * epoll events that mention it, so closing only marks it and closes the
 * socket. The memory is freed once the batch is done. */
static void client_close(client_t *c) {
    if (c->closing) return;
    c->closing = 1;
    epoll_ctl(server.epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    c->fd = -1;
    server.nclients--;
    c->next_closing = closing_list;
    closing_list = c;
}

static void free_closed_clients(void) {
    while (closing_list) {
        client_t *c = closing_list;
        closing_list = c->next_closing;
        buf_free(&c->in);
        buf_free(&c->out);
        args_free(&c->args);
        free(c);
    }
}

static void add_pending(client_t *c) {
    if (c->pending) return;
    if (npending == cap_pending) {
        cap_pending = cap_pending ? cap_pending * 2 : 64;
        pending = xrealloc(pending, cap_pending * sizeof *pending);
    }
    c->pending = 1;
    pending[npending++] = c;
}

/* Write as much queued output as the socket will take. If it fills up,
 * ask epoll to tell us when it is writable again. */
static void client_flush(client_t *c) {
    if (c->closing) return;
    while (buf_used(&c->out) > 0) {
        ssize_t n = send(c->fd, buf_ptr(&c->out), buf_used(&c->out), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!c->want_write) {
                    c->want_write = 1;
                    epoll_set(c, EPOLL_CTL_MOD, EPOLLIN | EPOLLOUT);
                }
                return;
            }
            client_close(c);
            return;
        }
        buf_consume(&c->out, (size_t)n);
    }
    buf_shrink_if_idle(&c->out, 256 * 1024);
    if (c->want_write) {
        c->want_write = 0;
        epoll_set(c, EPOLL_CTL_MOD, EPOLLIN);
    }
    if (c->close_after_reply) client_close(c);
}

static void process_input(client_t *c) {
    while (!c->closing && !c->close_after_reply && buf_used(&c->in) > 0) {
        size_t consumed;
        const char *err = NULL;
        resp_status st = resp_parse(buf_ptr(&c->in), buf_used(&c->in), &c->args, &consumed, &err);
        if (st == RESP_INCOMPLETE) break;
        if (st == RESP_ERROR) {
            buf_appendf(&c->out, "-ERR Protocol error: %s\r\n", err);
            c->close_after_reply = 1;
            break;
        }
        exec_command(c);
        buf_consume(&c->in, consumed);
    }
    if (buf_used(&c->in) > RESP_MAX_BULK + (1u << 20)) client_close(c);
    if (c->closing) return;
    if (buf_used(&c->out) > MAX_OUTPUT_BYTES) {
        client_close(c);
        return;
    }
    if (buf_used(&c->out) > 0 || c->close_after_reply) add_pending(c);
    buf_shrink_if_idle(&c->in, 256 * 1024);
}

static void client_read(client_t *c) {
    buf_reserve(&c->in, READ_CHUNK);
    ssize_t n = read(c->fd, c->in.data + c->in.len, c->in.cap - c->in.len);
    if (n == 0) return client_close(c);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return;
        return client_close(c);
    }
    c->in.len += (size_t)n;
    process_input(c);
}

static void accept_clients(void) {
    for (;;) {
        int fd = accept4(server.listenfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) log_msg("accept: %s", strerror(errno));
            return;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        client_create(fd);
    }
}

/* ---------- periodic work ---------- */

static void on_expired(const entry_t *e, void *ud) {
    (void)ud;
    arg_t del[2] = {{"DEL", 3}, {e->key, e->klen}};
    aof_feed(del, 2);
}

/* Keys nobody asks for would never be deleted by lazy expiry, so sweep a
 * slice of the table each tick, spending at most about a millisecond. */
static void active_expire(void) {
    static size_t cursor;
    long long start = mono_us(), now = now_ms();
    do {
        dict_sweep_expired(server.db, &cursor, 64, now, on_expired, NULL);
    } while (mono_us() - start < 1000 && cursor != 0);
}

static void cron(void) {
    active_expire();
    aof_cron();
}

/* ---------- setup ---------- */

static void on_signal(int sig) {
    (void)sig;
    server.shutdown = 1;
}

static int listen_on(const char *addr, int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 511) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [--port N] [--bind ADDR] [--aof FILE] [--fsync always|everysec|no]\n"
            "  --port N       TCP port (default 6399)\n"
            "  --bind ADDR    IPv4 address to listen on (default 127.0.0.1)\n"
            "  --aof FILE     keep an append only file for persistence (default: off)\n"
            "  --fsync MODE   when to flush the file to disk: always, everysec (default), no\n",
            prog);
}

int main(int argc, char **argv) {
    server.port = 6399;
    server.bind_addr = "127.0.0.1";
    server.aof_fd = -1;
    server.fsync_policy = FSYNC_EVERYSEC;
    const char *aof_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            server.port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--bind") && i + 1 < argc) {
            server.bind_addr = argv[++i];
        } else if (!strcmp(argv[i], "--aof") && i + 1 < argc) {
            aof_path = argv[++i];
        } else if (!strcmp(argv[i], "--fsync") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "always")) server.fsync_policy = FSYNC_ALWAYS;
            else if (!strcmp(m, "everysec")) server.fsync_policy = FSYNC_EVERYSEC;
            else if (!strcmp(m, "no")) server.fsync_policy = FSYNC_NO;
            else return usage(argv[0]), 2;
        } else {
            return usage(argv[0]), 2;
        }
    }
    if (server.port <= 0 || server.port > 65535) return usage(argv[0]), 2;

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = {.sa_handler = on_signal};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    server.start_ms = now_ms();
    server.db = dict_create();
    commands_init();
    if (aof_path && aof_open(aof_path) != 0) return 1;

    server.listenfd = listen_on(server.bind_addr, server.port);
    if (server.listenfd < 0) {
        log_msg("cannot listen on %s:%d: %s", server.bind_addr, server.port, strerror(errno));
        return 1;
    }
    server.epfd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event lev = {.events = EPOLLIN, .data.ptr = NULL};
    epoll_ctl(server.epfd, EPOLL_CTL_ADD, server.listenfd, &lev);
    log_msg("mini redis %s ready on %s:%d%s", MINI_VERSION, server.bind_addr, server.port,
            aof_path ? " (persistence on)" : " (in memory only)");

    struct epoll_event events[MAX_EVENTS];
    long long last_cron = mono_us();
    while (!server.shutdown) {
        int n = epoll_wait(server.epfd, events, MAX_EVENTS, CRON_INTERVAL_MS);
        for (int i = 0; i < n; i++) {
            client_t *c = events[i].data.ptr;
            if (!c) {
                accept_clients();
                continue;
            }
            if (c->closing) continue;
            uint32_t ev = events[i].events;
            if (ev & (EPOLLERR | EPOLLHUP)) {
                client_close(c);
                continue;
            }
            if (ev & EPOLLIN) client_read(c);
            if ((ev & EPOLLOUT) && !c->closing) client_flush(c);
        }

        if (mono_us() - last_cron >= CRON_INTERVAL_MS * 1000LL) {
            cron();
            last_cron = mono_us();
        }

        /* Data goes to disk first, then replies go to clients. */
        aof_before_sleep();
        for (size_t i = 0; i < npending; i++) {
            client_t *c = pending[i];
            c->pending = 0;
            client_flush(c);
        }
        npending = 0;
        free_closed_clients();
    }

    log_msg("shutting down");
    aof_close();
    return 0;
}
