#ifndef SERVER_H
#define SERVER_H

#include <signal.h>

#include "buf.h"
#include "dict.h"
#include "resp.h"
#include "util.h"

#define MINI_VERSION "0.1.0"

typedef struct client {
    int fd; /* -1 for the fake client used to replay the append only file */
    buf_t in, out;
    args_t args;
    unsigned closing : 1;
    unsigned pending : 1;           /* queued to have its output written */
    unsigned want_write : 1;        /* epoll is watching for writability */
    unsigned close_after_reply : 1; /* QUIT or protocol error */
    struct client *next_closing;
} client_t;

enum { FSYNC_NO = 0, FSYNC_EVERYSEC = 1, FSYNC_ALWAYS = 2 };

typedef struct {
    dict_t *db;
    int epfd, listenfd, port;
    const char *bind_addr;
    volatile sig_atomic_t shutdown;
    long long start_ms;
    unsigned long long total_cmds, total_conns;
    int nclients;
    /* append only file */
    const char *aof_path;
    int aof_fd; /* -1 when persistence is off */
    int fsync_policy;
    int aof_dirty; /* written but not yet fsynced */
    long long aof_last_fsync_ms;
    long long aof_size;
    buf_t aof_buf; /* commands run this loop iteration, not yet written */
    int loading;   /* replaying the append only file at startup */
} server_t;

extern server_t server;

/* commands.c */
void commands_init(void);
void exec_command(client_t *c);
void reply_simple(client_t *c, const char *s);
void reply_error(client_t *c, const char *s);
void reply_int(client_t *c, long long n);
void reply_bulk(client_t *c, const char *p, size_t len);
void reply_null(client_t *c);
void reply_array(client_t *c, size_t n);

/* aof.c */
int aof_open(const char *path); /* replay existing file, then open for append */
void aof_feed(const arg_t *v, size_t n);
void aof_before_sleep(void); /* write buffered commands, fsync if policy says so */
void aof_cron(void);         /* once per cron tick: handles the everysec fsync */
int aof_rewrite(void);
void aof_close(void);

#endif
