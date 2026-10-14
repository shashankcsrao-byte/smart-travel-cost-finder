/*
 * history.h - search history stored in SQLite (a single database file).
 *
 * Same tables as before (searches, live_searches), so an existing
 * travel_history.db keeps working. Every query uses "?" placeholders, which
 * prevents SQL injection; the newest 500 rows are kept; the same search again
 * within 2 minutes is not stored twice.
 */
#ifndef HISTORY_H
#define HISTORY_H

#include <pthread.h>
#include <sqlite3.h>

#include "json.h"

typedef struct {
    sqlite3 *db;
    pthread_mutex_t lock;
} History;

History *history_open(const char *path);          /* NULL on failure */
void history_close(History *h);

/* fare < 0 or mode NULL store NULL */
int history_add(History *h, const char *source, const char *destination, const char *mode, int has_fare, double fare);
int history_add_live(History *h, const char *oname, double olat, double olng, const char *dname, double dlat,
                     double dlng, const char *mode, double fare);
void history_write_recent(History *h, JW *w, int limit);
void history_write_popular(History *h, JW *w, int limit);
void history_write_recent_live(History *h, JW *w, int limit);
void history_clear(History *h);

#endif
