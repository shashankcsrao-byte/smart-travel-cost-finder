/*
 * cache.h - a small time-limited LRU cache, safe to share between server threads.
 *
 * Why: every Ola Maps call uses quota and adds latency. Repeating the same search
 * within a short time can reuse the previous answer.
 *
 * Data structure: a hash map (key -> node) plus a doubly linked list in recency order.
 *   get / set are O(1); the least recently used entry is evicted when the cache is full;
 *   entries older than ttl_seconds count as missing; a mutex makes each call atomic.
 * Values are strings; get() returns a fresh copy the caller frees, so another thread
 * evicting the entry can never pull it out from under you.
 */
#ifndef CACHE_H
#define CACHE_H

#include <pthread.h>

#include "strmap.h"

typedef struct CacheNode {
    char *key, *value;
    double expires;
    struct CacheNode *prev, *next;
} CacheNode;

typedef struct {
    int max_items;
    double ttl;
    StrMap *map;
    CacheNode *head, *tail;      /* head = most recently used */
    pthread_mutex_t lock;
} TTLCache;

TTLCache *cache_new(int max_items, double ttl_seconds);
void cache_free(TTLCache *c);
char *cache_get(TTLCache *c, const char *key);          /* NULL if missing or expired */
void cache_set(TTLCache *c, const char *key, const char *value);

#endif
