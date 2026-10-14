/*
 * history.c - see history.h.
 */
#include "history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util.h"

#define MAX_ROWS 500           /* keep the newest rows only, so the file never grows forever */
#define REPEAT_WINDOW_S 120    /* the same search again within 2 minutes is not stored twice */

static const char *SCHEMA[] = {
    "CREATE TABLE IF NOT EXISTS searches ("
    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
    " source TEXT NOT NULL,"
    " destination TEXT NOT NULL,"
    " cheapest_mode TEXT,"
    " cheapest_fare REAL,"
    " searched_at TEXT NOT NULL)",
    "CREATE TABLE IF NOT EXISTS live_searches ("
    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
    " origin_name TEXT NOT NULL, origin_lat REAL NOT NULL, origin_lng REAL NOT NULL,"
    " dest_name TEXT NOT NULL, dest_lat REAL NOT NULL, dest_lng REAL NOT NULL,"
    " cheapest_mode TEXT, cheapest_fare REAL, searched_at TEXT NOT NULL)",
    "CREATE INDEX IF NOT EXISTS idx_searches_pair ON searches (source, destination)",
};

History *history_open(const char *path) {
    History *h = xcalloc(1, sizeof *h);
    if (sqlite3_open(path, &h->db) != SQLITE_OK) {
        fprintf(stderr, "error: cannot open history database %s: %s\n", path, sqlite3_errmsg(h->db));
        sqlite3_close(h->db);
        free(h);
        return NULL;
    }
    sqlite3_busy_timeout(h->db, 3000);
    for (size_t i = 0; i < sizeof SCHEMA / sizeof *SCHEMA; i++) sqlite3_exec(h->db, SCHEMA[i], NULL, NULL, NULL);
    pthread_mutex_init(&h->lock, NULL);
    return h;
}

void history_close(History *h) {
    if (!h) return;
    sqlite3_close(h->db);
    pthread_mutex_destroy(&h->lock);
    free(h);
}

/* ISO 8601 UTC timestamp, like Python's datetime.now(timezone.utc).isoformat(timespec="seconds"). */
static void now_iso(char out[40]) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 40, "%Y-%m-%dT%H:%M:%S+00:00", &tm);
}

static time_t parse_iso(const char *s) {
    int y, mo, d, h, mi, sec, oh = 0, om = 0;
    char sign = '+';
    if (sscanf(s, "%d-%d-%dT%d:%d:%d%c%d:%d", &y, &mo, &d, &h, &mi, &sec, &sign, &oh, &om) < 6) return 0;
    long offset = (oh * 3600L + om * 60L) * (sign == '-' ? -1 : 1);
    return (time_t)(days_from_civil(y, mo, d) * 86400L + h * 3600L + mi * 60L + sec - offset);
}

static int recently_stored(sqlite3_stmt *st, int time_col) {
    const unsigned char *at = sqlite3_column_text(st, time_col);
    return at && difftime(time(NULL), parse_iso((const char *)at)) < REPEAT_WINDOW_S;
}

static void trim(History *h, const char *table) {
    char sql[160];
    snprintf(sql, sizeof sql, "DELETE FROM %s WHERE id <= (SELECT MAX(id) FROM %s) - ?", table, table);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(h->db, sql, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(st, 1, MAX_ROWS);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static int same_text(sqlite3_stmt *st, int col, const char *value) {
    const unsigned char *v = sqlite3_column_text(st, col);
    return v && strcmp((const char *)v, value) == 0;
}

int history_add(History *h, const char *source, const char *destination, const char *mode, int has_fare, double fare) {
    pthread_mutex_lock(&h->lock);
    sqlite3_stmt *st;
    int repeat = 0;
    if (sqlite3_prepare_v2(h->db, "SELECT source, destination, searched_at FROM searches ORDER BY id DESC LIMIT 1",
                           -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            repeat = same_text(st, 0, source) && same_text(st, 1, destination) && recently_stored(st, 2);
        sqlite3_finalize(st);
    }
    if (!repeat && sqlite3_prepare_v2(h->db,
                                      "INSERT INTO searches (source, destination, cheapest_mode, cheapest_fare, "
                                      "searched_at) VALUES (?, ?, ?, ?, ?)",
                                      -1, &st, NULL) == SQLITE_OK) {
        char at[40];
        now_iso(at);
        sqlite3_bind_text(st, 1, source, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, destination, -1, SQLITE_TRANSIENT);
        if (mode) sqlite3_bind_text(st, 3, mode, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(st, 3);
        if (has_fare) sqlite3_bind_double(st, 4, fare);
        else sqlite3_bind_null(st, 4);
        sqlite3_bind_text(st, 5, at, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
        trim(h, "searches");
    }
    pthread_mutex_unlock(&h->lock);
    return !repeat;
}

int history_add_live(History *h, const char *oname, double olat, double olng, const char *dname, double dlat,
                     double dlng, const char *mode, double fare) {
    pthread_mutex_lock(&h->lock);
    sqlite3_stmt *st;
    int repeat = 0;
    if (sqlite3_prepare_v2(h->db,
                           "SELECT origin_name, dest_name, origin_lat, dest_lat, searched_at FROM live_searches "
                           "ORDER BY id DESC LIMIT 1",
                           -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            repeat = same_text(st, 0, oname) && same_text(st, 1, dname) && sqlite3_column_double(st, 2) == olat &&
                     sqlite3_column_double(st, 3) == dlat && recently_stored(st, 4);
        sqlite3_finalize(st);
    }
    if (!repeat && sqlite3_prepare_v2(h->db,
                                      "INSERT INTO live_searches (origin_name, origin_lat, origin_lng, dest_name, "
                                      "dest_lat, dest_lng, cheapest_mode, cheapest_fare, searched_at) "
                                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                      -1, &st, NULL) == SQLITE_OK) {
        char at[40];
        now_iso(at);
        sqlite3_bind_text(st, 1, oname, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 2, olat);
        sqlite3_bind_double(st, 3, olng);
        sqlite3_bind_text(st, 4, dname, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 5, dlat);
        sqlite3_bind_double(st, 6, dlng);
        sqlite3_bind_text(st, 7, mode, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 8, fare);
        sqlite3_bind_text(st, 9, at, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
        trim(h, "live_searches");
    }
    pthread_mutex_unlock(&h->lock);
    return !repeat;
}

static void write_column(JW *w, sqlite3_stmt *st, int col) {
    switch (sqlite3_column_type(st, col)) {
    case SQLITE_NULL: jw_null(w); break;
    case SQLITE_INTEGER: jw_int(w, (long)sqlite3_column_int64(st, col)); break;
    case SQLITE_FLOAT: jw_num(w, sqlite3_column_double(st, col)); break;
    default: jw_str(w, (const char *)sqlite3_column_text(st, col));
    }
}

static void write_rows(History *h, JW *w, const char *sql, int limit) {
    pthread_mutex_lock(&h->lock);
    jw_arr(w);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(h->db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, limit);
        while (sqlite3_step(st) == SQLITE_ROW) {
            jw_obj(w);
            for (int c = 0; c < sqlite3_column_count(st); c++) {
                jw_key(w, sqlite3_column_name(st, c));
                write_column(w, st, c);
            }
            jw_obj_end(w);
        }
        sqlite3_finalize(st);
    }
    jw_arr_end(w);
    pthread_mutex_unlock(&h->lock);
}

void history_write_recent(History *h, JW *w, int limit) {
    write_rows(h, w,
               "SELECT source, destination, cheapest_mode, cheapest_fare, searched_at "
               "FROM searches ORDER BY id DESC LIMIT ?",
               limit);
}

void history_write_popular(History *h, JW *w, int limit) {
    write_rows(h, w,
               "SELECT source, destination, COUNT(*) AS searches FROM searches GROUP BY source, destination "
               "ORDER BY searches DESC, MAX(id) DESC LIMIT ?",
               limit);
}

void history_write_recent_live(History *h, JW *w, int limit) {
    pthread_mutex_lock(&h->lock);
    jw_arr(w);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(h->db,
                           "SELECT origin_name, origin_lat, origin_lng, dest_name, dest_lat, dest_lng, cheapest_mode, "
                           "cheapest_fare, searched_at FROM live_searches ORDER BY id DESC LIMIT ?",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, limit);
        while (sqlite3_step(st) == SQLITE_ROW) {
            jw_obj(w);
            jw_key(w, "origin"), jw_obj(w);
            jw_key(w, "name"), write_column(w, st, 0);
            jw_key(w, "lat"), write_column(w, st, 1);
            jw_key(w, "lng"), write_column(w, st, 2);
            jw_obj_end(w);
            jw_key(w, "destination"), jw_obj(w);
            jw_key(w, "name"), write_column(w, st, 3);
            jw_key(w, "lat"), write_column(w, st, 4);
            jw_key(w, "lng"), write_column(w, st, 5);
            jw_obj_end(w);
            jw_key(w, "cheapest_mode"), write_column(w, st, 6);
            jw_key(w, "cheapest_fare"), write_column(w, st, 7);
            jw_key(w, "searched_at"), write_column(w, st, 8);
            jw_obj_end(w);
        }
        sqlite3_finalize(st);
    }
    jw_arr_end(w);
    pthread_mutex_unlock(&h->lock);
}

void history_clear(History *h) {
    pthread_mutex_lock(&h->lock);
    sqlite3_exec(h->db, "DELETE FROM searches", NULL, NULL, NULL);
    sqlite3_exec(h->db, "DELETE FROM live_searches", NULL, NULL, NULL);
    pthread_mutex_unlock(&h->lock);
}
