/*
 * planner.c - see planner.h.
 */
#include "planner.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fares.h"
#include "json.h"
#include "routing.h"

static const char *const MODE_ORDER[] = {"walk", "bus", "metro", "auto", "bike_taxi", "cab"};

typedef struct {
    char mode[16];
    double fare, time_min;
    Buf body;                    /* the option's JSON, still open: "{...," without the closing brace */
} Option;

/* ---------------------------------------------------------------- deep links */

/* Python's float repr: always shows a decimal point (12.0, not 12). */
static void py_float(double x, char *out, size_t n) {
    num_repr(x, out, n);
    if (!strpbrk(out, ".en")) strncat(out, ".0", n - strlen(out) - 1);
}

/* urllib.parse.quote_plus: letters, digits and _.-~ stay; space -> +; the rest -> %XX. */
static void quote_plus(Buf *b, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || strchr("_.-~", *p)) buf_putc(b, (char)*p);
        else if (*p == ' ') buf_putc(b, '+');
        else buf_printf(b, "%%%02X", *p);
    }
}

/* json.dumps(..., ensure_ascii=True): anything outside ASCII becomes \uXXXX. */
static void json_ascii(Buf *b, const char *s) {
    buf_putc(b, '"');
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned cp = *p;
        int len = 1;
        if (cp >= 0xF0 && p[1] && p[2] && p[3]) {
            cp = ((cp & 7u) << 18) | ((p[1] & 63u) << 12) | ((p[2] & 63u) << 6) | (p[3] & 63u);
            len = 4;
        } else if (cp >= 0xE0 && p[1] && p[2]) {
            cp = ((cp & 15u) << 12) | ((p[1] & 63u) << 6) | (p[2] & 63u);
            len = 3;
        } else if (cp >= 0xC0 && p[1]) {
            cp = ((cp & 31u) << 6) | (p[1] & 63u);
            len = 2;
        }
        if (cp == '"') buf_puts(b, "\\\"");
        else if (cp == '\\') buf_puts(b, "\\\\");
        else if (cp == '\n') buf_puts(b, "\\n");
        else if (cp == '\r') buf_puts(b, "\\r");
        else if (cp == '\t') buf_puts(b, "\\t");
        else if (cp < 0x20 || (cp >= 0x7F && cp < 0x10000)) buf_printf(b, "\\u%04x", cp);
        else if (cp >= 0x10000) {
            cp -= 0x10000;
            buf_printf(b, "\\u%04x\\u%04x", 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
        } else buf_putc(b, (char)cp);
        p += len;
    }
    buf_putc(b, '"');
}

static void uber_place(Buf *b, const Place *p) {
    char lat[40], lng[40];
    py_float(p->lat, lat, sizeof lat);
    py_float(p->lng, lng, sizeof lng);
    Buf j;
    buf_init(&j);
    buf_printf(&j, "{\"latitude\":%s,\"longitude\":%s,\"addressLine1\":", lat, lng);
    json_ascii(&j, p->name);
    buf_putc(&j, '}');
    quote_plus(b, j.s);
    buf_free(&j);
}

/* Uber universal link with pickup and drop pre-filled. */
static char *uber_link(const Place *o, const Place *d) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "https://m.uber.com/looking?pickup=");
    uber_place(&b, o);
    buf_puts(&b, "&drop%5B0%5D=");
    uber_place(&b, d);
    return buf_steal(&b);
}

/* Google Maps directions URL (no API key needed). */
static char *maps_link(const Place *o, const Place *d, const char *travelmode) {
    char a[40], b2[40], pair[100];
    Buf b;
    buf_init(&b);
    buf_puts(&b, "https://www.google.com/maps/dir/?api=1&origin=");
    py_float(o->lat, a, sizeof a);
    py_float(o->lng, b2, sizeof b2);
    snprintf(pair, sizeof pair, "%s,%s", a, b2);
    quote_plus(&b, pair);
    buf_puts(&b, "&destination=");
    py_float(d->lat, a, sizeof a);
    py_float(d->lng, b2, sizeof b2);
    snprintf(pair, sizeof pair, "%s,%s", a, b2);
    quote_plus(&b, pair);
    buf_printf(&b, "&travelmode=%s", travelmode);
    return buf_steal(&b);
}

static void add_link(JW *w, const char *label, const char *url) {
    jw_obj(w);
    jw_key(w, "label"), jw_str(w, label);
    jw_key(w, "url"), jw_str(w, url);
    jw_obj_end(w);
}

/* ------------------------------------------------------------------ options */

static int mode_order(const char *mode) {
    for (int i = 0; i < 6; i++)
        if (strcmp(MODE_ORDER[i], mode) == 0) return i;
    return 6;
}

static void rank_key(const Option *o, int priority, double key[3]) {
    double order = mode_order(o->mode);
    if (priority == PRI_FASTEST) key[0] = o->time_min, key[1] = o->fare, key[2] = order;
    else if (priority == PRI_BALANCED)
        key[0] = py_round(o->fare + MINUTE_VALUE[PRI_BALANCED] * o->time_min, 6), key[1] = o->fare, key[2] = order;
    else key[0] = o->fare, key[1] = o->time_min, key[2] = order;
}

static int option_cmp(const Option *x, const Option *y, int priority) {
    double a[3], b[3];
    rank_key(x, priority, a);
    rank_key(y, priority, b);
    for (int i = 0; i < 3; i++) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

/* Starts an option's JSON: the fields every option has, up to fare_source. */
static void option_begin(Option *o, JW *w, const char *mode, const char *label, const char *providers, double fare,
                         const char *fare_kind, const char *fare_text, const char *fare_source) {
    snprintf(o->mode, sizeof o->mode, "%s", mode);
    o->fare = fare;
    buf_init(&o->body);
    jw_init(w, &o->body);
    jw_obj(w);
    jw_key(w, "mode"), jw_str(w, mode);
    jw_key(w, "label"), jw_str(w, label);
    jw_key(w, "providers"), jw_str(w, providers);
    jw_key(w, "fare"), jw_num(w, fare);
    jw_key(w, "fare_kind"), jw_str(w, fare_kind);
    jw_key(w, "fare_text"), jw_str(w, fare_text);
    jw_key(w, "fare_source"), jw_str(w, fare_source);
}

static void road_leg(JW *w, const char *kind, const Place *o, const Place *d, double km, double minutes,
                     const char *polyline) {
    jw_obj(w);
    jw_key(w, "kind"), jw_str(w, kind);
    jw_key(w, "from"), jw_str(w, o->name);
    jw_key(w, "to"), jw_str(w, d->name);
    jw_key(w, "distance_km"), jw_num(w, py_round(km, 2));
    jw_key(w, "time_min"), jw_num(w, minutes);
    jw_key(w, "polyline"), jw_str(w, polyline);
    jw_obj_end(w);
}

/* A one-leg option on the road route (walk, auto, cab, bike taxi, bus). */
static void road_option(Option *o, const char *mode, const char *label, const char *providers, double fare,
                        const char *fare_kind, const char *fare_text, const char *fare_source, double km,
                        double time_min, const Place *orig, const Place *dest, const Road *road, const char *note,
                        const char *l1, const char *u1, const char *l2, const char *u2) {
    JW w;
    option_begin(o, &w, mode, label, providers, fare, fare_kind, fare_text, fare_source);
    o->time_min = time_min;
    jw_key(&w, "distance_km"), jw_num(&w, km);
    jw_key(&w, "time_min"), jw_num(&w, time_min);
    jw_key(&w, "legs"), jw_arr(&w);
    road_leg(&w, mode, orig, dest, road->km, time_min, road->polyline);
    jw_arr_end(&w);
    jw_key(&w, "notes"), jw_arr(&w), jw_str(&w, note), jw_arr_end(&w);
    jw_key(&w, "links"), jw_arr(&w);
    add_link(&w, l1, u1);
    if (l2) add_link(&w, l2, u2);
    jw_arr_end(&w);
}

/* -------------------------------------------------------------------- metro */

typedef struct {
    double km, min;
} RoadPart;

typedef struct {
    char kind[8];                /* "walk" or "auto" */
    const char *from, *to;
    double km, min;
    long fare;
} AccessLeg;

/* Walk if short, otherwise take an auto. */
static AccessLeg access_leg(const char *place, const char *station, RoadPart road, int hour, int towards) {
    AccessLeg leg;
    leg.from = towards ? place : station;
    leg.to = towards ? station : place;
    leg.km = py_round(road.km, 2);
    if (road.km <= MAX_WALK_KM) {
        snprintf(leg.kind, sizeof leg.kind, "walk");
        leg.min = py_round((double)walk_minutes(road.km), 1);
        leg.fare = 0;
    } else {
        snprintf(leg.kind, sizeof leg.kind, "auto");
        leg.min = py_round(road.min, 1);
        leg.fare = auto_fare(road.km, hour, NULL);
    }
    return leg;
}

static void write_access(JW *w, const AccessLeg *leg) {
    jw_obj(w);
    jw_key(w, "kind"), jw_str(w, leg->kind);
    jw_key(w, "from"), jw_str(w, leg->from);
    jw_key(w, "to"), jw_str(w, leg->to);
    jw_key(w, "distance_km"), jw_num(w, leg->km);
    jw_key(w, "time_min"), jw_num(w, leg->min);
    if (strcmp(leg->kind, "auto") == 0) jw_key(w, "fare"), jw_int(w, leg->fare);
    jw_obj_end(w);
}

/* Fallback if the distance matrix call fails: straight line x 1.3 at 20 km/h. */
static RoadPart approx_road(double alat, double alng, double blat, double blng) {
    double km = haversine_km(alat, alng, blat, blng) * 1.3;
    return (RoadPart){py_round(km, 2), py_round(km / 20 * 60, 1)};
}

typedef struct {
    OlaClient *maps;
    const double (*origins)[2];
    int no;
    const double (*dests)[2];
    int nd;
    RoadCell *out;
    OlaError err;
    int rc;
} MatrixJob;

static void *matrix_thread(void *arg) {
    MatrixJob *j = arg;
    j->rc = ola_distance_matrix(j->maps, j->origins, j->no, j->dests, j->nd, j->out, &j->err);
    return NULL;
}

static time_t plus_minutes(time_t t, double minutes) {
    long long us = llround(minutes * 60.0 * 1e6);       /* like timedelta: rounded to microseconds */
    return t + (time_t)(us / 1000000);
}

/* Best metro journey for the priority using real train times. Returns 1 with an option, or 0 with a reason. */
static int plan_metro(OlaClient *maps, const MetroNetwork *metro, const Place *origin, const Place *dest,
                      time_t depart, int priority, Option *opt, char *reason, size_t reason_len) {
    Near starts[3], ends[3];
    int ns = metro_nearest(metro, origin->lat, origin->lng, 3, 6.0, starts);
    int ne = metro_nearest(metro, dest->lat, dest->lng, 3, 6.0, ends);
    if (!ns || !ne) {
        snprintf(reason, reason_len, "No metro station within 6 km of the start or the destination");
        return 0;
    }
    int same = 1;                                          /* same set of stations at both ends? */
    for (int i = 0; i < ns && same; i++) {
        int found = 0;
        for (int k = 0; k < ne; k++) found |= starts[i].station == ends[k].station;
        same = found;
    }
    for (int k = 0; k < ne && same; k++) {
        int found = 0;
        for (int i = 0; i < ns; i++) found |= starts[i].station == ends[k].station;
        same = found;
    }
    if (same) {
        snprintf(reason, reason_len, "Both places are next to the same stations, so the metro does not help");
        return 0;
    }

    /* Road distance to and from the stations: both lookups at once (each is a network round trip). */
    double o_point[1][2] = {{origin->lat, origin->lng}}, d_point[1][2] = {{dest->lat, dest->lng}};
    double start_pts[3][2], end_pts[3][2];
    for (int i = 0; i < ns; i++) start_pts[i][0] = metro->stations[starts[i].station].lat, start_pts[i][1] = metro->stations[starts[i].station].lng;
    for (int i = 0; i < ne; i++) end_pts[i][0] = metro->stations[ends[i].station].lat, end_pts[i][1] = metro->stations[ends[i].station].lng;
    RoadCell to_station[3], from_station[3];
    MatrixJob to_job = {maps, (const double(*)[2])o_point, 1, (const double(*)[2])start_pts, ns, to_station, {0}, 0};
    MatrixJob from_job = {maps, (const double(*)[2])end_pts, ne, (const double(*)[2])d_point, 1, from_station, {0}, 0};
    pthread_t th;
    int threaded = pthread_create(&th, NULL, matrix_thread, &from_job) == 0;
    matrix_thread(&to_job);
    if (threaded) pthread_join(th, NULL);
    else matrix_thread(&from_job);
    int approximate = to_job.rc != 0 || from_job.rc != 0;

    Clock c;
    ist_clock(depart, &c);
    int hour = c.hour;
    double minute_value = MINUTE_VALUE[priority];

    int have_best = 0, have_wait = 0, wait_station = -1;
    time_t wait_at = 0;
    double best_score[2] = {0, 0}, best_fare = 0, best_time = 0;
    AccessLeg best_first, best_last;
    MetroJourney *best_journey = NULL;
    MetroFare best_info;

    for (int i = 0; i < ns; i++) {
        const Station *a = &metro->stations[starts[i].station];
        RoadPart road_a = (!approximate && to_station[i].ok) ? (RoadPart){to_station[i].km, to_station[i].min}
                                                              : approx_road(origin->lat, origin->lng, a->lat, a->lng);
        AccessLeg first = access_leg(origin->name, a->name, road_a, hour, 1);
        time_t at_station = plus_minutes(depart, first.min);
        for (int k = 0; k < ne; k++) {
            if (starts[i].station == ends[k].station) continue;
            const Station *b = &metro->stations[ends[k].station];
            RoadPart road_b = (!approximate && from_station[k].ok) ? (RoadPart){from_station[k].km, from_station[k].min}
                                                                    : approx_road(b->lat, b->lng, dest->lat, dest->lng);
            MetroJourney *journey = metro_journey(metro, starts[i].station, ends[k].station, at_station, 0);
            if (!journey || journey->first_wait_min > MAX_TRAIN_WAIT_MIN) {
                if (!have_wait) {
                    have_wait = 1;
                    wait_station = starts[i].station;
                    wait_at = at_station;
                }
                metro_journey_free(journey);
                continue;
            }
            MetroFare info;
            metro_fare(metro, starts[i].station, ends[k].station, at_station, &info);
            if (!info.has_token) {
                metro_journey_free(journey);
                continue;
            }
            AccessLeg last = access_leg(dest->name, b->name, road_b, hour, 0);
            double total_fare = info.token + first.fare + last.fare;
            double total_time = first.min + journey->min + last.min;
            double score[2] = {total_fare + minute_value * total_time, total_time};
            if (!have_best || score[0] < best_score[0] || (score[0] == best_score[0] && score[1] < best_score[1])) {
                metro_journey_free(best_journey);
                have_best = 1;
                best_score[0] = score[0], best_score[1] = score[1];
                best_fare = total_fare, best_time = total_time;
                best_first = first, best_last = last;
                best_journey = journey;
                best_info = info;
            } else {
                metro_journey_free(journey);
            }
        }
    }

    if (!have_best) {
        if (have_wait) {
            char when[8];
            int has = metro_next_departure(metro, wait_station, wait_at, when);
            Clock at;
            ist_clock(wait_at, &at);
            int soon = 0;
            if (has) {
                int wh = 0, wm = 0;
                sscanf(when, "%d:%d", &wh, &wm);
                int diff = ((wh * 60 + wm) - (at.hour * 60 + at.minute)) % 1440;
                if (diff < 0) diff += 1440;
                soon = diff <= MAX_TRAIN_WAIT_MIN;
            }
            if (soon) {
                /* A train is coming, but the connections needed to finish the trip have stopped. */
                snprintf(reason, reason_len, "Too late to finish this trip by metro tonight (the last connection has gone)");
            } else if (has) {
                snprintf(reason, reason_len, "No metro train soon from %s. First train at %s", metro->stations[wait_station].name, when);
            } else {
                snprintf(reason, reason_len, "No metro train soon from %s. No more trains today", metro->stations[wait_station].name);
            }
            return 0;
        }
        snprintf(reason, reason_len, "No useful metro route between these places");
        return 0;
    }

    /* Notes */
    const MetroJourney *jn = best_journey;
    char clock[8], note[400];
    Buf first_note;
    buf_init(&first_note);
    metro_clock(jn->rides[0].board_s, clock);
    buf_printf(&first_note, "Real timetable: board at %s", clock);
    if (jn->first_wait_min >= 1) buf_printf(&first_note, " (about %ld min wait)", py_round_int(jn->first_wait_min));
    if (jn->nrides > 1) buf_printf(&first_note, ", %d line change(s)", jn->nrides - 1);
    metro_clock(jn->rides[jn->nrides - 1].alight_s, clock);
    buf_printf(&first_note, ", arrive %s.", clock);

    long auto_fares = best_first.fare + best_last.fare;
    char token[32], fare_text[96];
    snprintf(token, sizeof token, "%g", best_info.token);
    if (auto_fares) snprintf(fare_text, sizeof fare_text, "₹%s metro + ₹%ld auto", token, auto_fares);
    else snprintf(fare_text, sizeof fare_text, "₹%s metro", token);

    JW w;
    option_begin(opt, &w, "metro", "Namma Metro", "BMRCL", (double)py_round_int(best_fare), "official", fare_text,
                 "Official BMRCL station-to-station fare (Namma Metro GTFS)");
    opt->time_min = py_round(best_time, 1);
    double total_km = best_first.km + best_last.km;
    for (int r = 0; r < jn->nrides; r++) total_km += jn->rides[r].km;
    jw_key(&w, "distance_km"), jw_num(&w, py_round(total_km, 2));
    jw_key(&w, "time_min"), jw_num(&w, opt->time_min);
    jw_key(&w, "legs"), jw_arr(&w);
    write_access(&w, &best_first);
    for (int r = 0; r < jn->nrides; r++) {
        const Ride *ride = &jn->rides[r];
        const Line *ln = &metro->lines[ride->line];
        jw_obj(&w);
        jw_key(&w, "kind"), jw_str(&w, "metro");
        jw_key(&w, "from"), jw_str(&w, metro->stations[ride->stations[0]].name);
        jw_key(&w, "to"), jw_str(&w, metro->stations[ride->stations[ride->nstations - 1]].name);
        jw_key(&w, "distance_km"), jw_num(&w, py_round(ride->km, 2));
        jw_key(&w, "time_min"), jw_num(&w, py_round(ride->min, 1));
        jw_key(&w, "line"), jw_str(&w, ln->name);
        jw_key(&w, "color"), jw_str(&w, ln->color);
        jw_key(&w, "stops"), jw_int(&w, ride->nstations - 1);
        jw_key(&w, "coords"), jw_arr(&w);
        for (int s = 0; s < ride->nstations; s++) {
            jw_arr(&w);
            jw_num(&w, metro->stations[ride->stations[s]].lat);
            jw_num(&w, metro->stations[ride->stations[s]].lng);
            jw_arr_end(&w);
        }
        jw_arr_end(&w);
        jw_key(&w, "from_id"), jw_str(&w, metro->stations[ride->stations[0]].id);
        jw_key(&w, "to_id"), jw_str(&w, metro->stations[ride->stations[ride->nstations - 1]].id);
        metro_clock(ride->board_s, clock);
        jw_key(&w, "depart"), jw_str(&w, clock);
        metro_clock(ride->alight_s, clock);
        jw_key(&w, "arrive"), jw_str(&w, clock);
        jw_obj_end(&w);
    }
    write_access(&w, &best_last);
    jw_arr_end(&w);
    jw_key(&w, "notes"), jw_arr(&w);
    jw_str(&w, first_note.s);
    if (best_info.has_card && best_info.card != 0) {
        char card[32];
        snprintf(card, sizeof card, "%g", best_info.card);
        snprintf(note, sizeof note, "Smart card: ₹%s (%s) instead of ₹%s.", card, best_info.timeframe, token);
        jw_str(&w, note);
    }
    if (strcmp(best_first.kind, "auto") == 0 || strcmp(best_last.kind, "auto") == 0)
        jw_str(&w, "Includes auto fare (meter rate) to/from the station.");
    if (approximate) jw_str(&w, "Station access distances are approximate (map service unavailable).");
    jw_arr_end(&w);
    char *maps_url = maps_link(origin, dest, "transit");
    jw_key(&w, "links"), jw_arr(&w);
    add_link(&w, "Directions in Google Maps", maps_url);
    jw_arr_end(&w);
    free(maps_url);
    buf_free(&first_note);
    metro_journey_free(best_journey);
    return 1;
}

/* --------------------------------------------------------------- trip plan */

typedef struct {
    OlaClient *maps;
    const MetroNetwork *metro;
    const Place *origin, *dest;
    time_t depart;
    int priority;
    Option option;
    int ok;
    char reason[200];
} MetroJob;

static void *metro_thread(void *arg) {
    MetroJob *j = arg;
    j->ok = plan_metro(j->maps, j->metro, j->origin, j->dest, j->depart, j->priority, &j->option, j->reason,
                       sizeof j->reason);
    return NULL;
}

static void write_place(JW *w, const Place *p) {
    jw_obj(w);
    jw_key(w, "name"), jw_str(w, p->name);
    jw_key(w, "lat"), jw_num(w, p->lat);
    jw_key(w, "lng"), jw_num(w, p->lng);
    jw_obj_end(w);
}

int plan_trip(OlaClient *maps, const MetroNetwork *metro, const Place *origin, const Place *dest, time_t depart,
              int priority, Buf *out, char cheapest_mode[16], double *cheapest_fare, OlaError *err) {
    Clock dc;
    ist_clock(depart, &dc);
    int hour = dc.hour;

    /* The metro search needs no road data, so it runs while the road route is fetched. */
    MetroJob job = {maps, metro, origin, dest, depart, priority, {{0}, 0, 0, {0}}, 0, {0}};
    pthread_t th;
    int threaded = metro && pthread_create(&th, NULL, metro_thread, &job) == 0;
    Road road = {0};
    int road_rc = ola_directions(maps, origin->lat, origin->lng, dest->lat, dest->lng, &road, err);
    if (metro && threaded) pthread_join(th, NULL);
    else if (metro && road_rc == 0) metro_thread(&job);
    if (road_rc != 0) {
        if (job.ok) buf_free(&job.option.body);
        return -1;
    }
    double km = road.km, minutes = road.min;

    Option opts[6];
    int n = 0;
    char *uber = uber_link(origin, dest);

    if (km <= WALK_OPTION_MAX_KM) {
        char *walk_url = maps_link(origin, dest, "walking");
        char source[96];
        snprintf(source, sizeof source, "Walking at %g km/h along the road route.", WALK_KMPH);
        double walk_min = (double)walk_minutes(km);
        road_option(&opts[n++], "walk", "Walk", "On foot", 0, "free", "Free", source, km, walk_min, origin, dest, &road,
                    "Short trip: walking is free and often as quick as waiting for a ride.", "Walking directions",
                    walk_url, NULL, NULL);
        free(walk_url);
    }

    int night = 0;
    long auto_price = auto_fare(km, hour, &night);
    road_option(&opts[n++], "auto", "Auto", "Namma Yatri · meter autos", (double)auto_price, "regulated",
                night ? "Meter fare, night +50%" : "Meter fare", AUTO_SOURCE, km, minutes, origin, dest, &road,
                "Live travel time from Ola Maps (includes current traffic).", "Namma Yatri", "https://nammayatri.in/",
                "Uber", uber);
    road_option(&opts[n++], "cab", "Cab", "Uber · Ola", (double)cab_fare(km), "regulated",
                "App price may differ (surge, fees)", CAB_SOURCE, km, minutes, origin, dest, &road,
                "App prices may include surge and fees. Tap a button for the exact fare.", "Uber", uber, "Ola",
                "https://www.olacabs.com/");
    road_option(&opts[n++], "bike_taxi", "Bike taxi", "Rapido · Uber Moto", (double)bike_fare(km), "indicative",
                "Check the app for the actual price", BIKE_SOURCE, km, minutes, origin, dest, &road,
                "No official rate card exists. Check the app for the real price.", "Rapido", "https://www.rapido.bike/",
                "Uber", uber);

    int exact = 0;
    long bus_price = bmtc_fare(km, &exact);
    double bus_time = py_round(minutes * BUS_SLOWDOWN + BUS_AVG_WAIT_MIN, 1);
    char *bus_url = maps_link(origin, dest, "transit");
    char bus_note[200];
    snprintf(bus_note, sizeof bus_note,
             "Estimated: road time × %g + %g min average wait. Actual routes may need a change of bus.",
             BUS_SLOWDOWN, BUS_AVG_WAIT_MIN);
    road_option(&opts[n++], "bus", "BMTC bus", "BMTC ordinary", (double)bus_price, exact ? "official" : "estimated",
                exact ? "Stage fare" : "Estimated stage fare; check the BMTC chart", BMTC_SOURCE, km, bus_time, origin,
                dest, &road, bus_note, "Bus routes in Google Maps", bus_url, NULL, NULL);
    free(bus_url);
    free(uber);

    const char *unavailable_reason = NULL;
    if (!metro) unavailable_reason = "Metro data not loaded";
    else if (job.ok) opts[n++] = job.option;
    else unavailable_reason = job.reason;

    /* Stable insertion sort by the priority's key. */
    for (int i = 1; i < n; i++) {
        Option key = opts[i];
        int k = i - 1;
        while (k >= 0 && option_cmp(&opts[k], &key, priority) > 0) {
            opts[k + 1] = opts[k];
            k--;
        }
        opts[k + 1] = key;
    }
    int cheapest = 0, fastest = 0;
    for (int i = 1; i < n; i++) {
        if (option_cmp(&opts[i], &opts[cheapest], PRI_CHEAPEST) < 0) cheapest = i;
        if (option_cmp(&opts[i], &opts[fastest], PRI_FASTEST) < 0) fastest = i;
    }

    /* ---- the answer ---- */
    JW w;
    jw_init(&w, out);
    jw_obj(&w);
    jw_key(&w, "origin"), write_place(&w, origin);
    jw_key(&w, "destination"), write_place(&w, dest);
    time_t now = time(NULL);
    Clock nc;
    ist_clock(now, &nc);
    char stamp[40];
    snprintf(stamp, sizeof stamp, "%04d-%02d-%02dT%02d:%02d:%02d+05:30", nc.year, nc.month, nc.day, nc.hour, nc.minute, nc.second);
    jw_key(&w, "generated_at"), jw_str(&w, stamp);
    snprintf(stamp, sizeof stamp, "%04d-%02d-%02dT%02d:%02d+05:30", dc.year, dc.month, dc.day, dc.hour, dc.minute);
    jw_key(&w, "depart_at"), jw_str(&w, stamp);
    jw_key(&w, "priority"), jw_str(&w, PRIORITY_NAMES[priority]);
    jw_key(&w, "road"), jw_obj(&w);
    jw_key(&w, "distance_km"), jw_num(&w, km);
    jw_key(&w, "duration_min"), jw_num(&w, minutes);
    jw_key(&w, "polyline"), jw_str(&w, road.polyline);
    jw_obj_end(&w);
    jw_key(&w, "options"), jw_arr(&w);
    for (int i = 0; i < n; i++) {
        Buf *b = &opts[i].body;
        buf_printf(b, ",\"rank\":%d,\"id\":", i + 1);
        json_escape(b, opts[i].mode);
        buf_printf(b, ",\"is_cheapest\":%s,\"is_fastest\":%s}", i == cheapest ? "true" : "false",
                   i == fastest ? "true" : "false");
        jw_raw(&w, b->s);
    }
    jw_arr_end(&w);
    jw_key(&w, "unavailable"), jw_arr(&w);
    if (unavailable_reason) {
        jw_obj(&w);
        jw_key(&w, "mode"), jw_str(&w, "metro");
        jw_key(&w, "label"), jw_str(&w, "Namma Metro");
        jw_key(&w, "reason"), jw_str(&w, unavailable_reason);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_key(&w, "cheapest_mode"), jw_str(&w, opts[cheapest].mode);
    jw_key(&w, "fastest_mode"), jw_str(&w, opts[fastest].mode);
    jw_key(&w, "notes"), jw_arr(&w);
    jw_str(&w, "Uber, Ola, Rapido and Namma Yatri do not share live prices with other apps; "
               "road fares are estimates from published rates.");
    jw_str(&w, "Road times are live from Ola Maps. Metro times use the real BMRCL timetable (Namma Metro GTFS, ODbL).");
    jw_arr_end(&w);
    jw_obj_end(&w);

    snprintf(cheapest_mode, 16, "%s", opts[cheapest].mode);
    *cheapest_fare = opts[cheapest].fare;
    for (int i = 0; i < n; i++) buf_free(&opts[i].body);
    free(road.polyline);
    return 0;
}
