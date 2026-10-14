/*
 * ratelimit.h - a sliding-window rate limiter: at most `limit` requests per
 * `window` seconds for each client.
 *
 * Each client gets a queue of the times of its recent requests (a ring buffer,
 * the C version of collections.deque). Old times fall off the front; a request
 * is allowed while fewer than `limit` remain. O(1) amortised per request.
 */
#ifndef RATELIMIT_H
#define RATELIMIT_H

#include <pthread.h>

#include "strmap.h"

typedef struct {
    int limit;
    double window;
    StrMap *clients;             /* client -> ring buffer of request times */
    pthread_mutex_t lock;
} RateLimiter;

RateLimiter *ratelimit_new(int limit, double window_seconds);
void ratelimit_free(RateLimiter *r);
int ratelimit_allow(RateLimiter *r, const char *client);       /* 1 = go ahead (and recorded) */
int ratelimit_retry_after(RateLimiter *r, const char *client); /* seconds until a slot frees up */

#endif
