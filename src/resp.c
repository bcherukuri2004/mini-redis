#include "resp.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

static void args_push(args_t *a, const char *p, size_t len) {
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->v = xrealloc(a->v, a->cap * sizeof *a->v);
    }
    a->v[a->n].p = p;
    a->v[a->n].len = len;
    a->n++;
}

void args_free(args_t *a) {
    free(a->v);
    a->v = NULL;
    a->n = a->cap = 0;
}

/* Find the end of a "\r\n" terminated line starting at pos. Returns the
 * index of the '\r', or (size_t)-1 if the line is not complete yet. */
static size_t find_crlf(const char *buf, size_t len, size_t pos, size_t maxline) {
    size_t limit = len - pos > maxline ? pos + maxline : len;
    const char *nl = memchr(buf + pos, '\n', limit - pos);
    if (!nl) return (size_t)-1;
    size_t i = (size_t)(nl - buf);
    if (i == pos || buf[i - 1] != '\r') return (size_t)-2; /* bare \n */
    return i - 1;
}

resp_status resp_parse(const char *buf, size_t len, args_t *out, size_t *consumed, const char **err) {
    out->n = 0;
    if (len == 0) return RESP_INCOMPLETE;

    if (buf[0] != '*') {
        /* Inline command, e.g. typed into telnet: words separated by spaces. */
        const char *nl = memchr(buf, '\n', len > RESP_MAX_INLINE ? RESP_MAX_INLINE : len);
        if (!nl) {
            if (len >= RESP_MAX_INLINE) {
                *err = "too big inline request";
                return RESP_ERROR;
            }
            return RESP_INCOMPLETE;
        }
        size_t end = (size_t)(nl - buf);
        size_t line_end = (end > 0 && buf[end - 1] == '\r') ? end - 1 : end;
        size_t i = 0;
        while (i < line_end) {
            while (i < line_end && (buf[i] == ' ' || buf[i] == '\t')) i++;
            size_t start = i;
            while (i < line_end && buf[i] != ' ' && buf[i] != '\t') i++;
            if (i > start) args_push(out, buf + start, i - start);
        }
        *consumed = end + 1;
        return RESP_OK;
    }

    size_t pos = 1;
    size_t e = find_crlf(buf, len, pos, 32);
    if (e == (size_t)-1) {
        if (len > 32) {
            *err = "invalid multibulk length";
            return RESP_ERROR;
        }
        return RESP_INCOMPLETE;
    }
    if (e == (size_t)-2) {
        *err = "invalid multibulk length";
        return RESP_ERROR;
    }
    long long count;
    if (!str2ll(buf + pos, e - pos, &count) || count > RESP_MAX_ARGS) {
        *err = "invalid multibulk length";
        return RESP_ERROR;
    }
    pos = e + 2;
    if (count <= 0) {
        *consumed = pos;
        return RESP_OK;
    }

    for (long long i = 0; i < count; i++) {
        if (pos >= len) return RESP_INCOMPLETE;
        if (buf[pos] != '$') {
            *err = "expected '$' for bulk string";
            return RESP_ERROR;
        }
        e = find_crlf(buf, len, pos + 1, 32);
        if (e == (size_t)-1) {
            if (len - pos > 32) {
                *err = "invalid bulk length";
                return RESP_ERROR;
            }
            return RESP_INCOMPLETE;
        }
        if (e == (size_t)-2) {
            *err = "invalid bulk length";
            return RESP_ERROR;
        }
        long long blen;
        if (!str2ll(buf + pos + 1, e - (pos + 1), &blen) || blen < 0 || blen > RESP_MAX_BULK) {
            *err = "invalid bulk length";
            return RESP_ERROR;
        }
        size_t data = e + 2;
        if (len - data < (size_t)blen + 2) return RESP_INCOMPLETE;
        if (buf[data + blen] != '\r' || buf[data + blen + 1] != '\n') {
            *err = "bulk string not terminated by CRLF";
            return RESP_ERROR;
        }
        args_push(out, buf + data, (size_t)blen);
        pos = data + (size_t)blen + 2;
    }
    *consumed = pos;
    return RESP_OK;
}

void resp_encode(buf_t *b, const arg_t *v, size_t n) {
    buf_appendf(b, "*%zu\r\n", n);
    for (size_t i = 0; i < n; i++) {
        buf_appendf(b, "$%zu\r\n", v[i].len);
        buf_append(b, v[i].p, v[i].len);
        buf_append(b, "\r\n", 2);
    }
}
