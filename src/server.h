#ifndef SERVER_H
#define SERVER_H

#include <signal.h>

#include "buf.h"
#include "dict.h"
#include "resp.h"
#include "util.h"

#define MINI_VERSION "0.1.0"
#define MAX_REPLICAS 8 /* each one holds its own full copy while it syncs */

typedef struct client {
    int fd; /* -1 for the fake client used to replay the append only file */
    buf_t in, out;
    args_t args;
    unsigned closing : 1;
    unsigned pending : 1;           /* queued to have its output written */
    unsigned want_write : 1;        /* epoll is watching for writability */
    unsigned close_after_reply : 1; /* QUIT or protocol error */
    unsigned is_replica : 1;        /* on a primary: this connection is a replica */
    unsigned is_primary : 1;        /* on a replica: this connection is our link to the primary */
    long long ack_offset;           /* on a primary: last offset this replica reported */
    long long ack_ms;               /* when it reported it */
    struct client *next_closing;
} client_t;

enum { FSYNC_NO = 0, FSYNC_EVERYSEC = 1, FSYNC_ALWAYS = 2 };

/* Where a replica is in talking to its primary. */
enum {
    REPL_NONE = 0,     /* not a replica */
    REPL_DISCONNECTED, /* a replica, waiting to (re)connect */
    REPL_CONNECTING,   /* TCP connect in progress */
    REPL_HANDSHAKE,    /* sent SYNC, waiting for "+FULLSYNC <offset>" */
    REPL_LOADING,      /* receiving the full copy */
    REPL_ONLINE        /* copy done, applying the live stream */
};

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

    /* replication, primary side */
    client_t **replicas;
    size_t nreplicas, cap_replicas;
    long long repl_offset;          /* bytes of changes sent to replicas so far */
    size_t replbuf_limit;           /* cut off a replica whose unsent output passes this */
    long long last_snapshot_us;     /* time to build the last full copy */
    long long last_snapshot_keys;
    unsigned long long replicas_dropped_slow;

    /* replication, replica side */
    int repl_state;                 /* REPL_NONE when this server is a primary */
    char *primary_host;
    int primary_port;
    client_t *primary;              /* link to the primary, NULL when not connected */
    dict_t *sync_db;                /* the incoming full copy, swapped in when complete */
    long long replica_offset;       /* bytes of the live stream applied so far */
    long long repl_attempt_ms;      /* last connect attempt */
    long long repl_last_io_ms;      /* last byte received from the primary */
    long long sync_start_us;
    long long last_sync_us;         /* time the last full copy took, end to end */
    long long last_sync_keys;
    unsigned long long full_syncs;
    int applying;                   /* running a command that came from the primary */
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

/* server.c, used by replication */
client_t *client_create(int fd);
void client_close(client_t *c);
void add_pending(client_t *c);
void client_watch_write(client_t *c);

/* repl.c */
void repl_feed(const arg_t *v, size_t n);         /* send a change to every replica */
void repl_cron(void);                             /* reconnects, heartbeats, acks */
void repl_on_close(client_t *c);                  /* a replica or primary link closed */
void repl_on_writable(client_t *c);               /* primary link finished connecting */
void repl_process_link(client_t *c);              /* input arrived from our primary */
int repl_set_primary(const char *host, int port); /* become a replica */
void repl_set_none(void);                         /* stop being a replica, keep the data */
void repl_start_sync(client_t *c);                /* primary side of SYNC */
void repl_info(buf_t *b);

#endif
