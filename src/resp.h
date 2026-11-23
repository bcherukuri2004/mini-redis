#ifndef RESP_H
#define RESP_H

#include <stddef.h>

#include "buf.h"

#define RESP_MAX_BULK (64u * 1024 * 1024) /* largest single argument */
#define RESP_MAX_ARGS (1024 * 1024)       /* most arguments in one command */
#define RESP_MAX_INLINE (64 * 1024)       /* longest telnet style line */

typedef struct {
    const char *p;
    size_t len;
} arg_t;

/* A parsed command. The pointers in v point into the input buffer, so they
 * are only valid until that buffer is consumed. */
typedef struct {
    arg_t *v;
    size_t n, cap;
} args_t;

typedef enum { RESP_OK, RESP_INCOMPLETE, RESP_ERROR } resp_status;

/* Try to read one command from the front of buf. On RESP_OK, *consumed is
 * how many bytes it used (n may be 0 for an empty line). On RESP_INCOMPLETE
 * nothing is consumed and the caller should wait for more bytes. On
 * RESP_ERROR *err explains what was wrong and the connection should close. */
resp_status resp_parse(const char *buf, size_t len, args_t *out, size_t *consumed, const char **err);

/* Append a command in RESP array form (used for the append only file). */
void resp_encode(buf_t *b, const arg_t *v, size_t n);

void args_free(args_t *a);

#endif
