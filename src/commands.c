#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "server.h"

/* ---------- replies ---------- */

void reply_simple(client_t *c, const char *s) { buf_appendf(&c->out, "+%s\r\n", s); }
void reply_error(client_t *c, const char *s) { buf_appendf(&c->out, "-%s\r\n", s); }
void reply_int(client_t *c, long long n) { buf_appendf(&c->out, ":%lld\r\n", n); }
void reply_null(client_t *c) { buf_append(&c->out, "$-1\r\n", 5); }
void reply_array(client_t *c, size_t n) { buf_appendf(&c->out, "*%zu\r\n", n); }
void reply_bulk(client_t *c, const char *p, size_t len) {
    buf_appendf(&c->out, "$%zu\r\n", len);
    buf_append(&c->out, p, len);
    buf_append(&c->out, "\r\n", 2);
}

static void reply_wrong_args(client_t *c, const arg_t *name) {
    buf_appendf(&c->out, "-ERR wrong number of arguments for '");
    for (size_t i = 0; i < name->len; i++) {
        char ch = name->p[i];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        buf_append(&c->out, &ch, 1);
    }
    buf_append(&c->out, "' command\r\n", 11);
}

#define ERR_NOT_INT "ERR value is not an integer or out of range"
#define ERR_SYNTAX "ERR syntax error"

/* ---------- helpers ---------- */

static int opt_is(const arg_t *a, const char *name) {
    size_t n = strlen(name);
    return a->len == n && strncasecmp(a->p, name, n) == 0;
}

/* Find a key, deleting it first if it has expired (lazy expiry). While the
 * append only file is being replayed nothing expires, so the replay sees
 * exactly the state the original run saw. */
static entry_t *lookup_key(const char *k, size_t kl) {
    entry_t *e = dict_find(server.db, k, kl);
    if (!e) return NULL;
    if (e->expire >= 0 && !server.loading && e->expire <= now_ms()) {
        arg_t del[2] = {{"DEL", 3}, {k, kl}};
        aof_feed(del, 2);
        dict_remove(server.db, k, kl);
        return NULL;
    }
    return e;
}

static void feed_pexpireat(const arg_t *key, long long at) {
    char nb[32];
    int nl = snprintf(nb, sizeof nb, "%lld", at);
    arg_t v[3] = {{"PEXPIREAT", 9}, *key, {nb, (size_t)nl}};
    aof_feed(v, 3);
}

/* ---------- connection and server commands ---------- */

static void cmd_ping(client_t *c, args_t *a) {
    if (a->n > 2) return reply_wrong_args(c, &a->v[0]);
    if (a->n == 2) reply_bulk(c, a->v[1].p, a->v[1].len);
    else reply_simple(c, "PONG");
}

static void cmd_echo(client_t *c, args_t *a) { reply_bulk(c, a->v[1].p, a->v[1].len); }

static void cmd_quit(client_t *c, args_t *a) {
    (void)a;
    reply_simple(c, "OK");
    c->close_after_reply = 1;
}

static void cmd_select(client_t *c, args_t *a) {
    long long n;
    if (!str2ll(a->v[1].p, a->v[1].len, &n)) return reply_error(c, ERR_NOT_INT);
    if (n != 0) return reply_error(c, "ERR DB index is out of range");
    reply_simple(c, "OK");
}

/* The next three exist only so redis-cli, redis-benchmark and client
 * libraries can connect without errors. */
static void cmd_command(client_t *c, args_t *a) {
    (void)a;
    reply_array(c, 0);
}

static void cmd_config(client_t *c, args_t *a) {
    if (opt_is(&a->v[1], "GET")) reply_array(c, 0);
    else reply_error(c, "ERR unsupported CONFIG subcommand");
}

static void cmd_client(client_t *c, args_t *a) {
    (void)a;
    reply_simple(c, "OK");
}

static void cmd_info(client_t *c, args_t *a) {
    (void)a;
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "# Server\r\nmini_redis_version:%s\r\nprocess_id:%d\r\ntcp_port:%d\r\n", MINI_VERSION,
                (int)getpid(), server.port);
    buf_appendf(&b, "uptime_in_seconds:%lld\r\n\r\n", (now_ms() - server.start_ms) / 1000);
    buf_appendf(&b, "# Clients\r\nconnected_clients:%d\r\n\r\n", server.nclients);
    buf_appendf(&b, "# Stats\r\ntotal_connections_received:%llu\r\ntotal_commands_processed:%llu\r\n\r\n",
                server.total_conns, server.total_cmds);
    buf_appendf(&b, "# Persistence\r\naof_enabled:%d\r\naof_current_size:%lld\r\n\r\n", server.aof_fd >= 0,
                server.aof_size);
    buf_appendf(&b, "# Keyspace\r\ndb0:keys=%zu\r\n", server.db->used);
    reply_bulk(c, buf_ptr(&b), buf_used(&b));
    buf_free(&b);
}

static void cmd_rewriteaof(client_t *c, args_t *a) {
    (void)a;
    if (server.aof_fd < 0) return reply_error(c, "ERR append only file is not enabled");
    if (aof_rewrite() != 0) return reply_error(c, "ERR append only file rewrite failed");
    reply_simple(c, "OK");
}

/* ---------- strings ---------- */

static void cmd_get(client_t *c, args_t *a) {
    entry_t *e = lookup_key(a->v[1].p, a->v[1].len);
    if (!e) reply_null(c);
    else reply_bulk(c, e->val, e->vlen);
}

static void cmd_set(client_t *c, args_t *a) {
    const arg_t *k = &a->v[1], *v = &a->v[2];
    long long expire_at = -1;
    int nx = 0, xx = 0, keepttl = 0, have_exp = 0;
    const arg_t *exp_opt = NULL, *exp_val = NULL;

    /* Check the shape of every option first; only then look at the number.
     * Real Redis reports a syntax error before a bad number, and so do we. */
    for (size_t i = 3; i < a->n; i++) {
        const arg_t *o = &a->v[i];
        if (opt_is(o, "NX") && !xx) {
            nx = 1;
        } else if (opt_is(o, "XX") && !nx) {
            xx = 1;
        } else if (opt_is(o, "KEEPTTL") && !have_exp) {
            keepttl = 1;
        } else if ((opt_is(o, "EX") || opt_is(o, "PX") || opt_is(o, "EXAT") || opt_is(o, "PXAT")) && !keepttl &&
                   !have_exp && i + 1 < a->n) {
            have_exp = 1;
            exp_opt = o;
            exp_val = &a->v[++i];
        } else {
            return reply_error(c, ERR_SYNTAX);
        }
    }
    if (have_exp) {
        long long n;
        if (!str2ll(exp_val->p, exp_val->len, &n)) return reply_error(c, ERR_NOT_INT);
        int seconds = opt_is(exp_opt, "EX") || opt_is(exp_opt, "EXAT");
        int absolute = opt_is(exp_opt, "EXAT") || opt_is(exp_opt, "PXAT");
        if (n <= 0 || (seconds && n > LLONG_MAX / 1000)) return reply_error(c, "ERR invalid expire time in 'set' command");
        if (seconds) n *= 1000;
        if (!absolute) {
            long long now = now_ms();
            if (n > LLONG_MAX - now) return reply_error(c, "ERR invalid expire time in 'set' command");
            n += now;
        }
        expire_at = n;
    }

    entry_t *e = lookup_key(k->p, k->len);
    if ((nx && e) || (xx && !e)) return reply_null(c);

    /* An absolute expiry already in the past still stores the key, which
     * then disappears on first access. Real Redis 7.0 does the same. */
    e = dict_set(server.db, k->p, k->len, v->p, v->len);
    if (!keepttl) e->expire = expire_at;
    reply_simple(c, "OK");

    /* Log a form that replays the same way no matter when it is replayed. */
    char nb[32];
    if (have_exp) {
        int nl = snprintf(nb, sizeof nb, "%lld", expire_at);
        arg_t out[5] = {{"SET", 3}, *k, *v, {"PXAT", 4}, {nb, (size_t)nl}};
        aof_feed(out, 5);
    } else if (keepttl) {
        arg_t out[4] = {{"SET", 3}, *k, *v, {"KEEPTTL", 7}};
        aof_feed(out, 4);
    } else {
        arg_t out[3] = {{"SET", 3}, *k, *v};
        aof_feed(out, 3);
    }
}

static void cmd_mset(client_t *c, args_t *a) {
    if ((a->n - 1) % 2 != 0) return reply_wrong_args(c, &a->v[0]);
    for (size_t i = 1; i < a->n; i += 2) {
        entry_t *e = dict_set(server.db, a->v[i].p, a->v[i].len, a->v[i + 1].p, a->v[i + 1].len);
        e->expire = -1;
    }
    reply_simple(c, "OK");
    aof_feed(a->v, a->n);
}

static void cmd_mget(client_t *c, args_t *a) {
    reply_array(c, a->n - 1);
    for (size_t i = 1; i < a->n; i++) {
        entry_t *e = lookup_key(a->v[i].p, a->v[i].len);
        if (e) reply_bulk(c, e->val, e->vlen);
        else reply_null(c);
    }
}

static void cmd_del(client_t *c, args_t *a) {
    long long n = 0;
    for (size_t i = 1; i < a->n; i++) {
        if (lookup_key(a->v[i].p, a->v[i].len) && dict_remove(server.db, a->v[i].p, a->v[i].len)) {
            n++;
            arg_t out[2] = {{"DEL", 3}, a->v[i]};
            aof_feed(out, 2);
        }
    }
    reply_int(c, n);
}

static void cmd_exists(client_t *c, args_t *a) {
    long long n = 0;
    for (size_t i = 1; i < a->n; i++)
        if (lookup_key(a->v[i].p, a->v[i].len)) n++;
    reply_int(c, n);
}

static void cmd_type(client_t *c, args_t *a) {
    reply_simple(c, lookup_key(a->v[1].p, a->v[1].len) ? "string" : "none");
}

static void incr_generic(client_t *c, const arg_t *k, long long delta, const char *overflow_msg) {
    entry_t *e = lookup_key(k->p, k->len);
    long long cur = 0;
    if (e && !str2ll(e->val, e->vlen, &cur)) return reply_error(c, ERR_NOT_INT);
    long long nv;
    if (__builtin_add_overflow(cur, delta, &nv)) return reply_error(c, overflow_msg);
    char nb[32];
    int nl = snprintf(nb, sizeof nb, "%lld", nv);
    dict_set(server.db, k->p, k->len, nb, (size_t)nl); /* keeps any existing TTL */
    reply_int(c, nv);
    /* Log the result, not the increment, so replaying twice is harmless. */
    arg_t out[4] = {{"SET", 3}, *k, {nb, (size_t)nl}, {"KEEPTTL", 7}};
    aof_feed(out, 4);
}

static void cmd_incr(client_t *c, args_t *a) { incr_generic(c, &a->v[1], 1, "ERR increment or decrement would overflow"); }
static void cmd_decr(client_t *c, args_t *a) { incr_generic(c, &a->v[1], -1, "ERR increment or decrement would overflow"); }

static void cmd_incrby(client_t *c, args_t *a) {
    long long d;
    if (!str2ll(a->v[2].p, a->v[2].len, &d)) return reply_error(c, ERR_NOT_INT);
    incr_generic(c, &a->v[1], d, "ERR increment or decrement would overflow");
}

static void cmd_decrby(client_t *c, args_t *a) {
    long long d;
    if (!str2ll(a->v[2].p, a->v[2].len, &d)) return reply_error(c, ERR_NOT_INT);
    if (d == LLONG_MIN) return reply_error(c, "ERR decrement would overflow");
    incr_generic(c, &a->v[1], -d, "ERR increment or decrement would overflow");
}

/* ---------- expiry ---------- */

/* EXPIRE, PEXPIRE, EXPIREAT, PEXPIREAT in one function. */
static void expire_generic(client_t *c, args_t *a, int seconds, int absolute) {
    long long n;
    if (!str2ll(a->v[2].p, a->v[2].len, &n)) return reply_error(c, ERR_NOT_INT);
    const arg_t *k = &a->v[1];
    long long now = now_ms();
    if (seconds) {
        if (n > LLONG_MAX / 1000 || n < LLONG_MIN / 1000) return reply_error(c, "ERR invalid expire time in command");
        n *= 1000;
    }
    if (!absolute) {
        if (__builtin_add_overflow(n, now, &n)) return reply_error(c, "ERR invalid expire time in command");
    }
    entry_t *e = lookup_key(k->p, k->len);
    if (!e) return reply_int(c, 0);
    if (n <= now && !server.loading) {
        dict_remove(server.db, k->p, k->len);
        arg_t del[2] = {{"DEL", 3}, *k};
        aof_feed(del, 2);
        return reply_int(c, 1);
    }
    e->expire = n;
    reply_int(c, 1);
    feed_pexpireat(k, n);
}

static void cmd_expire(client_t *c, args_t *a) { expire_generic(c, a, 1, 0); }
static void cmd_pexpire(client_t *c, args_t *a) { expire_generic(c, a, 0, 0); }
static void cmd_expireat(client_t *c, args_t *a) { expire_generic(c, a, 1, 1); }
static void cmd_pexpireat(client_t *c, args_t *a) { expire_generic(c, a, 0, 1); }

static void ttl_generic(client_t *c, args_t *a, int ms) {
    entry_t *e = lookup_key(a->v[1].p, a->v[1].len);
    if (!e) return reply_int(c, -2);
    if (e->expire < 0) return reply_int(c, -1);
    long long t = e->expire - now_ms();
    if (t < 0) t = 0;
    reply_int(c, ms ? t : (t + 500) / 1000);
}

static void cmd_ttl(client_t *c, args_t *a) { ttl_generic(c, a, 0); }
static void cmd_pttl(client_t *c, args_t *a) { ttl_generic(c, a, 1); }

static void cmd_persist(client_t *c, args_t *a) {
    entry_t *e = lookup_key(a->v[1].p, a->v[1].len);
    if (!e || e->expire < 0) return reply_int(c, 0);
    e->expire = -1;
    reply_int(c, 1);
    arg_t out[2] = {{"PERSIST", 7}, a->v[1]};
    aof_feed(out, 2);
}

/* ---------- keyspace ---------- */

static void cmd_dbsize(client_t *c, args_t *a) {
    (void)a;
    reply_int(c, (long long)server.db->used);
}

static void cmd_flushall(client_t *c, args_t *a) {
    if (a->n > 2 || (a->n == 2 && !opt_is(&a->v[1], "ASYNC") && !opt_is(&a->v[1], "SYNC")))
        return reply_error(c, ERR_SYNTAX);
    dict_clear(server.db);
    reply_simple(c, "OK");
    arg_t out[1] = {{"FLUSHALL", 8}};
    aof_feed(out, 1);
}

static void cmd_keys(client_t *c, args_t *a) {
    const arg_t *pat = &a->v[1];
    long long now = now_ms();
    size_t cap = 64, n = 0;
    entry_t **hits = xmalloc(cap * sizeof *hits);
    for (size_t i = 0; i < server.db->size; i++) {
        for (entry_t *e = server.db->tab[i]; e; e = e->next) {
            if (e->expire >= 0 && e->expire <= now) continue;
            if (!glob_match(pat->p, pat->len, e->key, e->klen)) continue;
            if (n == cap) {
                cap *= 2;
                hits = xrealloc(hits, cap * sizeof *hits);
            }
            hits[n++] = e;
        }
    }
    reply_array(c, n);
    for (size_t i = 0; i < n; i++) reply_bulk(c, hits[i]->key, hits[i]->klen);
    free(hits);
}

/* ---------- command table ---------- */

typedef void (*cmd_fn)(client_t *c, args_t *a);
typedef struct {
    const char *name;
    cmd_fn fn;
    int arity; /* exact count including the name; negative means at least */
} cmd_t;

/* Hot commands first. */
static const cmd_t cmd_list[] = {
    {"GET", cmd_get, 2},           {"SET", cmd_set, -3},         {"DEL", cmd_del, -2},
    {"EXISTS", cmd_exists, -2},    {"INCR", cmd_incr, 2},        {"DECR", cmd_decr, 2},
    {"INCRBY", cmd_incrby, 3},     {"DECRBY", cmd_decrby, 3},    {"MGET", cmd_mget, -2},
    {"MSET", cmd_mset, -3},        {"EXPIRE", cmd_expire, 3},    {"PEXPIRE", cmd_pexpire, 3},
    {"EXPIREAT", cmd_expireat, 3}, {"PEXPIREAT", cmd_pexpireat, 3}, {"TTL", cmd_ttl, 2},
    {"PTTL", cmd_pttl, 2},         {"PERSIST", cmd_persist, 2},  {"TYPE", cmd_type, 2},
    {"KEYS", cmd_keys, 2},         {"DBSIZE", cmd_dbsize, 1},    {"FLUSHALL", cmd_flushall, -1},
    {"PING", cmd_ping, -1},        {"ECHO", cmd_echo, 2},        {"QUIT", cmd_quit, 1},
    {"SELECT", cmd_select, 2},     {"INFO", cmd_info, -1},       {"COMMAND", cmd_command, -1},
    {"CONFIG", cmd_config, -2},    {"CLIENT", cmd_client, -2},   {"REWRITEAOF", cmd_rewriteaof, 1},
};

static dict_t *cmd_table;

void commands_init(void) {
    cmd_table = dict_create();
    for (size_t i = 0; i < sizeof cmd_list / sizeof cmd_list[0]; i++) {
        const cmd_t *p = &cmd_list[i];
        dict_set(cmd_table, p->name, strlen(p->name), (const char *)&p, sizeof p);
    }
}

static const cmd_t *lookup_cmd(const arg_t *name) {
    char up[32];
    if (name->len == 0 || name->len >= sizeof up) return NULL;
    for (size_t i = 0; i < name->len; i++) {
        char ch = name->p[i];
        up[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    }
    entry_t *e = dict_find(cmd_table, up, name->len);
    if (!e) return NULL;
    const cmd_t *p;
    memcpy(&p, e->val, sizeof p);
    return p;
}

void exec_command(client_t *c) {
    args_t *a = &c->args;
    if (a->n == 0) return;
    const cmd_t *cmd = lookup_cmd(&a->v[0]);
    if (!cmd) {
        buf_appendf(&c->out, "-ERR unknown command '");
        buf_append(&c->out, a->v[0].p, a->v[0].len > 64 ? 64 : a->v[0].len);
        buf_append(&c->out, "'\r\n", 3);
        return;
    }
    if ((cmd->arity > 0 && (int)a->n != cmd->arity) || (cmd->arity < 0 && (int)a->n < -cmd->arity)) {
        reply_wrong_args(c, &a->v[0]);
        return;
    }
    server.total_cmds++;
    cmd->fn(c, a);
}
