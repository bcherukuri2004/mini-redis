#ifndef BUF_H
#define BUF_H

#include <stddef.h>

/* A growable byte buffer with a read cursor. Bytes in [off, len) are
 * unread. Consuming from the front just moves the cursor, so a client that
 * pipelines thousands of commands does not cause thousands of memmoves. */
typedef struct {
    char *data;
    size_t off, len, cap;
} buf_t;

void buf_init(buf_t *b);
void buf_free(buf_t *b);
/* Make sure at least `extra` writable bytes exist after data+len. */
void buf_reserve(buf_t *b, size_t extra);
void buf_append(buf_t *b, const void *p, size_t n);
void buf_appendf(buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Mark n bytes at the front as read. */
void buf_consume(buf_t *b, size_t n);
/* Give memory back after a huge value has passed through. */
void buf_shrink_if_idle(buf_t *b, size_t threshold);

static inline size_t buf_used(const buf_t *b) { return b->len - b->off; }
static inline char *buf_ptr(const buf_t *b) { return b->data + b->off; }

#endif
