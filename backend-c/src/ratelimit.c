/*
 * ratelimit.c - see ratelimit.h.
 */
#include "ratelimit.h"

#include <stdlib.h>

#include "util.h"

typedef struct {
    double *times;               /* ring buffer with room for `limit` entries */
    int head, count;
} Window;

RateLimiter *ratelimit_new(int limit, double window_seconds) {
    RateLimiter *r = xcalloc(1, sizeof *r);
    r->limit = limit;
    r->window = window_seconds;
    r->clients = sm_new(64);
    pthread_mutex_init(&r->lock, NULL);
    return r;
}

static void free_window(void *p) {
    Window *w = p;
    free(w->times);
    free(w);
}

void ratelimit_free(RateLimiter *r) {
    if (!r) return;
    sm_free(r->clients, free_window);
    pthread_mutex_destroy(&r->lock);
    free(r);
}

static void expire(Window *w, double now, double window, int limit) {
    while (w->count && now - w->times[w->head] >= window) {     /* popleft */
        w->head = (w->head + 1) % limit;
        w->count--;
    }
}

int ratelimit_allow(RateLimiter *r, const char *client) {
    double now = now_monotonic();
    pthread_mutex_lock(&r->lock);
    Window *w = sm_get(r->clients, client);
    if (!w) {
        w = xcalloc(1, sizeof *w);
        w->times = xmalloc((size_t)r->limit * sizeof *w->times);
        sm_put(r->clients, client, w);
    }
    expire(w, now, r->window, r->limit);
    int ok = w->count < r->limit;
    if (ok) {
        w->times[(w->head + w->count) % r->limit] = now;            /* append */
        w->count++;
    }
    if (sm_count(r->clients) > 10000) {                             /* forget idle clients */
        size_t bucket = 0;
        SmEntry *e = NULL, *next;
        e = sm_next(r->clients, &bucket, NULL);
        while (e) {
            next = sm_next(r->clients, &bucket, e);
            Window *idle = e->value;
            expire(idle, now, r->window, r->limit);
            if (idle->count == 0 && idle != w) {
                char *key = xstrdup(e->key);
                free_window(sm_remove(r->clients, key));
                free(key);
                bucket = 0;                     /* the table changed: start the walk again */
                next = sm_next(r->clients, &bucket, NULL);
            }
            e = next;
        }
    }
    pthread_mutex_unlock(&r->lock);
    return ok;
}

int ratelimit_retry_after(RateLimiter *r, const char *client) {
    double now = now_monotonic();
    int seconds = 0;
    pthread_mutex_lock(&r->lock);
    Window *w = sm_get(r->clients, client);
    if (w && w->count) {
        seconds = (int)(r->window - (now - w->times[w->head])) + 1;
        if (seconds < 0) seconds = 0;
    }
    pthread_mutex_unlock(&r->lock);
    return seconds;
}
