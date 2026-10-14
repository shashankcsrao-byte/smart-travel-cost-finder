/*
 * strmap.h - a hash map from strings to pointers (separate chaining).
 *
 * Used wherever the Python code used a dict keyed by text: stations by id,
 * fares by station pair, graph nodes by name, caches and rate limits.
 *   get / put / remove: O(1) on average; the table doubles when it gets full.
 */
#ifndef STRMAP_H
#define STRMAP_H

#include <stddef.h>

typedef struct SmEntry {
    char *key;
    void *value;
    struct SmEntry *next;
} SmEntry;

typedef struct {
    SmEntry **buckets;
    size_t nbuckets, count;
} StrMap;

StrMap *sm_new(size_t expected);
void sm_free(StrMap *m, void (*free_value)(void *));
void *sm_get(const StrMap *m, const char *key);
int sm_has(const StrMap *m, const char *key);
void sm_put(StrMap *m, const char *key, void *value);   /* replaces an existing value */
void *sm_remove(StrMap *m, const char *key);           /* returns the old value */
size_t sm_count(const StrMap *m);

/* Visit every entry: for (SmEntry *e = NULL; (e = sm_next(m, &i, e)); ) ... with size_t i = 0. */
SmEntry *sm_next(const StrMap *m, size_t *bucket, SmEntry *prev);

unsigned long long fnv1a(const void *data, size_t n, unsigned long long seed);

#endif
