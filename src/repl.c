#define _GNU_SOURCE
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "replproto.h"
#include "server.h"

/* Primary and replica replication.
 *
 * A replica connects to its primary like any client and sends SYNC. The
 * primary answers "+FULLSYNC <offset>", then every key as SET (plus
 * PEXPIREAT for keys with a TTL), then "REPLCONF SNAPSHOTEND". From then on
 * every change the primary makes is sent to the replica in the same form it
 * is written to the append only file.
 *
 * The server is single threaded, so the full copy is built and queued in one
 * step. A write that arrives later is queued after it, never inside it.
 *
 * The replica loads the copy into a separate table and swaps it in only when
 * the copy is complete, so its own clients see the old data until then,
 * never a half loaded mix. If the link drops, the replica keeps serving its
 * old data, retries once a second, and does a fresh full copy. */

#define REPL_RETRY_MS 1000
#define REPL_TIMEOUT_MS 15000
#define REPL_PING_MS 1000
#define REPL_ACK_MS 1000

static buf_t feed_buf;

/* ---------- primary side ---------- */

static void replica_remove(client_t *c) {
    for (size_t i = 0; i < server.nreplicas; i++) {
        if (server.replicas[i] == c) {
            server.replicas[i] = server.replicas[--server.nreplicas];
            return;
        }
    }
}

/* Send one change to every replica. Nothing is encoded when there are no
 * replicas, so a server without replicas pays nothing for this. */
void repl_feed(const arg_t *v, size_t n) {
    if (server.nreplicas == 0) return;
    buf_consume(&feed_buf, buf_used(&feed_buf));
    resp_encode(&feed_buf, v, n);
    size_t len = buf_used(&feed_buf);
    server.repl_offset += (long long)len;
    for (size_t i = 0; i < server.nreplicas;) {
        client_t *r = server.replicas[i];
        buf_append(&r->out, buf_ptr(&feed_buf), len);
        if (buf_used(&r->out) > server.replbuf_limit) {
            log_msg("replica on fd %d fell too far behind (%zu bytes unsent), dropping it", r->fd, buf_used(&r->out));
            server.replicas_dropped_slow++;
            client_close(r); /* removes it from the list, so do not advance i */
            continue;
        }
        add_pending(r);
        i++;
    }
}

void repl_start_sync(client_t *c) {
    long long t0 = mono_us();
    buf_appendf(&c->out, "+FULLSYNC %lld\r\n", server.repl_offset);
    long long keys = snapshot_encode(server.db, now_ms(), &c->out, 0, NULL, NULL);
    arg_t end[2] = {{"REPLCONF", 8}, {"SNAPSHOTEND", 11}};
    resp_encode(&c->out, end, 2);
    server.last_snapshot_us = mono_us() - t0;
    server.last_snapshot_keys = keys;

    c->is_replica = 1;
    c->ack_offset = server.repl_offset;
    c->ack_ms = now_ms();
    if (server.nreplicas == server.cap_replicas) {
        server.cap_replicas = server.cap_replicas ? server.cap_replicas * 2 : 4;
        server.replicas = xrealloc(server.replicas, server.cap_replicas * sizeof *server.replicas);
    }
    server.replicas[server.nreplicas++] = c;
    log_msg("replica on fd %d: full copy of %lld keys, %zu bytes, built in %lld us", c->fd, keys, buf_used(&c->out),
            server.last_snapshot_us);
}

/* ---------- replica side ---------- */

static void repl_connect(void) {
    server.repl_attempt_ms = now_ms();
    char port[16];
    snprintf(port, sizeof port, "%d", server.primary_port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(server.primary_host, port, &hints, &res);
    if (rc != 0 || !res) {
        log_msg("cannot resolve primary %s: %s", server.primary_host, gai_strerror(rc));
        return;
    }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        freeaddrinfo(res);
        return;
    }
    int r = connect(fd, res->ai_addr, res->ai_addrlen);
    int e = errno;
    freeaddrinfo(res);
    if (r != 0 && e != EINPROGRESS) {
        log_msg("cannot connect to primary %s:%d: %s", server.primary_host, server.primary_port, strerror(e));
        close(fd);
        return;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    client_t *c = client_create(fd);
    c->is_primary = 1;
    server.primary = c;
    server.repl_state = REPL_CONNECTING;
    server.repl_last_io_ms = now_ms();
    client_watch_write(c);
}

void repl_on_writable(client_t *c) {
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        log_msg("cannot connect to primary %s:%d: %s", server.primary_host, server.primary_port, strerror(err));
        client_close(c);
        return;
    }
    server.repl_state = REPL_HANDSHAKE;
    server.sync_start_us = mono_us();
    server.repl_last_io_ms = now_ms();
    arg_t sync[1] = {{"SYNC", 4}};
    resp_encode(&c->out, sync, 1);
    add_pending(c);
    log_msg("connected to primary %s:%d, asking for a full copy", server.primary_host, server.primary_port);
}

static void send_ack(void) {
    if (server.repl_state != REPL_ONLINE || !server.primary) return;
    char nb[32];
    int nl = snprintf(nb, sizeof nb, "%lld", server.replica_offset);
    arg_t ack[3] = {{"REPLCONF", 8}, {"ACK", 3}, {nb, (size_t)nl}};
    resp_encode(&server.primary->out, ack, 3);
    add_pending(server.primary);
}

static void finish_sync(void) {
    dict_destroy(server.db);
    server.db = server.sync_db;
    server.sync_db = NULL;
    server.repl_state = REPL_ONLINE;
    server.last_sync_us = mono_us() - server.sync_start_us;
    server.last_sync_keys = (long long)server.db->used;
    server.full_syncs++;
    /* The copy was not logged as it arrived; write the log from it now. */
    if (server.aof_fd >= 0 && aof_rewrite() != 0) {
        log_msg("fatal: cannot write the append only file after a full sync");
        exit(1);
    }
    log_msg("full sync done: %lld keys in %lld us", server.last_sync_keys, server.last_sync_us);
    send_ack();
}

static int is_snapshot_end(const args_t *a) {
    return a->n == 2 && a->v[0].len == 8 && !memcmp(a->v[0].p, "REPLCONF", 8) && a->v[1].len == 11 &&
           !memcmp(a->v[1].p, "SNAPSHOTEND", 11);
}

void repl_process_link(client_t *c) {
    server.repl_last_io_ms = now_ms();
    if (server.repl_state == REPL_HANDSHAKE) {
        long long off;
        size_t used;
        int r = parse_fullsync_line(buf_ptr(&c->in), buf_used(&c->in), &off, &used);
        if (r == 0) return;
        if (r < 0) {
            size_t show = buf_used(&c->in) < 40 ? buf_used(&c->in) : 40;
            char seen[41];
            for (size_t i = 0; i < show; i++) {
                char ch = buf_ptr(&c->in)[i];
                seen[i] = (ch >= 32 && ch < 127) ? ch : '.';
            }
            seen[show] = '\0';
            log_msg("primary %s:%d did not answer SYNC as mini redis does (got \"%s\"), will retry",
                    server.primary_host, server.primary_port, seen);
            client_close(c);
            return;
        }
        buf_consume(&c->in, used);
        server.repl_state = REPL_LOADING;
        server.sync_db = dict_create();
        server.replica_offset = off;
    }

    while (!c->closing && buf_used(&c->in) > 0 &&
           (server.repl_state == REPL_LOADING || server.repl_state == REPL_ONLINE)) {
        size_t consumed;
        const char *err = NULL;
        resp_status st = resp_parse(buf_ptr(&c->in), buf_used(&c->in), &c->args, &consumed, &err);
        if (st == RESP_INCOMPLETE) break;
        if (st == RESP_ERROR) {
            log_msg("bad data from primary: %s", err);
            client_close(c);
            return;
        }
        if (server.repl_state == REPL_LOADING && is_snapshot_end(&c->args)) {
            buf_consume(&c->in, consumed);
            finish_sync();
            continue;
        }
        int loading = server.repl_state == REPL_LOADING;
        dict_t *live = server.db;
        if (loading) server.db = server.sync_db;
        size_t mark = c->out.len;
        server.applying = 1;
        exec_command(c);
        server.applying = 0;
        if (c->out.len > mark && c->out.data[mark] == '-')
            log_msg("command from primary failed on this replica: %.*s", (int)(c->out.len - mark - 2),
                    c->out.data + mark);
        c->out.len = mark; /* replies to the primary's commands are dropped */
        if (loading) server.db = live;
        else server.replica_offset += (long long)consumed;
        buf_consume(&c->in, consumed);
    }
    if (c->closing) return;
    if (buf_used(&c->in) > RESP_MAX_BULK + (1u << 20)) {
        client_close(c);
        return;
    }
    buf_shrink_if_idle(&c->in, 256 * 1024);
    if (buf_used(&c->out) > 0) add_pending(c);
}

int repl_set_primary(const char *host, int port) {
    if (server.primary) client_close(server.primary);
    free(server.primary_host);
    server.primary_host = xmemdup(host, strlen(host));
    server.primary_port = port;
    server.repl_state = REPL_DISCONNECTED;
    log_msg("now a replica of %s:%d", host, port);
    repl_connect();
    return 0;
}

void repl_set_none(void) {
    if (server.repl_state == REPL_NONE) return;
    server.repl_state = REPL_NONE; /* first, so closing the link does not schedule a retry */
    if (server.primary) client_close(server.primary);
    free(server.primary_host);
    server.primary_host = NULL;
    server.primary_port = 0;
    log_msg("no longer a replica; keeping the data and accepting writes");
}

void repl_on_close(client_t *c) {
    if (c->is_replica) {
        replica_remove(c);
        log_msg("replica on fd %d disconnected", c->fd);
        return;
    }
    if (server.primary == c) server.primary = NULL;
    if (server.sync_db) {
        dict_destroy(server.sync_db);
        server.sync_db = NULL;
        log_msg("full sync interrupted; keeping the data we had before it");
    }
    if (server.repl_state != REPL_NONE) {
        server.repl_state = REPL_DISCONNECTED;
        log_msg("lost the connection to primary %s:%d, retrying every %d ms", server.primary_host,
                server.primary_port, REPL_RETRY_MS);
    }
}

void repl_cron(void) {
    static long long last_ping, last_ack;
    long long now = now_ms();

    /* Primary: a regular PING lets replicas notice a link that went silent. */
    if (server.nreplicas > 0 && now - last_ping >= REPL_PING_MS) {
        last_ping = now;
        arg_t ping[1] = {{"PING", 4}};
        repl_feed(ping, 1);
    }

    if (server.repl_state == REPL_NONE) return;
    if (server.repl_state == REPL_DISCONNECTED) {
        if (now - server.repl_attempt_ms >= REPL_RETRY_MS) repl_connect();
        return;
    }
    if (server.primary && now - server.repl_last_io_ms > REPL_TIMEOUT_MS) {
        log_msg("nothing from primary for %d ms, reconnecting", REPL_TIMEOUT_MS);
        client_close(server.primary);
        return;
    }
    if (server.repl_state == REPL_ONLINE && now - last_ack >= REPL_ACK_MS) {
        last_ack = now;
        send_ack();
    }
}

void repl_info(buf_t *b) {
    buf_appendf(b, "# Replication\r\n");
    if (server.repl_state == REPL_NONE) {
        long long now = now_ms();
        buf_appendf(b, "role:master\r\nconnected_slaves:%zu\r\n", server.nreplicas);
        for (size_t i = 0; i < server.nreplicas; i++) {
            client_t *r = server.replicas[i];
            buf_appendf(b, "slave%zu:fd=%d,state=online,offset=%lld,lag=%lld,unsent_bytes=%zu\r\n", i, r->fd,
                        r->ack_offset, (now - r->ack_ms) / 1000, buf_used(&r->out));
        }
        buf_appendf(b,
                    "master_repl_offset:%lld\r\nlast_snapshot_keys:%lld\r\nlast_snapshot_usec:%lld\r\n"
                    "replicas_dropped_slow:%llu\r\n\r\n",
                    server.repl_offset, server.last_snapshot_keys, server.last_snapshot_us,
                    server.replicas_dropped_slow);
        return;
    }
    int st = server.repl_state;
    buf_appendf(b, "role:slave\r\nmaster_host:%s\r\nmaster_port:%d\r\nmaster_link_status:%s\r\n",
                server.primary_host, server.primary_port, st == REPL_ONLINE ? "up" : "down");
    buf_appendf(b, "master_last_io_seconds_ago:%lld\r\nmaster_sync_in_progress:%d\r\n",
                server.primary ? (now_ms() - server.repl_last_io_ms) / 1000 : -1,
                st == REPL_HANDSHAKE || st == REPL_LOADING);
    buf_appendf(b,
                "slave_repl_offset:%lld\r\nslave_read_only:1\r\nfull_syncs_completed:%llu\r\n"
                "last_full_sync_usec:%lld\r\nlast_full_sync_keys:%lld\r\n\r\n",
                server.replica_offset, server.full_syncs, server.last_sync_us, server.last_sync_keys);
}
