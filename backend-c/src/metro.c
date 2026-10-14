/*
 * metro.c - see metro.h.
 */
#include "metro.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csv.h"
#include "heap.h"
#include "routing.h"
#include "util.h"
#include "zipread.h"

#define DAY METRO_DAY

typedef struct {
    int n, cap;
    int *service;
    char *type;
} ExceptionList;

/* ---------------------------------------------------------------- helpers */

static int hms(const char *s) {
    int h = 0, m = 0, sec = 0;
    sscanf(s, "%d:%d:%d", &h, &m, &sec);
    return h * 3600 + m * 60 + sec;
}

void metro_clock(double seconds, char out[8]) {
    long s = py_round_int(seconds);
    s = ((s % DAY) + DAY) % DAY;
    snprintf(out, 8, "%02ld:%02ld", s / 3600, s % 3600 / 60);
}

static void put_number(StrMap *map, const char *key, double value) {
    double *p = sm_get(map, key);
    if (!p) {
        p = xmalloc(sizeof *p);
        sm_put(map, key, p);
    }
    *p = value;
}

static const double *get_number(const StrMap *map, const char *key) { return sm_get(map, key); }

static int service_id(MetroNetwork *m, const char *id, int create) {
    intptr_t v = (intptr_t)sm_get(m->service_index, id);
    if (v) return (int)v - 1;
    if (!create) return -1;
    m->services = xrealloc(m->services, (size_t)(m->nservices + 1) * sizeof *m->services);
    m->services[m->nservices] = xstrdup(id);
    sm_put(m->service_index, id, (void *)(intptr_t)(m->nservices + 1));
    return m->nservices++;
}

int metro_station(const MetroNetwork *m, const char *id) {
    intptr_t v = (intptr_t)sm_get(m->station_index, id);
    return v ? (int)v - 1 : -1;
}

static int line_id(const MetroNetwork *m, const char *id) {
    intptr_t v = (intptr_t)sm_get(m->line_index, id);
    return v ? (int)v - 1 : -1;
}

static DepList *deplist(const MetroNetwork *m, int service, int line, int dir, int station) {
    size_t i = (((size_t)service * (size_t)m->nlines + (size_t)line) * 2 + (size_t)dir) * (size_t)m->nstations +
               (size_t)station;
    return &m->deps[i];
}

static Csv *load_csv(const ZipArchive *z, const char *name) {
    size_t len = 0;
    char *text = zip_read(z, name, &len);
    return text ? csv_parse(text, len) : NULL;
}

/* ============================================================ loading */

typedef struct {
    int seq;
    Call call;
} RawCall;

typedef struct {
    RawCall *rows;
    int n, cap;
} RawCalls;

static int rawcall_cmp(const void *a, const void *b) {
    const RawCall *x = a, *y = b;
    return (x->seq > y->seq) - (x->seq < y->seq);
}

static int dep_cmp_ctx_trips_n;
static const Trip *dep_cmp_trips;

static int dep_cmp(const void *a, const void *b) {
    const Dep *x = a, *y = b;
    if (x->dep != y->dep) return x->dep < y->dep ? -1 : 1;
    int c = strcmp(dep_cmp_trips[x->trip].id, dep_cmp_trips[y->trip].id);
    if (c) return c;
    return (x->idx > y->idx) - (x->idx < y->idx);
}

typedef struct {
    char *a, *b;
    double km, min;
} Link;

typedef struct {
    int line, dir, trip;
} LongKey;

static const MetroNetwork *sort_net;
static int longkey_cmp(const void *a, const void *b) {
    const LongKey *x = a, *y = b;
    int c = strcmp(sort_net->lines[x->line].id, sort_net->lines[y->line].id);
    return c ? c : (x->dir > y->dir) - (x->dir < y->dir);
}

static int str_ptr_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

MetroNetwork *metro_load(const char *zip_path, char *err, size_t errlen) {
    ZipArchive z;
    if (zip_open(&z, zip_path) != 0) {
        snprintf(err, errlen, "cannot read %s", zip_path);
        return NULL;
    }
    const char *names[] = {"stops.txt", "routes.txt", "trips.txt", "stop_times.txt", "calendar.txt",
                           "calendar_dates.txt", "fare_attributes.txt", "fare_rules.txt", "fare_products.txt",
                           "fare_leg_rules.txt", "timeframes.txt", "feed_info.txt"};
    Csv *t[12];
    for (int i = 0; i < 12; i++) {
        t[i] = load_csv(&z, names[i]);
        if (!t[i]) {
            snprintf(err, errlen, "%s is missing from the GTFS feed", names[i]);
            for (int k = 0; k < i; k++) csv_free(t[k]);
            zip_close(&z);
            return NULL;
        }
    }
    zip_close(&z);
    Csv *stops = t[0], *routes = t[1], *trips = t[2], *stop_times = t[3], *calendar = t[4], *cdates = t[5];
    Csv *fattr = t[6], *frules = t[7], *fprod = t[8], *flegs = t[9], *tframes = t[10], *finfo = t[11];

    MetroNetwork *m = xcalloc(1, sizeof *m);
    m->service_index = sm_new(16);
    m->exceptions = sm_new(64);
    m->token_fare = sm_new(8192);
    m->card_fare = sm_new(16384);
    m->hop_km = sm_new(256);

    /* ---- feed end date ---- */
    const char *end = csv_get(finfo, 0, csv_col(finfo, "feed_end_date"));
    int y, mo, d;
    if (finfo->nrows > 0 && sscanf(end, "%4d%2d%2d", &y, &mo, &d) == 3) {
        m->has_feed_end = 1;
        m->feed_end_days = days_from_civil(y, mo, d);
    }

    /* ---- stations (parent stations only) ---- */
    int s_id = csv_col(stops, "stop_id"), s_name = csv_col(stops, "stop_name"), s_lat = csv_col(stops, "stop_lat"),
        s_lon = csv_col(stops, "stop_lon"), s_type = csv_col(stops, "location_type"),
        s_parent = csv_col(stops, "parent_station");
    StrMap *parent_of = sm_new((size_t)stops->nrows);
    m->stations = xcalloc((size_t)stops->nrows, sizeof *m->stations);
    m->station_index = sm_new((size_t)stops->nrows);
    for (int r = 0; r < stops->nrows; r++) {
        const char *id = csv_get(stops, r, s_id), *parent = csv_get(stops, r, s_parent);
        sm_put(parent_of, id, xstrdup(parent[0] ? parent : id));
        if (strcmp(csv_get(stops, r, s_type), "1") != 0) continue;
        Station *st = &m->stations[m->nstations];
        st->id = xstrdup(id);
        st->name = xstrdup(csv_get(stops, r, s_name));
        st->lat = strtod(csv_get(stops, r, s_lat), NULL);
        st->lng = strtod(csv_get(stops, r, s_lon), NULL);
        sm_put(m->station_index, id, (void *)(intptr_t)(m->nstations + 1));
        m->nstations++;
    }

    /* ---- lines ---- */
    int r_id = csv_col(routes, "route_id"), r_short = csv_col(routes, "route_short_name"),
        r_color = csv_col(routes, "route_color");
    m->lines = xcalloc((size_t)(routes->nrows ? routes->nrows : 1), sizeof *m->lines);
    m->line_index = sm_new(8);
    for (int r = 0; r < routes->nrows; r++) {
        Line *ln = &m->lines[m->nlines];
        ln->id = xstrdup(csv_get(routes, r, r_id));
        Buf b;
        buf_init(&b);
        buf_printf(&b, "%s Line", csv_get(routes, r, r_short));
        ln->name = buf_steal(&b);
        buf_init(&b);
        const char *color = csv_get(routes, r, r_color);
        buf_printf(&b, "#%s", color[0] ? color : "888888");
        ln->color = buf_steal(&b);
        sm_put(m->line_index, ln->id, (void *)(intptr_t)(m->nlines + 1));
        m->nlines++;
    }

    /* ---- calendar: which services run on which date ---- */
    static const char *weekdays[7] = {"monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"};
    m->calendar = xcalloc((size_t)(calendar->nrows ? calendar->nrows : 1), sizeof *m->calendar);
    for (int r = 0; r < calendar->nrows; r++) {
        CalRow *row = &m->calendar[m->ncalendar++];
        row->service = service_id(m, csv_get(calendar, r, csv_col(calendar, "service_id")), 1);
        for (int w = 0; w < 7; w++) row->days[w] = strcmp(csv_get(calendar, r, csv_col(calendar, weekdays[w])), "1") == 0;
        snprintf(row->start, sizeof row->start, "%s", csv_get(calendar, r, csv_col(calendar, "start_date")));
        snprintf(row->end, sizeof row->end, "%s", csv_get(calendar, r, csv_col(calendar, "end_date")));
    }
    for (int r = 0; r < cdates->nrows; r++) {
        const char *date = csv_get(cdates, r, csv_col(cdates, "date"));
        ExceptionList *list = sm_get(m->exceptions, date);
        if (!list) {
            list = xcalloc(1, sizeof *list);
            sm_put(m->exceptions, date, list);
        }
        if (list->n == list->cap) {
            list->cap = list->cap ? list->cap * 2 : 4;
            list->service = xrealloc(list->service, (size_t)list->cap * sizeof(int));
            list->type = xrealloc(list->type, (size_t)list->cap);
        }
        list->service[list->n] = service_id(m, csv_get(cdates, r, csv_col(cdates, "service_id")), 1);
        list->type[list->n] = csv_get(cdates, r, csv_col(cdates, "exception_type"))[0];
        list->n++;
    }

    /* ---- trips ---- */
    int t_id = csv_col(trips, "trip_id"), t_route = csv_col(trips, "route_id"), t_dir = csv_col(trips, "direction_id"),
        t_service = csv_col(trips, "service_id");
    StrMap *trip_index = sm_new((size_t)trips->nrows);
    m->trips = xcalloc((size_t)(trips->nrows ? trips->nrows : 1), sizeof *m->trips);
    for (int r = 0; r < trips->nrows; r++) {
        int line = line_id(m, csv_get(trips, r, t_route));
        if (line < 0) continue;
        Trip *tr = &m->trips[m->ntrips];
        tr->id = xstrdup(csv_get(trips, r, t_id));
        tr->line = line;
        tr->dir = strcmp(csv_get(trips, r, t_dir), "1") == 0;
        tr->service = service_id(m, csv_get(trips, r, t_service), 1);
        sm_put(trip_index, tr->id, (void *)(intptr_t)(m->ntrips + 1));
        m->ntrips++;
    }
    sm_free(trip_index, NULL);   /* rebuilt below with the same numbers */
    trip_index = sm_new((size_t)m->ntrips);
    for (int i = 0; i < m->ntrips; i++) sm_put(trip_index, m->trips[i].id, (void *)(intptr_t)(i + 1));

    /* ---- stop times, grouped by trip in the order trips first appear ---- */
    int st_trip = csv_col(stop_times, "trip_id"), st_arr = csv_col(stop_times, "arrival_time"),
        st_dep = csv_col(stop_times, "departure_time"), st_stop = csv_col(stop_times, "stop_id"),
        st_seq = csv_col(stop_times, "stop_sequence"), st_dist = csv_col(stop_times, "shape_dist_traveled");
    RawCalls *raw = xcalloc((size_t)(m->ntrips ? m->ntrips : 1), sizeof *raw);
    int *order = xmalloc((size_t)(m->ntrips ? m->ntrips : 1) * sizeof *order);
    int norder = 0;
    for (int r = 0; r < stop_times->nrows; r++) {
        intptr_t ti = (intptr_t)sm_get(trip_index, csv_get(stop_times, r, st_trip));
        if (!ti) continue;
        RawCalls *rc = &raw[ti - 1];
        if (rc->n == 0) order[norder++] = (int)ti - 1;
        if (rc->n == rc->cap) {
            rc->cap = rc->cap ? rc->cap * 2 : 32;
            rc->rows = xrealloc(rc->rows, (size_t)rc->cap * sizeof *rc->rows);
        }
        const char *stop = csv_get(stop_times, r, st_stop);
        const char *parent = sm_get(parent_of, stop);
        int station = metro_station(m, parent ? parent : stop);
        if (station < 0) {
            snprintf(err, errlen, "stop %s has no parent station", stop);
            goto fail;
        }
        rc->rows[rc->n++] = (RawCall){atoi(csv_get(stop_times, r, st_seq)),
                                      {station, hms(csv_get(stop_times, r, st_arr)), hms(csv_get(stop_times, r, st_dep)),
                                       strtod(csv_get(stop_times, r, st_dist), NULL)}};
    }

    m->deps = xcalloc((size_t)m->nservices * (size_t)m->nlines * 2 * (size_t)m->nstations, sizeof *m->deps);
    int *longest = xmalloc((size_t)m->nlines * 2 * sizeof *longest);
    int *weekday_departures = xcalloc((size_t)m->nlines * 2, sizeof *weekday_departures);
    for (int i = 0; i < m->nlines * 2; i++) longest[i] = -1;
    int weekday = service_id(m, "weekday", 0);

    for (int o = 0; o < norder; o++) {
        int ti = order[o];
        Trip *tr = &m->trips[ti];
        RawCalls *rc = &raw[ti];
        qsort(rc->rows, (size_t)rc->n, sizeof *rc->rows, rawcall_cmp);
        tr->ncalls = rc->n;
        tr->calls = xmalloc((size_t)rc->n * sizeof *tr->calls);
        for (int k = 0; k < rc->n; k++) tr->calls[k] = rc->rows[k].call;
        for (int k = 0; k < rc->n - 1; k++) {
            DepList *l = deplist(m, tr->service, tr->line, tr->dir, tr->calls[k].station);
            if (l->n == l->cap) {
                l->cap = l->cap ? l->cap * 2 : 16;
                l->d = xrealloc(l->d, (size_t)l->cap * sizeof *l->d);
            }
            l->d[l->n++] = (Dep){tr->calls[k].dep, ti, k};
        }
        int key = tr->line * 2 + tr->dir;
        if (longest[key] < 0 || tr->ncalls > m->trips[longest[key]].ncalls) longest[key] = ti;
        if (tr->service == weekday && rc->n > 0 && tr->calls[0].dep >= 8 * 3600 && tr->calls[0].dep < 20 * 3600)
            weekday_departures[key]++;
    }
    dep_cmp_trips = m->trips;
    (void)dep_cmp_ctx_trips_n;
    size_t ndeps = (size_t)m->nservices * (size_t)m->nlines * 2 * (size_t)m->nstations;
    for (size_t i = 0; i < ndeps; i++)
        if (m->deps[i].n > 1) qsort(m->deps[i].d, (size_t)m->deps[i].n, sizeof(Dep), dep_cmp);   /* sorted -> binary search works */

    /* Average wait = half the weekday daytime headway of that line (static graph only). */
    for (int l = 0; l < m->nlines; l++) {
        int most = 0;
        for (int dir = 0; dir < 2; dir++)
            if (weekday_departures[l * 2 + dir] > most) most = weekday_departures[l * 2 + dir];
        double headway = most ? 12.0 * 60 / most : 2 * DEFAULT_WAIT_MIN;
        m->lines[l].wait_min = py_round(headway / 2, 1);
    }

    /* ---- static graph from one full-length trip per line and direction ---- */
    LongKey keys[64];
    int nkeys = 0;
    for (int i = 0; i < m->nlines * 2 && nkeys < 64; i++)
        if (longest[i] >= 0) keys[nkeys++] = (LongKey){i / 2, i % 2, longest[i]};
    sort_net = m;
    qsort(keys, (size_t)nkeys, sizeof *keys, longkey_cmp);

    Link *links = NULL;
    int nlinks = 0, caplinks = 0;
#define ADD_LINK(A, B, KM, MIN)                                              \
    do {                                                                     \
        if (nlinks == caplinks) {                                            \
            caplinks = caplinks ? caplinks * 2 : 256;                        \
            links = xrealloc(links, (size_t)caplinks * sizeof *links);       \
        }                                                                    \
        links[nlinks++] = (Link){(A), (B), (KM), (MIN)};                     \
    } while (0)

    Buf a, b;
    for (int k = 0; k < nkeys; k++) {
        Trip *tr = &m->trips[keys[k].trip];
        Line *ln = &m->lines[keys[k].line];
        if (keys[k].dir == 0) {
            ln->nstations = tr->ncalls;
            ln->stations = xmalloc((size_t)tr->ncalls * sizeof(int));
            for (int c = 0; c < tr->ncalls; c++) ln->stations[c] = tr->calls[c].station;
        }
        for (int c = 0; c < tr->ncalls; c++) {
            Station *st = &m->stations[tr->calls[c].station];
            int has = 0;
            for (int q = 0; q < st->nlines; q++)
                if (st->lines[q] == keys[k].line) has = 1;
            if (!has && st->nlines < 8) st->lines[st->nlines++] = keys[k].line;
        }
        for (int c = 0; c + 1 < tr->ncalls; c++) {
            const Call *ca = &tr->calls[c], *cb = &tr->calls[c + 1];
            double km = py_round(cb->dist - ca->dist, 3);
            double minutes = py_round((cb->arr - ca->arr) / 60.0, 2);
            const char *sa = m->stations[ca->station].id, *sb = m->stations[cb->station].id;
            buf_init(&a);
            buf_printf(&a, "%s|%s|%s", sa, sb, ln->id);
            put_number(m->hop_km, a.s, km);
            buf_free(&a);
            buf_init(&a);
            buf_printf(&a, "%s@%s", sa, ln->id);
            buf_init(&b);
            buf_printf(&b, "%s@%s", sb, ln->id);
            ADD_LINK(buf_steal(&a), buf_steal(&b), km, minutes);
        }
    }
    for (int s = 0; s < m->nstations; s++) {
        Station *st = &m->stations[s];
        for (int q = 0; q < st->nlines; q++) {
            Line *ln = &m->lines[st->lines[q]];
            buf_init(&a);
            buf_printf(&a, "IN:%s", st->id);
            buf_init(&b);
            buf_printf(&b, "%s@%s", st->id, ln->id);
            ADD_LINK(buf_steal(&a), buf_steal(&b), 0, ln->wait_min);
            buf_init(&a);
            buf_printf(&a, "%s@%s", st->id, ln->id);
            buf_init(&b);
            buf_printf(&b, "OUT:%s", st->id);
            ADD_LINK(buf_steal(&a), buf_steal(&b), 0, 0);
            for (int o = 0; o < st->nlines; o++) {
                if (st->lines[o] == st->lines[q]) continue;
                Line *other = &m->lines[st->lines[o]];
                buf_init(&a);
                buf_printf(&a, "%s@%s", st->id, ln->id);
                buf_init(&b);
                buf_printf(&b, "%s@%s", st->id, other->id);
                ADD_LINK(buf_steal(&a), buf_steal(&b), 0, INTERCHANGE_WALK_MIN + other->wait_min);
            }
        }
    }
#undef ADD_LINK

    /* Vertices = every distinct name, sorted (like Python's sorted(set(...))). */
    StrMap *seen = sm_new((size_t)nlinks);
    char **node_names = xmalloc((size_t)(nlinks * 2 + 1) * sizeof *node_names);
    int nnodes = 0;
    for (int i = 0; i < nlinks; i++) {
        if (!sm_has(seen, links[i].a)) {
            sm_put(seen, links[i].a, NULL);
            node_names[nnodes++] = links[i].a;
        }
        if (!sm_has(seen, links[i].b)) {
            sm_put(seen, links[i].b, NULL);
            node_names[nnodes++] = links[i].b;
        }
    }
    sm_free(seen, NULL);
    qsort(node_names, (size_t)nnodes, sizeof *node_names, str_ptr_cmp);
    m->graph = graph_new((const char *const *)node_names, nnodes);
    free(node_names);
    m->top_speed_km_s = 0;
    for (int i = 0; i < nlinks; i++) {
        graph_add_edge(m->graph, graph_index(m->graph, links[i].a), graph_index(m->graph, links[i].b), MODE_METRO,
                       links[i].km, links[i].min);
        if (links[i].km > 0 && links[i].min > 0) {
            double speed = links[i].km / (links[i].min * 60);
            if (speed > m->top_speed_km_s) m->top_speed_km_s = speed;
        }
        free(links[i].a);
        free(links[i].b);
    }
    free(links);
    if (m->top_speed_km_s <= 0) m->top_speed_km_s = 0.02;

    m->node_station = xmalloc((size_t)m->graph->n * sizeof(int));
    m->node_line = xmalloc((size_t)m->graph->n * sizeof(int));
    for (int v = 0; v < m->graph->n; v++) {
        const char *name = m->graph->names[v];
        const char *at = strchr(name, '@');
        if (at) {
            char *sid = xstrndup(name, (size_t)(at - name));
            m->node_station[v] = metro_station(m, sid);
            m->node_line[v] = line_id(m, at + 1);
            free(sid);
        } else {
            m->node_station[v] = metro_station(m, strchr(name, ':') + 1);
            m->node_line[v] = -1;
        }
    }

    /* ---- official fares ---- */
    StrMap *price = sm_new(32);
    for (int r = 0; r < fattr->nrows; r++)
        put_number(price, csv_get(fattr, r, csv_col(fattr, "fare_id")), strtod(csv_get(fattr, r, csv_col(fattr, "price")), NULL));
    int fr_id = csv_col(frules, "fare_id"), fr_o = csv_col(frules, "origin_id"), fr_d = csv_col(frules, "destination_id");
    for (int r = 0; r < frules->nrows; r++) {
        const double *p = get_number(price, csv_get(frules, r, fr_id));
        if (!p) continue;
        buf_init(&a);
        buf_printf(&a, "%s|%s", csv_get(frules, r, fr_o), csv_get(frules, r, fr_d));
        put_number(m->token_fare, a.s, *p);
        buf_free(&a);
    }
    StrMap *amount = sm_new(64);
    for (int r = 0; r < fprod->nrows; r++)
        put_number(amount, csv_get(fprod, r, csv_col(fprod, "fare_product_id")),
                   strtod(csv_get(fprod, r, csv_col(fprod, "amount")), NULL));
    int fl_prod = csv_col(flegs, "fare_product_id"), fl_from = csv_col(flegs, "from_area_id"),
        fl_to = csv_col(flegs, "to_area_id"), fl_tf = csv_col(flegs, "from_timeframe_group_id");
    for (int r = 0; r < flegs->nrows; r++) {
        const char *product = csv_get(flegs, r, fl_prod);
        if (!strstr(product, "smartcard")) continue;
        const double *p = get_number(amount, product);
        if (!p) continue;
        buf_init(&a);
        buf_printf(&a, "%s|%s|%s", csv_get(flegs, r, fl_from), csv_get(flegs, r, fl_to), csv_get(flegs, r, fl_tf));
        put_number(m->card_fare, a.s, *p);
        buf_free(&a);
    }
    sm_free(price, free);
    sm_free(amount, free);

    m->timeframes = xcalloc((size_t)(tframes->nrows ? tframes->nrows : 1), sizeof *m->timeframes);
    for (int r = 0; r < tframes->nrows; r++) {
        Timeframe *tf = &m->timeframes[m->ntimeframes++];
        tf->group = xstrdup(csv_get(tframes, r, csv_col(tframes, "timeframe_group_id")));
        snprintf(tf->start, sizeof tf->start, "%s", csv_get(tframes, r, csv_col(tframes, "start_time")));
        snprintf(tf->end, sizeof tf->end, "%s", csv_get(tframes, r, csv_col(tframes, "end_time")));
        tf->service = service_id(m, csv_get(tframes, r, csv_col(tframes, "service_id")), 0);
    }

    for (int i = 0; i < m->ntrips; i++) free(raw[i].rows);
    free(raw);
    free(order);
    free(longest);
    free(weekday_departures);
    sm_free(trip_index, NULL);
    sm_free(parent_of, free);
    for (int i = 0; i < 12; i++) csv_free(t[i]);
    return m;

fail:
    for (int i = 0; i < m->ntrips; i++) free(raw[i].rows);
    free(raw);
    free(order);
    sm_free(trip_index, NULL);
    sm_free(parent_of, free);
    for (int i = 0; i < 12; i++) csv_free(t[i]);
    metro_free(m);
    return NULL;
}

static void free_exceptions(void *p) {
    ExceptionList *l = p;
    free(l->service);
    free(l->type);
    free(l);
}

void metro_free(MetroNetwork *m) {
    if (!m) return;
    for (int i = 0; i < m->nstations; i++) {
        free(m->stations[i].id);
        free(m->stations[i].name);
    }
    free(m->stations);
    for (int i = 0; i < m->nlines; i++) {
        free(m->lines[i].id);
        free(m->lines[i].name);
        free(m->lines[i].color);
        free(m->lines[i].stations);
    }
    free(m->lines);
    for (int i = 0; i < m->ntrips; i++) {
        free(m->trips[i].id);
        free(m->trips[i].calls);
    }
    free(m->trips);
    if (m->deps) {
        size_t n = (size_t)m->nservices * (size_t)m->nlines * 2 * (size_t)m->nstations;
        for (size_t i = 0; i < n; i++) free(m->deps[i].d);
        free(m->deps);
    }
    for (int i = 0; i < m->nservices; i++) free(m->services[i]);
    free(m->services);
    free(m->calendar);
    for (int i = 0; i < m->ntimeframes; i++) free(m->timeframes[i].group);
    free(m->timeframes);
    sm_free(m->station_index, NULL);
    sm_free(m->line_index, NULL);
    sm_free(m->service_index, NULL);
    sm_free(m->exceptions, free_exceptions);
    sm_free(m->token_fare, free);
    sm_free(m->card_fare, free);
    sm_free(m->hop_km, free);
    graph_free(m->graph);
    free(m->node_station);
    free(m->node_line);
    free(m);
}

/* ============================================================ calendar */

/* Which services run on a date: calendar.txt, then the exceptions in calendar_dates.txt. */
static void services_on(const MetroNetwork *m, long days, char *active) {
    memset(active, 0, (size_t)m->nservices);
    time_t t = (time_t)days * 86400;
    struct tm tm;
    gmtime_r(&t, &tm);
    char key[16];
    snprintf(key, sizeof key, "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    int weekday = (tm.tm_wday + 6) % 7;
    for (int i = 0; i < m->ncalendar; i++) {
        const CalRow *row = &m->calendar[i];
        if (row->days[weekday] && strcmp(row->start, key) <= 0 && strcmp(key, row->end) <= 0) active[row->service] = 1;
    }
    const ExceptionList *list = sm_get(m->exceptions, key);
    for (int i = 0; list && i < list->n; i++) {
        if (list->type[i] == '1') active[list->service[i]] = 1;
        else if (list->type[i] == '2') active[list->service[i]] = 0;
    }
}

static long ist_days(time_t when) {
    Clock c;
    ist_clock(when, &c);
    return days_from_civil(c.year, c.month, c.day);
}

static const char *timeframe(const MetroNetwork *m, time_t when) {
    Clock c;
    ist_clock(when, &c);
    char *active = xmalloc((size_t)m->nservices + 1);
    services_on(m, days_from_civil(c.year, c.month, c.day), active);
    char clock[16];
    snprintf(clock, sizeof clock, "%02d:%02d:%02d", c.hour, c.minute, c.second);
    const char *group = "offpeak";
    for (int i = 0; i < m->ntimeframes; i++) {
        const Timeframe *tf = &m->timeframes[i];
        if (tf->service >= 0 && active[tf->service] && strcmp(tf->start, clock) <= 0 && strcmp(clock, tf->end) < 0) {
            group = tf->group;
            break;
        }
    }
    free(active);
    return group;
}

long metro_feed_days_left(const MetroNetwork *m, time_t now, int *has) {
    *has = m->has_feed_end;
    return m->has_feed_end ? m->feed_end_days - ist_days(now) : 0;
}

/* ============================================================ queries */

typedef struct {
    double km;
    int station;
} Ranked;

static int ranked_cmp(const void *a, const void *b) {
    const Ranked *x = a, *y = b;
    if (x->km != y->km) return x->km < y->km ? -1 : 1;
    return (x->station > y->station) - (x->station < y->station);   /* keeps the sort stable */
}

/* k closest stations by straight-line distance, within max_km. */
int metro_nearest(const MetroNetwork *m, double lat, double lng, int k, double max_km, Near *out) {
    Ranked *all = xmalloc((size_t)m->nstations * sizeof *all);
    for (int s = 0; s < m->nstations; s++)
        all[s] = (Ranked){haversine_km(lat, lng, m->stations[s].lat, m->stations[s].lng), s};
    qsort(all, (size_t)m->nstations, sizeof *all, ranked_cmp);
    int n = 0;
    for (int i = 0; i < k && i < m->nstations; i++)
        if (all[i].km <= max_km) out[n++] = (Near){all[i].station, py_round(all[i].km, 2)};
    free(all);
    return n;
}

void metro_fare(const MetroNetwork *m, int o, int d, time_t when, MetroFare *out) {
    out->timeframe = timeframe(m, when);
    char key[256];
    snprintf(key, sizeof key, "%s|%s", m->stations[o].id, m->stations[d].id);
    const double *token = get_number(m->token_fare, key);
    snprintf(key, sizeof key, "%s|%s|%s", m->stations[o].id, m->stations[d].id, out->timeframe);
    const double *card = get_number(m->card_fare, key);
    out->has_token = token != NULL;
    out->token = token ? *token : 0;
    out->has_card = card != NULL;
    out->card = card ? *card : 0;
}

/* ===================================================== timetable search */

typedef struct {
    int service, off;
} Offset;

/* Services to search: today's, plus yesterday's shifted back 24 h
   (GTFS writes a 00:20 train of yesterday's service as 24:20:00). */
static int service_offsets(const MetroNetwork *m, long day, Offset *out) {
    char *active = xmalloc((size_t)m->nservices + 1);
    int n = 0;
    services_on(m, day, active);
    for (int s = 0; s < m->nservices; s++)
        if (active[s]) out[n++] = (Offset){s, 0};
    services_on(m, day - 1, active);
    for (int s = 0; s < m->nservices; s++)
        if (active[s]) out[n++] = (Offset){s, -DAY};
    free(active);
    return n;
}

typedef struct {
    int ok;
    double dep;
    int trip, idx, off;
} Train;

static int train_less(const MetroNetwork *m, const Train *a, const Train *b) {
    if (a->dep != b->dep) return a->dep < b->dep;
    int c = strcmp(m->trips[a->trip].id, m->trips[b->trip].id);
    if (c) return c < 0;
    if (a->idx != b->idx) return a->idx < b->idx;
    return a->off < b->off;
}

/* Earliest departure of `line` in `direction` from `station` at or after t.
   Binary search on the sorted departure list: O(log n). */
static Train next_train(const MetroNetwork *m, const Offset *offs, int noffs, int line, int dir, int station, double t) {
    Train best = {0};
    for (int i = 0; i < noffs; i++) {
        const DepList *l = deplist(m, offs[i].service, line, dir, station);
        if (!l->n) continue;
        double target = t - offs[i].off;
        int lo = 0, hi = l->n;                         /* bisect_left */
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (l->d[mid].dep < target) lo = mid + 1;
            else hi = mid;
        }
        if (lo < l->n) {
            Train cand = {1, (double)l->d[lo].dep + offs[i].off, l->d[lo].trip, l->d[lo].idx, offs[i].off};
            if (!best.ok || train_less(m, &cand, &best)) best = cand;
        }
    }
    return best;
}

typedef struct {
    double prio;
    int station, line;
    const char *sid, *lid;
} TItem;

static int titem_cmp(const void *x, const void *y) {
    const TItem *a = x, *b = y;
    if (a->prio != b->prio) return a->prio < b->prio ? -1 : 1;
    int c = strcmp(a->sid, b->sid);
    return c ? c : strcmp(a->lid, b->lid);
}

typedef struct {
    int prev;                    /* previous state, -1 for none */
    int ride;                    /* 1 = rode a train, 0 = changed line */
    int trip;
    double dep;
} TParent;

typedef struct {
    const MetroNetwork *m;
    int use_astar;
    double glat, glng;
} TimeCtx;

static double estimate(const TimeCtx *c, int station) {
    if (!c->use_astar) return 0.0;
    const Station *s = &c->m->stations[station];
    return haversine_km(s->lat, s->lng, c->glat, c->glng) / c->m->top_speed_km_s;
}

typedef struct {
    int found;
    double t0;
    double *best;
    TParent *parent;
    int pops;
} TimeResult;

static void search_timetable(const MetroNetwork *m, int o, int d, time_t depart, int use_astar, Trace *trace,
                             TimeResult *res) {
    Clock c;
    ist_clock(depart, &c);
    double t0 = c.hour * 3600 + c.minute * 60 + c.second;
    Offset offs[64];
    int noffs = service_offsets(m, days_from_civil(c.year, c.month, c.day), offs);
    TimeCtx ctx = {m, use_astar, m->stations[d].lat, m->stations[d].lng};

    int nl = m->nlines, nstates = m->nstations * nl;
    double *best = xmalloc((size_t)nstates * sizeof *best);
    TParent *parent = xmalloc((size_t)nstates * sizeof *parent);
    char *done = xcalloc((size_t)nstates, 1);
    for (int i = 0; i < nstates; i++) {
        best[i] = INFINITY;
        parent[i] = (TParent){-1, 0, -1, 0};
    }

    Heap heap;
    heap_init(&heap, sizeof(TItem), titem_cmp);
    const Station *origin = &m->stations[o];
    for (int q = 0; q < origin->nlines; q++) {
        int line = origin->lines[q];
        best[o * nl + line] = t0;
        heap_push(&heap, &(TItem){t0 + estimate(&ctx, o), o, line, origin->id, m->lines[line].id});
    }
    int pops = 0, found = -1;
    TItem it;
    while (heap_pop(&heap, &it)) {
        int node = it.station * nl + it.line;
        double t = best[node];
        if (done[node] || it.prio > t + estimate(&ctx, it.station) + 1e-9) continue;   /* stale entry */
        done[node] = 1;
        pops++;
        TraceEvent *ev = trace ? trace_add(trace, node, (t - t0) / 60) : NULL;
        if (it.station == d) {
            found = node;
            break;
        }
#define RELAX(STATION, LINE, ARRIVE, PARENT)                                                                  \
    do {                                                                                                       \
        int nxt_ = (STATION) * nl + (LINE);                                                                    \
        double arrive_ = (ARRIVE);                                                                             \
        if (arrive_ < best[nxt_]) {                                                                            \
            best[nxt_] = arrive_;                                                                              \
            parent[nxt_] = (PARENT);                                                                           \
            heap_push(&heap, &(TItem){arrive_ + estimate(&ctx, (STATION)), (STATION), (LINE),                  \
                                      m->stations[(STATION)].id, m->lines[(LINE)].id});                        \
            if (ev) trace_relaxed(ev, nxt_, (arrive_ - t0) / 60);                                               \
        }                                                                                                      \
    } while (0)

        for (int dir = 0; dir < 2; dir++) {
            Train train = next_train(m, offs, noffs, it.line, dir, it.station, t);
            if (!train.ok) continue;
            const Call *next = &m->trips[train.trip].calls[train.idx + 1];
            RELAX(next->station, it.line, (double)next->arr + train.off, ((TParent){node, 1, train.trip, train.dep}));
        }
        const Station *st = &m->stations[it.station];
        for (int q = 0; q < st->nlines; q++)
            if (st->lines[q] != it.line)
                RELAX(it.station, st->lines[q], t + INTERCHANGE_WALK_MIN * 60, ((TParent){node, 0, -1, 0}));
#undef RELAX
        if (ev) {
            size_t count = heap.count;
            TItem *copy = xmalloc((count ? count : 1) * sizeof *copy);
            memcpy(copy, heap.data, count * sizeof *copy);
            qsort(copy, count, sizeof *copy, titem_cmp);
            for (size_t k = 0; k < count && k < 12 && ev->nheap < 4; k++) {
                int v = copy[k].station * nl + copy[k].line;
                if (!done[v] && copy[k].prio <= best[v] + estimate(&ctx, copy[k].station) + 1e-9)
                    ev->heap[ev->nheap++] = (NodeCost){v, (best[v] - t0) / 60};
            }
            free(copy);
        }
    }
    heap_free(&heap);
    free(done);
    res->found = found;
    res->t0 = t0;
    res->best = best;
    res->parent = parent;
    res->pops = pops;
}

static Ride *add_ride(MetroJourney *j, int line, int first_station) {
    j->rides = xrealloc(j->rides, (size_t)(j->nrides + 1) * sizeof *j->rides);
    Ride *r = &j->rides[j->nrides++];
    memset(r, 0, sizeof *r);
    r->line = line;
    r->cap = 8;
    r->stations = xmalloc((size_t)r->cap * sizeof(int));
    r->stations[r->nstations++] = first_station;
    return r;
}

static void ride_add_station(Ride *r, int station) {
    if (r->nstations == r->cap) {
        r->cap *= 2;
        r->stations = xrealloc(r->stations, (size_t)r->cap * sizeof(int));
    }
    r->stations[r->nstations++] = station;
}

static double hop_km(const MetroNetwork *m, int a, int b, int line) {
    char key[256];
    snprintf(key, sizeof key, "%s|%s|%s", m->stations[a].id, m->stations[b].id, m->lines[line].id);
    const double *km = get_number(m->hop_km, key);
    if (km && *km) return *km;
    return haversine_km(m->stations[a].lat, m->stations[a].lng, m->stations[b].lat, m->stations[b].lng);
}

/* Earliest arrival using the real timetable, leaving the origin at `depart`.
   Returns rides with real board/alight clock times, or NULL if no train can make the trip. */
MetroJourney *metro_journey(const MetroNetwork *m, int o, int d, time_t depart, int use_astar) {
    if (o == d) return NULL;
    TimeResult r;
    search_timetable(m, o, d, depart, use_astar, NULL, &r);
    MetroJourney *j = NULL;
    if (r.found >= 0) {
        int nl = m->nlines;
        int nhops = 0;
        for (int v = r.found; r.parent[v].prev >= 0; v = r.parent[v].prev) nhops++;
        int *hops = xmalloc((size_t)(nhops ? nhops : 1) * sizeof(int));
        int k = nhops;
        for (int v = r.found; r.parent[v].prev >= 0; v = r.parent[v].prev) hops[--k] = v;

        j = xcalloc(1, sizeof *j);
        j->origin = o;
        j->destination = d;
        for (int i = 0; i < nhops; i++) {
            int nxt = hops[i];
            const TParent *p = &r.parent[nxt];
            if (!p->ride) continue;
            int a = p->prev / nl, line = p->prev % nl, b = nxt / nl;
            Ride *ride = NULL;
            if (j->nrides) {
                Ride *last = &j->rides[j->nrides - 1];
                if (last->line == line && last->stations[last->nstations - 1] == a) ride = last;
            }
            if (!ride) {
                ride = add_ride(j, line, a);
                ride->board_s = p->dep;
                ride->timed = 1;
            }
            ride_add_station(ride, b);
            ride->km += hop_km(m, a, b, line);
            ride->alight_s = r.best[nxt];
        }
        free(hops);
        double first_board = j->nrides ? j->rides[0].board_s : r.t0;
        double total_km = 0;
        for (int i = 0; i < j->nrides; i++) {
            Ride *ride = &j->rides[i];
            ride->min = py_round((ride->alight_s - ride->board_s) / 60, 1);
            ride->km = py_round(ride->km, 2);
            total_km += ride->km;
        }
        double arrive_s = r.best[r.found];
        j->km = py_round(total_km, 2);
        j->min = py_round((arrive_s - r.t0) / 60, 1);       /* includes the real wait for trains */
        j->first_wait_min = py_round((first_board - r.t0) / 60, 1);
        j->depart_s = r.t0;
        j->arrive_s = arrive_s;
        j->pops = r.pops;
        j->timetable = 1;
    }
    free(r.best);
    free(r.parent);
    return j;
}

/* Quickest journey on the static average-wait graph (or NULL). */
MetroJourney *metro_route(const MetroNetwork *m, int o, int d) {
    if (o == d) return NULL;
    char name[128];
    snprintf(name, sizeof name, "IN:%s", m->stations[o].id);
    int src = graph_index(m->graph, name);
    snprintf(name, sizeof name, "OUT:%s", m->stations[d].id);
    int dst = graph_index(m->graph, name);
    if (src < 0 || dst < 0) return NULL;
    PathResult *p = dijkstra(m->graph, src, dst, MODE_METRO, 1, NULL);
    if (!p) return NULL;
    MetroJourney *j = xcalloc(1, sizeof *j);
    j->origin = o;
    j->destination = d;
    for (int i = 0; i < p->nedges; i++) {
        int u = p->path[i], v = p->edges[i]->to;
        int lu = m->node_line[u], lv = m->node_line[v];
        if (lu < 0 || lv < 0 || lu != lv) continue;          /* IN:, OUT: or a line change */
        int a = m->node_station[u], b = m->node_station[v];
        Ride *ride = NULL;
        if (j->nrides) {
            Ride *last = &j->rides[j->nrides - 1];
            if (last->line == lu && last->stations[last->nstations - 1] == a) ride = last;
        }
        if (!ride) ride = add_ride(j, lu, a);
        ride_add_station(ride, b);
        ride->km += p->edges[i]->km;
        ride->min += p->edges[i]->min;
    }
    for (int i = 0; i < j->nrides; i++) {
        j->rides[i].km = py_round(j->rides[i].km, 2);
        j->rides[i].min = py_round(j->rides[i].min, 1);
    }
    j->km = py_round(p->km, 2);
    j->min = py_round(p->min, 1);                            /* includes average waiting and line changes */
    path_free(p);
    return j;
}

void metro_journey_free(MetroJourney *j) {
    if (!j) return;
    for (int i = 0; i < j->nrides; i++) free(j->rides[i].stations);
    free(j->rides);
    free(j);
}

int metro_next_departure(const MetroNetwork *m, int station, time_t depart, char out[8]) {
    Clock c;
    ist_clock(depart, &c);
    double t = c.hour * 3600 + c.minute * 60;
    Offset offs[64];
    int noffs = service_offsets(m, days_from_civil(c.year, c.month, c.day), offs);
    int have = 0;
    double soonest = 0;
    const Station *st = &m->stations[station];
    for (int q = 0; q < st->nlines; q++)
        for (int dir = 0; dir < 2; dir++) {
            Train tr = next_train(m, offs, noffs, st->lines[q], dir, station, t);
            if (tr.ok && (!have || tr.dep < soonest)) {
                soonest = tr.dep;
                have = 1;
            }
        }
    if (have) metro_clock(soonest, out);
    return have;
}

/* ============================================================ JSON output */

void metro_write_station(const MetroNetwork *m, JW *w, int s) {
    const Station *st = &m->stations[s];
    jw_obj(w);
    jw_key(w, "id"), jw_str(w, st->id);
    jw_key(w, "name"), jw_str(w, st->name);
    jw_key(w, "lat"), jw_num(w, st->lat);
    jw_key(w, "lng"), jw_num(w, st->lng);
    jw_key(w, "lines"), jw_arr(w);
    for (int q = 0; q < st->nlines; q++) jw_str(w, m->lines[st->lines[q]].id);
    jw_arr_end(w);
    jw_obj_end(w);
}

void metro_write_ride(const MetroNetwork *m, JW *w, const Ride *r) {
    const Line *ln = &m->lines[r->line];
    jw_obj(w);
    jw_key(w, "line"), jw_str(w, ln->id);
    jw_key(w, "stations"), jw_arr(w);
    for (int i = 0; i < r->nstations; i++) jw_str(w, m->stations[r->stations[i]].id);
    jw_arr_end(w);
    jw_key(w, "distance_km"), jw_num(w, r->km);
    jw_key(w, "time_min"), jw_num(w, r->min);
    if (r->timed) {
        char clock[8];
        metro_clock(r->board_s, clock);
        jw_key(w, "depart"), jw_str(w, clock);
        metro_clock(r->alight_s, clock);
        jw_key(w, "arrive"), jw_str(w, clock);
    }
    jw_key(w, "line_name"), jw_str(w, ln->name);
    jw_key(w, "color"), jw_str(w, ln->color);
    jw_key(w, "from"), jw_str(w, m->stations[r->stations[0]].name);
    jw_key(w, "to"), jw_str(w, m->stations[r->stations[r->nstations - 1]].name);
    jw_key(w, "stops"), jw_int(w, r->nstations - 1);
    jw_key(w, "coords"), jw_arr(w);
    for (int i = 0; i < r->nstations; i++) {
        jw_arr(w);
        jw_num(w, m->stations[r->stations[i]].lat);
        jw_num(w, m->stations[r->stations[i]].lng);
        jw_arr_end(w);
    }
    jw_arr_end(w);
    jw_obj_end(w);
}

void metro_write_journey(const MetroNetwork *m, JW *w, const MetroJourney *j) {
    jw_obj(w);
    jw_key(w, "origin"), metro_write_station(m, w, j->origin);
    jw_key(w, "destination"), metro_write_station(m, w, j->destination);
    jw_key(w, "rides"), jw_arr(w);
    for (int i = 0; i < j->nrides; i++) metro_write_ride(m, w, &j->rides[i]);
    jw_arr_end(w);
    jw_key(w, "changes"), jw_int(w, j->nrides > 1 ? j->nrides - 1 : 0);
    jw_key(w, "distance_km"), jw_num(w, j->km);
    jw_key(w, "time_min"), jw_num(w, j->min);
    if (j->timetable) {
        char clock[8];
        jw_key(w, "first_wait_min"), jw_num(w, j->first_wait_min);
        metro_clock(j->depart_s, clock);
        jw_key(w, "depart"), jw_str(w, clock);
        metro_clock(j->arrive_s, clock);
        jw_key(w, "arrive"), jw_str(w, clock);
        jw_key(w, "pops"), jw_int(w, j->pops);
    }
    jw_key(w, "timetable"), jw_bool(w, j->timetable);
    jw_obj_end(w);
}

void metro_write_fare(JW *w, const MetroFare *f) {
    jw_obj(w);
    jw_key(w, "token");
    if (f->has_token) jw_num(w, f->token);
    else jw_null(w);
    jw_key(w, "smart_card");
    if (f->has_card) jw_num(w, f->card);
    else jw_null(w);
    jw_key(w, "timeframe"), jw_str(w, f->timeframe);
    jw_obj_end(w);
}

void metro_write_network(const MetroNetwork *m, JW *w) {
    jw_obj(w);
    jw_key(w, "stations"), jw_arr(w);
    for (int s = 0; s < m->nstations; s++) metro_write_station(m, w, s);
    jw_arr_end(w);
    jw_key(w, "lines"), jw_arr(w);
    for (int l = 0; l < m->nlines; l++) {
        const Line *ln = &m->lines[l];
        jw_obj(w);
        jw_key(w, "id"), jw_str(w, ln->id);
        jw_key(w, "name"), jw_str(w, ln->name);
        jw_key(w, "color"), jw_str(w, ln->color);
        jw_key(w, "stations"), jw_arr(w);
        for (int i = 0; i < ln->nstations; i++) jw_str(w, m->stations[ln->stations[i]].id);
        jw_arr_end(w);
        jw_key(w, "coords"), jw_arr(w);
        for (int i = 0; i < ln->nstations; i++) {
            jw_arr(w);
            jw_num(w, m->stations[ln->stations[i]].lat);
            jw_num(w, m->stations[ln->stations[i]].lng);
            jw_arr_end(w);
        }
        jw_arr_end(w);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    jw_obj_end(w);
}

static void write_node(JW *w, const MetroNetwork *m, int station, int line, double t) {
    jw_obj(w);
    jw_key(w, "station"), jw_str(w, m->stations[station].id);
    jw_key(w, "line"), jw_str(w, m->lines[line].id);
    jw_key(w, "t"), jw_num(w, py_round(t, 1));
    jw_obj_end(w);
}

typedef struct {
    const MetroNetwork *m;
    int goal;
} StaticCtx;

static double static_estimate(int node, void *ctx) {
    const StaticCtx *c = ctx;
    const Station *s = &c->m->stations[c->m->node_station[node]], *g = &c->m->stations[c->goal];
    return haversine_km(s->lat, s->lng, g->lat, g->lng) / (c->m->top_speed_km_s * 60);
}

/* Replay data for the website: every state the search settled, in order. Uses the real
   timetable at `when`; if no train runs then (late night) it falls back to the static graph. */
void metro_write_trace(const MetroNetwork *m, int o, int d, time_t when, int use_astar, JW *w) {
    int nl = m->nlines;
    Trace trace = {0};
    TimeResult r;
    search_timetable(m, o, d, when, use_astar, &trace, &r);
    int timetable = r.found >= 0;
    free(r.best);
    free(r.parent);

    jw_obj(w);
    jw_key(w, "from"), metro_write_station(m, w, o);
    jw_key(w, "to"), metro_write_station(m, w, d);
    jw_key(w, "events"), jw_arr(w);
    MetroJourney *route = NULL;
    if (timetable) {
        for (int i = 0; i < trace.n; i++) {
            const TraceEvent *ev = &trace.ev[i];
            int st = ev->node / nl, ln = ev->node % nl;
            jw_obj(w);
            jw_key(w, "station"), jw_str(w, m->stations[st].id);
            jw_key(w, "line"), jw_str(w, m->lines[ln].id);
            jw_key(w, "t"), jw_num(w, py_round(ev->cost, 1));
            jw_key(w, "heap"), jw_arr(w);
            for (int k = 0; k < ev->nheap; k++) write_node(w, m, ev->heap[k].node / nl, ev->heap[k].node % nl, ev->heap[k].t);
            jw_arr_end(w);
            jw_key(w, "frontier"), jw_arr(w);
            for (int k = 0; k < ev->nrelaxed; k++)
                if (ev->relaxed[k].node / nl != st)
                    write_node(w, m, ev->relaxed[k].node / nl, ev->relaxed[k].node % nl, ev->relaxed[k].t);
            jw_arr_end(w);
            jw_obj_end(w);
        }
        route = metro_journey(m, o, d, when, use_astar);
    } else {
        Trace st_trace = {0};
        char name[128];
        snprintf(name, sizeof name, "IN:%s", m->stations[o].id);
        int src = graph_index(m->graph, name);
        snprintf(name, sizeof name, "OUT:%s", m->stations[d].id);
        int dst = graph_index(m->graph, name);
        StaticCtx ctx = {m, d};
        PathResult *p = use_astar ? astar(m->graph, src, dst, MODE_METRO, static_estimate, &ctx, 1, &st_trace)
                                  : dijkstra(m->graph, src, dst, MODE_METRO, 1, &st_trace);
        path_free(p);
        for (int i = 0; i < st_trace.n; i++) {
            const TraceEvent *ev = &st_trace.ev[i];
            int line = m->node_line[ev->node];
            if (line < 0) continue;
            int station = m->node_station[ev->node];
            jw_obj(w);
            jw_key(w, "station"), jw_str(w, m->stations[station].id);
            jw_key(w, "line"), jw_str(w, m->lines[line].id);
            jw_key(w, "t"), jw_num(w, py_round(ev->cost, 1));
            jw_key(w, "heap"), jw_arr(w);
            for (int k = 0; k < ev->nheap; k++) {
                int v = ev->heap[k].node;
                if (m->node_line[v] >= 0) write_node(w, m, m->node_station[v], m->node_line[v], ev->heap[k].t);
            }
            jw_arr_end(w);
            jw_key(w, "frontier"), jw_arr(w);
            for (int k = 0; k < ev->nrelaxed; k++) {
                int v = ev->relaxed[k].node;
                if (m->node_line[v] >= 0 && m->node_station[v] != station)
                    write_node(w, m, m->node_station[v], m->node_line[v], ev->relaxed[k].t);
            }
            jw_arr_end(w);
            jw_obj_end(w);
        }
        trace_free(&st_trace);
        route = metro_route(m, o, d);
    }
    jw_arr_end(w);
    trace_free(&trace);

    jw_key(w, "route");
    if (route) metro_write_journey(m, w, route);
    else jw_null(w);
    metro_journey_free(route);
    jw_key(w, "timetable"), jw_bool(w, timetable);
    jw_key(w, "algorithm"), jw_str(w, use_astar ? "astar" : "dijkstra");
    Clock c;
    ist_clock(when, &c);
    char at[8];
    snprintf(at, sizeof at, "%02d:%02d", c.hour, c.minute);
    jw_key(w, "at"), jw_str(w, at);
    MetroFare fare;
    metro_fare(m, o, d, when, &fare);
    jw_key(w, "fare"), metro_write_fare(w, &fare);
    jw_key(w, "states"), jw_int(w, m->graph->n);
    jw_obj_end(w);
}
