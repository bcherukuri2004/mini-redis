/* Unit tests for the building blocks: number parsing, glob matching, the
 * buffer, the hash table and the protocol parser. Built with sanitizers. */
#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "dict.h"
#include "resp.h"
#include "util.h"

static int failures, checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

static int ll(const char *s, long long *out) { return str2ll(s, strlen(s), out); }
static int glob(const char *p, const char *s) { return glob_match(p, strlen(p), s, strlen(s)); }

static void test_str2ll(void) {
    long long v;
    CHECK(ll("0", &v) && v == 0);
    CHECK(ll("123", &v) && v == 123);
    CHECK(ll("-456", &v) && v == -456);
    CHECK(ll("9223372036854775807", &v) && v == LLONG_MAX);
    CHECK(ll("-9223372036854775808", &v) && v == LLONG_MIN);
    CHECK(!ll("9223372036854775808", &v));
    CHECK(!ll("-9223372036854775809", &v));
    CHECK(!ll("", &v));
    CHECK(!ll("-", &v));
    CHECK(!ll("-0", &v));
    CHECK(!ll("00", &v));
    CHECK(!ll("01", &v));
    CHECK(!ll("+1", &v));
    CHECK(!ll(" 1", &v));
    CHECK(!ll("1 ", &v));
    CHECK(!ll("1.5", &v));
    CHECK(!ll("abc", &v));
    CHECK(!ll("99999999999999999999999", &v));
}

static void test_glob(void) {
    CHECK(glob("*", ""));
    CHECK(glob("*", "anything"));
    CHECK(glob("a*", "abc"));
    CHECK(glob("*c", "abc"));
    CHECK(glob("a*c", "abc"));
    CHECK(glob("a*c", "ac"));
    CHECK(!glob("a*c", "ab"));
    CHECK(glob("a?c", "abc"));
    CHECK(!glob("a?c", "ac"));
    CHECK(glob("h[ae]llo", "hello"));
    CHECK(glob("h[ae]llo", "hallo"));
    CHECK(!glob("h[ae]llo", "hillo"));
    CHECK(glob("h[^e]llo", "hallo"));
    CHECK(!glob("h[^e]llo", "hello"));
    CHECK(glob("h[a-c]llo", "hbllo"));
    CHECK(!glob("h[a-c]llo", "hdllo"));
    CHECK(glob("a\\*b", "a*b"));
    CHECK(!glob("a\\*b", "axb"));
    CHECK(glob("*a*b*c*", "xxaxxbxxcxx"));
    CHECK(!glob("*a*b*c*", "xxcxxbxxaxx"));
    CHECK(glob("", ""));
    CHECK(!glob("", "x"));
    /* A pattern full of stars must not take exponential time. */
    char s[64];
    memset(s, 'a', sizeof s - 1);
    s[sizeof s - 1] = '\0';
    CHECK(!glob("*a*a*a*a*a*a*a*a*a*a*a*a*b", s));
}

static void test_buf(void) {
    buf_t b;
    buf_init(&b);
    buf_append(&b, "hello", 5);
    buf_appendf(&b, " %d %s", 42, "world");
    CHECK(buf_used(&b) == 14 && memcmp(buf_ptr(&b), "hello 42 world", 14) == 0);
    buf_consume(&b, 6);
    CHECK(buf_used(&b) == 8 && memcmp(buf_ptr(&b), "42 world", 8) == 0);
    /* Long formatted output goes down the slow path. */
    char big[300];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    buf_appendf(&b, "%s", big);
    CHECK(buf_used(&b) == 8 + 299);
    buf_consume(&b, buf_used(&b));
    CHECK(buf_used(&b) == 0);
    /* Many small appends and consumes should keep working after compaction. */
    for (int i = 0; i < 100000; i++) {
        buf_append(&b, "abcdefgh", 8);
        if (i % 3 == 0) buf_consume(&b, 8);
    }
    CHECK(buf_used(&b) == (size_t)(100000 - 33334) * 8);
    buf_shrink_if_idle(&b, 16);
    CHECK(b.data != NULL); /* not idle, so not freed */
    buf_free(&b);
}

static void count_cb(const entry_t *e, void *ud) {
    (void)e;
    (*(size_t *)ud)++;
}

static void test_dict(void) {
    dict_t *d = dict_create();
    char k[32], v[32];
    const int N = 200000;
    for (int i = 0; i < N; i++) {
        int kl = snprintf(k, sizeof k, "key:%d", i);
        int vl = snprintf(v, sizeof v, "val:%d", i);
        dict_set(d, k, (size_t)kl, v, (size_t)vl);
    }
    CHECK(d->used == (size_t)N);
    CHECK(d->size >= d->used / 2); /* it grew */
    int bad = 0;
    for (int i = 0; i < N; i++) {
        int kl = snprintf(k, sizeof k, "key:%d", i);
        entry_t *e = dict_find(d, k, (size_t)kl);
        int vl = snprintf(v, sizeof v, "val:%d", i);
        if (!e || e->vlen != (size_t)vl || memcmp(e->val, v, (size_t)vl) != 0) bad++;
    }
    CHECK(bad == 0);
    CHECK(dict_find(d, "nope", 4) == NULL);

    /* Replacing a value keeps the expiry; a new key starts without one. */
    entry_t *e = dict_set(d, "key:5", 5, "new", 3);
    CHECK(e->expire == -1);
    e->expire = 12345;
    e = dict_set(d, "key:5", 5, "newer", 5);
    CHECK(e->expire == 12345 && e->vlen == 5 && d->used == (size_t)N);

    /* Binary safe keys and values. */
    dict_set(d, "a\0b", 3, "x\0y", 3);
    e = dict_find(d, "a\0b", 3);
    CHECK(e && e->vlen == 3 && e->val[1] == '\0' && e->val[2] == 'y');
    CHECK(dict_find(d, "a\0c", 3) == NULL);

    /* Delete the even keys. */
    for (int i = 0; i < N; i += 2) {
        int kl = snprintf(k, sizeof k, "key:%d", i);
        CHECK(dict_remove(d, k, (size_t)kl) == 1);
    }
    CHECK(dict_remove(d, "key:0", 5) == 0);
    CHECK(d->used == (size_t)(N / 2) + 1);

    /* Sweep: expire every key ending in 1 in the past, others in the future. */
    size_t expected = 0;
    for (size_t i = 0; i < d->size; i++) {
        for (entry_t *x = d->tab[i]; x; x = x->next) {
            if (x->klen > 4 && x->key[x->klen - 1] == '1') {
                x->expire = 100;
                expected++;
            } else if (x->klen > 4) {
                x->expire = 1000000;
            }
        }
    }
    size_t cursor = 0, seen = 0, removed = 0;
    size_t before = d->used;
    for (size_t round = 0; round < d->size / 64 + 2; round++)
        removed += dict_sweep_expired(d, &cursor, 64, 5000, count_cb, &seen);
    CHECK(removed == expected && seen == expected && d->used == before - expected);
    CHECK(dict_find(d, "key:11", 6) == NULL);
    CHECK(dict_find(d, "key:13", 6) != NULL);

    dict_clear(d);
    CHECK(d->used == 0 && dict_find(d, "key:13", 6) == NULL);
    dict_set(d, "after", 5, "clear", 5);
    CHECK(dict_find(d, "after", 5) != NULL);
    dict_destroy(d);
}

static resp_status parse(const char *s, size_t len, args_t *a, size_t *consumed, const char **err) {
    return resp_parse(s, len, a, consumed, err);
}

static void test_resp(void) {
    args_t a = {0};
    size_t used;
    const char *err = NULL;

    const char *cmd = "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nhello\r\n";
    size_t clen = strlen(cmd);
    CHECK(parse(cmd, clen, &a, &used, &err) == RESP_OK);
    CHECK(used == clen && a.n == 3);
    CHECK(a.v[0].len == 3 && memcmp(a.v[0].p, "SET", 3) == 0);
    CHECK(a.v[2].len == 5 && memcmp(a.v[2].p, "hello", 5) == 0);

    /* Every strict prefix is "incomplete", never an error and never OK. */
    int all_incomplete = 1;
    for (size_t n = 0; n < clen; n++)
        if (parse(cmd, n, &a, &used, &err) != RESP_INCOMPLETE) all_incomplete = 0;
    CHECK(all_incomplete);

    /* Two pipelined commands: the first parse stops after the first. */
    char two[256];
    snprintf(two, sizeof two, "%s*1\r\n$4\r\nPING\r\n", cmd);
    CHECK(parse(two, strlen(two), &a, &used, &err) == RESP_OK && used == clen);
    CHECK(parse(two + used, strlen(two) - used, &a, &used, &err) == RESP_OK && a.n == 1);

    /* Binary data inside a bulk string, including CRLF. */
    const char bin[] = "*2\r\n$3\r\nGET\r\n$6\r\na\r\nb\0c\r\n";
    CHECK(parse(bin, sizeof bin - 1, &a, &used, &err) == RESP_OK && a.v[1].len == 6);

    /* Empty argument and empty array. */
    CHECK(parse("*1\r\n$0\r\n\r\n", 10 + 1, &a, &used, &err) == RESP_OK && a.n == 1 && a.v[0].len == 0);
    CHECK(parse("*0\r\n", 4, &a, &used, &err) == RESP_OK && a.n == 0 && used == 4);

    /* Inline commands. */
    const char *il = "  set   foo  bar \r\n";
    CHECK(parse(il, strlen(il), &a, &used, &err) == RESP_OK && a.n == 3 && used == strlen(il));
    CHECK(a.v[1].len == 3 && memcmp(a.v[1].p, "foo", 3) == 0);
    CHECK(parse("PING\n", 5, &a, &used, &err) == RESP_OK && a.n == 1);
    CHECK(parse("\r\n", 2, &a, &used, &err) == RESP_OK && a.n == 0 && used == 2);
    CHECK(parse("PIN", 3, &a, &used, &err) == RESP_INCOMPLETE);

    /* Malformed input is an error, not a hang or a crash. */
    CHECK(parse("*x\r\n", 4, &a, &used, &err) == RESP_ERROR);
    CHECK(parse("*-5\r\n", 5, &a, &used, &err) == RESP_OK && a.n == 0);
    CHECK(parse("*99999999\r\n", 11, &a, &used, &err) == RESP_ERROR);
    CHECK(parse("*1\r\n+OK\r\n", 9, &a, &used, &err) == RESP_ERROR);
    CHECK(parse("*1\r\n$-1\r\n", 9, &a, &used, &err) == RESP_ERROR);
    CHECK(parse("*1\r\n$abc\r\n", 10, &a, &used, &err) == RESP_ERROR);
    CHECK(parse("*1\r\n$99999999999\r\n", 18, &a, &used, &err) == RESP_ERROR);
    CHECK(parse("*1\r\n$3\r\nabcXY", 13, &a, &used, &err) == RESP_ERROR);
    CHECK(parse("*1\n$3\r\nabc\r\n", 12, &a, &used, &err) == RESP_ERROR);

    /* A header line that never ends must not be buffered forever. */
    char junk[200];
    memset(junk, '1', sizeof junk);
    junk[0] = '*';
    CHECK(parse(junk, sizeof junk, &a, &used, &err) == RESP_ERROR);

    /* Encode then parse gives the same command back. */
    arg_t orig[3] = {{"SET", 3}, {"k\r\ney", 5}, {"", 0}};
    buf_t b;
    buf_init(&b);
    resp_encode(&b, orig, 3);
    CHECK(parse(buf_ptr(&b), buf_used(&b), &a, &used, &err) == RESP_OK && used == buf_used(&b));
    CHECK(a.n == 3 && a.v[1].len == 5 && memcmp(a.v[1].p, "k\r\ney", 5) == 0 && a.v[2].len == 0);
    buf_free(&b);
    args_free(&a);
}

int main(void) {
    test_str2ll();
    test_glob();
    test_buf();
    test_dict();
    test_resp();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
