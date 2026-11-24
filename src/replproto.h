#ifndef REPLPROTO_H
#define REPLPROTO_H

#include <stddef.h>

#include "buf.h"
#include "dict.h"

/* The pieces of replication that do not need a network, kept apart so the
 * unit tests can check them directly. */

/* Write every live key of d into out as SET commands, plus PEXPIREAT for
 * keys with a TTL. Keys whose time has passed at `now` are skipped. When
 * out holds at least `chunk` bytes, flush(out, ud) is called so the caller
 * can write it somewhere; pass flush = NULL to keep everything in out.
 * Returns the number of keys written, or -1 if flush failed. */
long long snapshot_encode(const dict_t *d, long long now, buf_t *out, size_t chunk,
                          int (*flush)(buf_t *out, void *ud), void *ud);

/* The first thing a primary sends back after SYNC is one line:
 *     +FULLSYNC <offset>\r\n
 * Returns 1 and fills *offset and *consumed when a whole valid line is at
 * the front of p, 0 when more bytes are needed, and -1 when the bytes can
 * never become a valid line (for example a real Redis primary, which
 * answers with a binary snapshot instead). */
int parse_fullsync_line(const char *p, size_t len, long long *offset, size_t *consumed);

#define FULLSYNC_MAX_LINE 64

#endif
