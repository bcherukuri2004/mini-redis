#define _GNU_SOURCE
#include "util.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void oom(void) {
    fputs("fatal: out of memory\n", stderr);
    abort();
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) oom();
    return p;
}

void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p) oom();
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) oom();
    return q;
}

char *xmemdup(const void *p, size_t n) {
    char *q = xmalloc(n + 1);
    if (n) memcpy(q, p, n);
    q[n] = '\0';
    return q;
}

long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

long long mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int str2ll(const char *s, size_t len, long long *out) {
    if (len == 0 || len > 20) return 0;
    size_t i = 0;
    int neg = 0;
    if (s[0] == '-') {
        neg = 1;
        i = 1;
        if (len == 1) return 0;
    }
    if (s[i] == '0') {
        /* Only a bare "0" is allowed: no "00", "-0" or "0x". */
        if (len - i == 1 && !neg) {
            *out = 0;
            return 1;
        }
        return 0;
    }
    unsigned long long v = 0;
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        unsigned d = (unsigned)(s[i] - '0');
        if (v > (ULLONG_MAX - d) / 10) return 0;
        v = v * 10 + d;
    }
    if (neg) {
        if (v > (unsigned long long)LLONG_MAX + 1ULL) return 0;
        *out = (v == (unsigned long long)LLONG_MAX + 1ULL) ? LLONG_MIN : -(long long)v;
    } else {
        if (v > (unsigned long long)LLONG_MAX) return 0;
        *out = (long long)v;
    }
    return 1;
}

int glob_match(const char *p, size_t pl, const char *s, size_t sl) {
    size_t pi = 0, si = 0;
    size_t star_p = (size_t)-1, star_s = 0;
    while (si < sl) {
        if (pi < pl && p[pi] == '*') {
            star_p = pi++;
            star_s = si;
            continue;
        }
        if (pi < pl) {
            int ok = 0;
            size_t adv = 1;
            unsigned char ch = (unsigned char)s[si];
            if (p[pi] == '?') {
                ok = 1;
            } else if (p[pi] == '[') {
                size_t j = pi + 1;
                int neg = 0, match = 0;
                if (j < pl && p[j] == '^') {
                    neg = 1;
                    j++;
                }
                while (j < pl && p[j] != ']') {
                    if (p[j] == '\\' && j + 1 < pl) {
                        j++;
                        if ((unsigned char)p[j] == ch) match = 1;
                        j++;
                    } else if (j + 2 < pl && p[j + 1] == '-' && p[j + 2] != ']') {
                        unsigned char lo = (unsigned char)p[j], hi = (unsigned char)p[j + 2];
                        if (lo > hi) {
                            unsigned char t = lo;
                            lo = hi;
                            hi = t;
                        }
                        if (ch >= lo && ch <= hi) match = 1;
                        j += 3;
                    } else {
                        if ((unsigned char)p[j] == ch) match = 1;
                        j++;
                    }
                }
                ok = (match != neg);
                adv = (j < pl) ? (j - pi + 1) : (j - pi);
            } else if (p[pi] == '\\' && pi + 1 < pl) {
                ok = ((unsigned char)p[pi + 1] == ch);
                adv = 2;
            } else {
                ok = ((unsigned char)p[pi] == ch);
            }
            if (ok) {
                pi += adv;
                si++;
                continue;
            }
        }
        if (star_p != (size_t)-1) {
            pi = star_p + 1;
            si = ++star_s;
            continue;
        }
        return 0;
    }
    while (pi < pl && p[pi] == '*') pi++;
    return pi == pl;
}

void log_msg(const char *fmt, ...) {
    char ts[32];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(ts, sizeof ts, "%d %b %H:%M:%S", &tmv);
    fprintf(stderr, "%s * ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
