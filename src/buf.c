#include "buf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

void buf_init(buf_t *b) { memset(b, 0, sizeof *b); }

void buf_free(buf_t *b) {
    free(b->data);
    memset(b, 0, sizeof *b);
}

void buf_reserve(buf_t *b, size_t extra) {
    if (b->cap - b->len >= extra) return;
    /* Reclaim consumed space at the front before growing. */
    if (b->off > 0) {
        memmove(b->data, b->data + b->off, b->len - b->off);
        b->len -= b->off;
        b->off = 0;
        if (b->cap - b->len >= extra) return;
    }
    size_t need = b->len + extra;
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < need) ncap *= 2;
    b->data = xrealloc(b->data, ncap);
    b->cap = ncap;
}

void buf_append(buf_t *b, const void *p, size_t n) {
    if (n == 0) return;
    buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

void buf_appendf(buf_t *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[128];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    if ((size_t)n < sizeof tmp) {
        buf_append(b, tmp, (size_t)n);
    } else {
        buf_reserve(b, (size_t)n + 1);
        vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
}

void buf_consume(buf_t *b, size_t n) {
    b->off += n;
    if (b->off >= b->len) {
        b->off = b->len = 0;
    }
}

void buf_shrink_if_idle(buf_t *b, size_t threshold) {
    if (b->len == b->off && b->cap > threshold) {
        free(b->data);
        b->data = NULL;
        b->cap = b->off = b->len = 0;
    }
}
