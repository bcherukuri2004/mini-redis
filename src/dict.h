#ifndef DICT_H
#define DICT_H

#include <stddef.h>
#include <stdint.h>

/* One key and value, plus an optional expiry. Chained hash table: entries
 * that land in the same bucket form a linked list. */
typedef struct entry {
    struct entry *next;
    char *key;
    char *val;
    size_t klen, vlen;
    long long expire; /* absolute wall clock ms, or -1 for no expiry */
    uint64_t hash;
} entry_t;

typedef struct {
    entry_t **tab;
    size_t size; /* number of buckets, always a power of two */
    size_t used; /* number of entries */
    uint64_t seed;
} dict_t;

dict_t *dict_create(void);
void dict_destroy(dict_t *d);
void dict_clear(dict_t *d);

entry_t *dict_find(dict_t *d, const char *key, size_t klen);
/* Insert the key or replace its value. A new entry starts with expire -1;
 * an existing entry keeps whatever expire it had. Returns the entry. */
entry_t *dict_set(dict_t *d, const char *key, size_t klen, const char *val, size_t vlen);
/* Returns 1 if the key existed and was removed. */
int dict_remove(dict_t *d, const char *key, size_t klen);

typedef void (*dict_expired_cb)(const entry_t *e, void *ud);
/* Look at `nbuckets` buckets starting at *cursor, delete entries whose
 * expire time has passed, and advance the cursor (wrapping). The callback
 * runs just before each deleted entry is freed. Returns how many were
 * deleted. */
size_t dict_sweep_expired(dict_t *d, size_t *cursor, size_t nbuckets, long long now,
                          dict_expired_cb cb, void *ud);

#endif
