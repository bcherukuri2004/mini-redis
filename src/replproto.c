#include "replproto.h"

#include <stdio.h>
#include <string.h>

#include "resp.h"
#include "util.h"

long long snapshot_encode(const dict_t *d, long long now, buf_t *out, size_t chunk,
                          int (*flush)(buf_t *out, void *ud), void *ud) {
    long long keys = 0;
    for (size_t i = 0; i < d->size; i++) {
        for (entry_t *e = d->tab[i]; e; e = e->next) {
            if (e->expire >= 0 && e->expire <= now) continue;
            arg_t set[3] = {{"SET", 3}, {e->key, e->klen}, {e->val, e->vlen}};
            resp_encode(out, set, 3);
            if (e->expire >= 0) {
                char nb[32];
                int nl = snprintf(nb, sizeof nb, "%lld", e->expire);
                arg_t ex[3] = {{"PEXPIREAT", 9}, {e->key, e->klen}, {nb, (size_t)nl}};
                resp_encode(out, ex, 3);
            }
            keys++;
            if (flush && buf_used(out) >= chunk && flush(out, ud) != 0) return -1;
        }
    }
    if (flush && buf_used(out) > 0 && flush(out, ud) != 0) return -1;
    return keys;
}

int parse_fullsync_line(const char *p, size_t len, long long *offset, size_t *consumed) {
    static const char prefix[] = "+FULLSYNC ";
    size_t plen = sizeof prefix - 1;
    /* Check the bytes we have against the prefix before waiting for more,
     * so a wrong reply is rejected at once instead of hanging. */
    size_t cmp = len < plen ? len : plen;
    if (memcmp(p, prefix, cmp) != 0) return -1;
    const char *nl = memchr(p, '\n', len < FULLSYNC_MAX_LINE ? len : FULLSYNC_MAX_LINE);
    if (!nl) return len >= FULLSYNC_MAX_LINE ? -1 : 0;
    size_t line = (size_t)(nl - p);
    if (line < plen + 2 || p[line - 1] != '\r') return -1;
    long long v;
    if (!str2ll(p + plen, line - 1 - plen, &v) || v < 0) return -1;
    *offset = v;
    *consumed = line + 1;
    return 1;
}
