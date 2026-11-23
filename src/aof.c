#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "server.h"

/* The append only file (AOF) is a log of every command that changed data,
 * in the same wire format clients use. On startup the server replays it to
 * rebuild the dataset. */

static int write_all(int fd, const char *p, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

void aof_feed(const arg_t *v, size_t n) {
    if (server.aof_fd < 0 || server.loading) return;
    resp_encode(&server.aof_buf, v, n);
}

/* Replay the file through the normal command code using a fake client with
 * no socket. A command cut off at the very end (a crash in the middle of a
 * write) is dropped and the file is trimmed. Anything else that fails to
 * parse or run means the file is damaged and we refuse to start. */
static int aof_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) return 0;
        log_msg("cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    client_t c;
    memset(&c, 0, sizeof c);
    c.fd = -1;
    server.loading = 1;

    size_t good = 0;
    unsigned long long cmds = 0;
    int eof = 0, rc = 0;
    for (;;) {
        for (;;) {
            size_t used = buf_used(&c.in);
            if (used == 0) break;
            size_t consumed;
            const char *err = NULL;
            resp_status st = resp_parse(buf_ptr(&c.in), used, &c.args, &consumed, &err);
            if (st == RESP_INCOMPLETE) break;
            if (st == RESP_ERROR) {
                log_msg("append only file is corrupt near byte %zu: %s", good, err);
                rc = -1;
                goto done;
            }
            exec_command(&c);
            if (buf_used(&c.out) > 0 && *buf_ptr(&c.out) == '-') {
                log_msg("append only file has a command that failed near byte %zu", good);
                rc = -1;
                goto done;
            }
            buf_consume(&c.out, buf_used(&c.out));
            buf_consume(&c.in, consumed);
            good += consumed;
            cmds++;
        }
        if (eof) break;
        buf_reserve(&c.in, 1 << 20);
        ssize_t n = read(fd, c.in.data + c.in.len, c.in.cap - c.in.len);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_msg("read error on %s: %s", path, strerror(errno));
            rc = -1;
            goto done;
        }
        if (n == 0) eof = 1;
        else c.in.len += (size_t)n;
    }
    if (buf_used(&c.in) > 0) {
        log_msg("append only file ends with a half written command, dropping %zu bytes", buf_used(&c.in));
        if (truncate(path, (off_t)good) != 0) {
            log_msg("could not truncate %s: %s", path, strerror(errno));
            rc = -1;
        }
    }
    server.aof_size = (long long)good;
    log_msg("replayed %llu commands from %s", cmds, path);

done:
    server.loading = 0;
    close(fd);
    buf_free(&c.in);
    buf_free(&c.out);
    args_free(&c.args);
    return rc;
}

int aof_open(const char *path) {
    server.aof_path = path;
    if (aof_load(path) != 0) return -1;
    server.aof_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (server.aof_fd < 0) {
        log_msg("cannot open %s for writing: %s", path, strerror(errno));
        return -1;
    }
    server.aof_last_fsync_ms = now_ms();
    return 0;
}

/* Called once per event loop pass, before replies go out. With fsync always
 * that ordering means a client only hears "OK" after the data is on disk. */
void aof_before_sleep(void) {
    if (server.aof_fd < 0 || buf_used(&server.aof_buf) == 0) return;
    if (write_all(server.aof_fd, buf_ptr(&server.aof_buf), buf_used(&server.aof_buf)) != 0) {
        /* Carrying on would silently break the durability promise. */
        log_msg("fatal: cannot write to append only file: %s", strerror(errno));
        exit(1);
    }
    server.aof_size += (long long)buf_used(&server.aof_buf);
    buf_consume(&server.aof_buf, buf_used(&server.aof_buf));
    buf_shrink_if_idle(&server.aof_buf, 1 << 20);
    server.aof_dirty = 1;
    if (server.fsync_policy == FSYNC_ALWAYS) {
        fdatasync(server.aof_fd);
        server.aof_dirty = 0;
    }
}

/* With fsync everysec, flush to disk at most once a second. This runs on
 * the main thread, so a slow disk can briefly pause the server. */
void aof_cron(void) {
    if (server.aof_fd < 0 || server.fsync_policy != FSYNC_EVERYSEC || !server.aof_dirty) return;
    long long now = now_ms();
    if (now - server.aof_last_fsync_ms >= 1000) {
        fdatasync(server.aof_fd);
        server.aof_dirty = 0;
        server.aof_last_fsync_ms = now;
    }
}

static int flush_chunk(int fd, buf_t *b) {
    int rc = write_all(fd, buf_ptr(b), buf_used(b));
    buf_consume(b, buf_used(b));
    return rc;
}

/* Shrink the log by writing the current dataset as a fresh file: one SET
 * per key, plus PEXPIREAT for keys with a TTL. The new file is built under
 * a temporary name and renamed over the old one, so a crash at any point
 * leaves either the complete old file or the complete new file. This runs
 * on the main thread and pauses the server while it works. */
int aof_rewrite(void) {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", server.aof_path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (fd < 0) return -1;

    buf_t b;
    buf_init(&b);
    long long now = now_ms(), total = 0;
    int rc = 0;
    for (size_t i = 0; i < server.db->size && rc == 0; i++) {
        for (entry_t *e = server.db->tab[i]; e; e = e->next) {
            if (e->expire >= 0 && e->expire <= now) continue;
            arg_t set[3] = {{"SET", 3}, {e->key, e->klen}, {e->val, e->vlen}};
            resp_encode(&b, set, 3);
            if (e->expire >= 0) {
                char nb[32];
                int nl = snprintf(nb, sizeof nb, "%lld", e->expire);
                arg_t ex[3] = {{"PEXPIREAT", 9}, {e->key, e->klen}, {nb, (size_t)nl}};
                resp_encode(&b, ex, 3);
            }
            if (buf_used(&b) >= (1 << 20)) {
                total += (long long)buf_used(&b);
                if (flush_chunk(fd, &b) != 0) {
                    rc = -1;
                    break;
                }
            }
        }
    }
    if (rc == 0) {
        total += (long long)buf_used(&b);
        rc = flush_chunk(fd, &b);
    }
    buf_free(&b);
    if (rc == 0 && fdatasync(fd) != 0) rc = -1;
    if (rc == 0 && rename(tmp, server.aof_path) != 0) rc = -1;
    if (rc != 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }

    /* Make the rename itself durable. */
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", server.aof_path);
    char *slash = strrchr(dir, '/');
    int dfd = open(slash ? (*slash = '\0', slash == dir ? "/" : dir) : ".", O_RDONLY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }

    close(server.aof_fd);
    server.aof_fd = fd;
    server.aof_size = total;
    server.aof_dirty = 0;
    /* Commands still waiting in the buffer are already part of the snapshot
     * we just wrote. Appending them again would double count things like
     * INCR, so they are thrown away. */
    buf_consume(&server.aof_buf, buf_used(&server.aof_buf));
    return 0;
}

void aof_close(void) {
    if (server.aof_fd < 0) return;
    aof_before_sleep();
    fdatasync(server.aof_fd);
    close(server.aof_fd);
    server.aof_fd = -1;
}
