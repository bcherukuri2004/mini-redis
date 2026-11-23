#define _GNU_SOURCE
#include "dict.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

#define DICT_INITIAL_SIZE 64

/* FNV 1a with a per process random seed and a final mixing step. The seed
 * stops a client from crafting keys that all collide in one bucket. */
static uint64_t hash_bytes(uint64_t seed, const char *k, size_t n) {
    uint64_t h = 1469598103934665603ULL ^ seed;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)k[i];
        h *= 1099511628211ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

dict_t *dict_create(void) {
    dict_t *d = xmalloc(sizeof *d);
    d->size = DICT_INITIAL_SIZE;
    d->used = 0;
    d->tab = xcalloc(d->size, sizeof *d->tab);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    d->seed = (uint64_t)ts.tv_nsec * 0x9e3779b97f4a7c15ULL ^ (uint64_t)getpid() ^ (uint64_t)(uintptr_t)d;
    return d;
}

static void free_entry(entry_t *e) {
    free(e->key);
    free(e->val);
    free(e);
}

static void free_all(dict_t *d) {
    for (size_t i = 0; i < d->size; i++) {
        entry_t *e = d->tab[i];
        while (e) {
            entry_t *next = e->next;
            free_entry(e);
            e = next;
        }
        d->tab[i] = NULL;
    }
    d->used = 0;
}

void dict_destroy(dict_t *d) {
    if (!d) return;
    free_all(d);
    free(d->tab);
    free(d);
}

void dict_clear(dict_t *d) {
    free_all(d);
    free(d->tab);
    d->size = DICT_INITIAL_SIZE;
    d->tab = xcalloc(d->size, sizeof *d->tab);
}

/* Rebuild the table with more buckets. This moves every entry at once, so
 * one insert out of many thousands takes noticeably longer. Real Redis
 * spreads this work out; this project keeps the simple version. */
static void resize(dict_t *d, size_t nsize) {
    entry_t **nt = xcalloc(nsize, sizeof *nt);
    for (size_t i = 0; i < d->size; i++) {
        entry_t *e = d->tab[i];
        while (e) {
            entry_t *next = e->next;
            size_t idx = e->hash & (nsize - 1);
            e->next = nt[idx];
            nt[idx] = e;
            e = next;
        }
    }
    free(d->tab);
    d->tab = nt;
    d->size = nsize;
}

entry_t *dict_find(dict_t *d, const char *key, size_t klen) {
    uint64_t h = hash_bytes(d->seed, key, klen);
    for (entry_t *e = d->tab[h & (d->size - 1)]; e; e = e->next) {
        if (e->hash == h && e->klen == klen && memcmp(e->key, key, klen) == 0) return e;
    }
    return NULL;
}

entry_t *dict_set(dict_t *d, const char *key, size_t klen, const char *val, size_t vlen) {
    uint64_t h = hash_bytes(d->seed, key, klen);
    for (entry_t *e = d->tab[h & (d->size - 1)]; e; e = e->next) {
        if (e->hash == h && e->klen == klen && memcmp(e->key, key, klen) == 0) {
            char *nv = xmemdup(val, vlen);
            free(e->val);
            e->val = nv;
            e->vlen = vlen;
            return e;
        }
    }
    if (d->used >= d->size) resize(d, d->size * 2);
    entry_t *e = xmalloc(sizeof *e);
    e->key = xmemdup(key, klen);
    e->val = xmemdup(val, vlen);
    e->klen = klen;
    e->vlen = vlen;
    e->expire = -1;
    e->hash = h;
    size_t idx = h & (d->size - 1);
    e->next = d->tab[idx];
    d->tab[idx] = e;
    d->used++;
    return e;
}

int dict_remove(dict_t *d, const char *key, size_t klen) {
    uint64_t h = hash_bytes(d->seed, key, klen);
    entry_t **pp = &d->tab[h & (d->size - 1)];
    while (*pp) {
        entry_t *e = *pp;
        if (e->hash == h && e->klen == klen && memcmp(e->key, key, klen) == 0) {
            *pp = e->next;
            free_entry(e);
            d->used--;
            return 1;
        }
        pp = &e->next;
    }
    return 0;
}

size_t dict_sweep_expired(dict_t *d, size_t *cursor, size_t nbuckets, long long now,
                          dict_expired_cb cb, void *ud) {
    size_t removed = 0;
    size_t idx = *cursor;
    if (idx >= d->size) idx = 0;
    for (size_t n = 0; n < nbuckets; n++) {
        entry_t **pp = &d->tab[idx];
        while (*pp) {
            entry_t *e = *pp;
            if (e->expire >= 0 && e->expire <= now) {
                if (cb) cb(e, ud);
                *pp = e->next;
                free_entry(e);
                d->used--;
                removed++;
            } else {
                pp = &e->next;
            }
        }
        idx++;
        if (idx >= d->size) idx = 0;
    }
    *cursor = idx;
    return removed;
}
