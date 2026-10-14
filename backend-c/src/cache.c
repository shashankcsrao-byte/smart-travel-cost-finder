/*
 * cache.c - see cache.h.
 */
#include "cache.h"

#include <stdlib.h>

#include "util.h"

TTLCache *cache_new(int max_items, double ttl_seconds) {
    TTLCache *c = xcalloc(1, sizeof *c);
    c->max_items = max_items;
    c->ttl = ttl_seconds;
    c->map = sm_new((size_t)max_items);
    pthread_mutex_init(&c->lock, NULL);
    return c;
}

static void unlink_node(TTLCache *c, CacheNode *n) {
    if (n->prev) n->prev->next = n->next;
    else c->head = n->next;
    if (n->next) n->next->prev = n->prev;
    else c->tail = n->prev;
    n->prev = n->next = NULL;
}

static void push_front(TTLCache *c, CacheNode *n) {
    n->prev = NULL;
    n->next = c->head;
    if (c->head) c->head->prev = n;
    c->head = n;
    if (!c->tail) c->tail = n;
}

static void drop(TTLCache *c, CacheNode *n) {
    unlink_node(c, n);
    sm_remove(c->map, n->key);
    free(n->key);
    free(n->value);
    free(n);
}

void cache_free(TTLCache *c) {
    if (!c) return;
    while (c->head) drop(c, c->head);
    sm_free(c->map, NULL);
    pthread_mutex_destroy(&c->lock);
    free(c);
}

char *cache_get(TTLCache *c, const char *key) {
    char *copy = NULL;
    pthread_mutex_lock(&c->lock);
    CacheNode *n = sm_get(c->map, key);
    if (n) {
        if (now_monotonic() >= n->expires) {
            drop(c, n);
        } else {
            unlink_node(c, n);                 /* mark as recently used */
            push_front(c, n);
            copy = xstrdup(n->value);
        }
    }
    pthread_mutex_unlock(&c->lock);
    return copy;
}

void cache_set(TTLCache *c, const char *key, const char *value) {
    pthread_mutex_lock(&c->lock);
    CacheNode *n = sm_get(c->map, key);
    if (n) {
        free(n->value);
        n->value = xstrdup(value);
        unlink_node(c, n);
    } else {
        n = xcalloc(1, sizeof *n);
        n->key = xstrdup(key);
        n->value = xstrdup(value);
        sm_put(c->map, key, n);
    }
    n->expires = now_monotonic() + c->ttl;
    push_front(c, n);
    while ((int)sm_count(c->map) > c->max_items) drop(c, c->tail);   /* evict least recently used */
    pthread_mutex_unlock(&c->lock);
}
