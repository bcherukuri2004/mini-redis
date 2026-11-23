#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>

/* Allocation helpers. Running out of memory aborts the process. */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
/* Copy n bytes and add a trailing NUL (so the copy is also a C string). */
char *xmemdup(const void *p, size_t n);

/* Wall clock in milliseconds. Key expiry times are stored in wall clock
 * time because they are written to disk and must survive a restart. */
long long now_ms(void);
/* Monotonic clock in microseconds, for measuring how long work took. */
long long mono_us(void);

/* Strict string to integer parse, same rules as Redis: no spaces, no plus
 * sign, no leading zeros, must fit in a long long. Returns 1 on success. */
int str2ll(const char *s, size_t len, long long *out);

/* Glob match supporting * ? [abc] [a-z] [^x] and backslash escapes. */
int glob_match(const char *pat, size_t plen, const char *s, size_t slen);

void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
