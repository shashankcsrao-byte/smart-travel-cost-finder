/*
 * strmap.c - see strmap.h.
 */
#include "strmap.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

#define FNV_OFFSET 1469598103934665603ULL

unsigned long long fnv1a(const void *data, size_t n, unsigned long long seed) {
    const unsigned char *p = data;
    unsigned long long h = seed ? seed : FNV_OFFSET;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static size_t bucket_of(const StrMap *m, const char *key) {
    return (size_t)(fnv1a(key, strlen(key), 0) % m->nbuckets);
}

StrMap *sm_new(size_t expected) {
    StrMap *m = xmalloc(sizeof *m);
    m->nbuckets = 16;
    while (m->nbuckets < expected * 2) m->nbuckets *= 2;
    m->buckets = xcalloc(m->nbuckets, sizeof *m->buckets);
    m->count = 0;
    return m;
}

void sm_free(StrMap *m, void (*free_value)(void *)) {
    if (!m) return;
    for (size_t i = 0; i < m->nbuckets; i++) {
        SmEntry *e = m->buckets[i];
        while (e) {
            SmEntry *next = e->next;
            if (free_value) free_value(e->value);
            free(e->key);
            free(e);
            e = next;
        }
    }
    free(m->buckets);
    free(m);
}

static SmEntry *find(const StrMap *m, const char *key) {
    for (SmEntry *e = m->buckets[bucket_of(m, key)]; e; e = e->next)
        if (strcmp(e->key, key) == 0) return e;
    return NULL;
}

void *sm_get(const StrMap *m, const char *key) {
    SmEntry *e = find(m, key);
    return e ? e->value : NULL;
}

int sm_has(const StrMap *m, const char *key) { return find(m, key) != NULL; }

static void rehash(StrMap *m) {
    size_t old_n = m->nbuckets;
    SmEntry **old = m->buckets;
    m->nbuckets *= 2;
    m->buckets = xcalloc(m->nbuckets, sizeof *m->buckets);
    for (size_t i = 0; i < old_n; i++) {
        SmEntry *e = old[i];
        while (e) {
            SmEntry *next = e->next;
            size_t b = bucket_of(m, e->key);
            e->next = m->buckets[b];
            m->buckets[b] = e;
            e = next;
        }
    }
    free(old);
}

void sm_put(StrMap *m, const char *key, void *value) {
    SmEntry *e = find(m, key);
    if (e) {
        e->value = value;
        return;
    }
    if (m->count + 1 > m->nbuckets) rehash(m);        /* keep chains short */
    size_t b = bucket_of(m, key);
    e = xmalloc(sizeof *e);
    e->key = xstrdup(key);
    e->value = value;
    e->next = m->buckets[b];
    m->buckets[b] = e;
    m->count++;
}

void *sm_remove(StrMap *m, const char *key) {
    size_t b = bucket_of(m, key);
    SmEntry **link = &m->buckets[b];
    for (SmEntry *e = *link; e; link = &e->next, e = e->next) {
        if (strcmp(e->key, key) == 0) {
            void *value = e->value;
            *link = e->next;
            free(e->key);
            free(e);
            m->count--;
            return value;
        }
    }
    return NULL;
}

size_t sm_count(const StrMap *m) { return m->count; }

SmEntry *sm_next(const StrMap *m, size_t *bucket, SmEntry *prev) {
    if (prev && prev->next) return prev->next;
    if (prev) (*bucket)++;
    for (; *bucket < m->nbuckets; (*bucket)++)
        if (m->buckets[*bucket]) return m->buckets[*bucket];
    return NULL;
}
