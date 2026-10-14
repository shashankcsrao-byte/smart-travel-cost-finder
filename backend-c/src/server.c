/*
 * server.c - the Smart Travel Cost Finder web backend, written in C.
 *
 * Run from the project folder:
 *     ./backend-c/build/smart-travel
 * Then open http://localhost:8000
 *
 * Request flow:
 *     Browser (web/app.js) --HTTP/JSON--> this server --> routing.c (Dijkstra, A*, Yen, mixed)
 *                                                    --> planner.c, metro.c, olamaps.c (live Bengaluru)
 *                                                    --> history.c (SQLite)
 *
 * Settings come from environment variables (and a .env file in the folder you start it from):
 *     PORT, HOST, OLA_MAPS_API_KEY, TRAVEL_DB_PATH, WEB_DIR, METRO_GTFS_ZIP,
 *     ALLOWED_HOSTS, RENDER_EXTERNAL_HOSTNAME, FORWARDED_ALLOW_IPS
 */
#include <ctype.h>
#include <curl/curl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "fares.h"
#include "graph.h"
#include "history.h"
#include "http.h"
#include "json.h"
#include "metro.h"
#include "olamaps.h"
#include "planner.h"
#include "ratelimit.h"
#include "routing.h"
#include "strmap.h"
#include "util.h"

/* Live mode only covers the Bengaluru region (metro data and fare rules are local). */
#define LAT_MIN 12.70
#define LAT_MAX 13.30
#define LNG_MIN 77.30
#define LNG_MAX 77.90

#define CSP                                                                                                     \
    "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "                                \
    "img-src 'self' data: https://tile.openstreetmap.org https://*.tile.openstreetmap.org; "                   \
    "font-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'self'; form-action 'self'"
#define LONG_CACHE "public, max-age=31536000, immutable"
#define VENDOR_CACHE "public, max-age=604800"

/* Requests per minute per client IP for the endpoints that spend Ola Maps quota. */
#define LIMIT_PLACES 90
#define LIMIT_PLACE 60
#define LIMIT_PLAN 30
#define LIMIT_REVERSE 30

/* Our own files are linked as app.js?v=<hash of their contents>, so browsers may keep them for a
   year: a new deploy changes the hash and therefore the URL. */
static const char *const VERSIONED_ASSETS[] = {"styles.css", "motion.js", "app.js", "vendor/fonts.css",
                                               "vendor/leaflet/leaflet.css", "vendor/leaflet/leaflet.js"};

typedef struct {
    Graph *graph;
    History *history;
    OlaClient *maps;
    MetroNetwork *metro;
    char **hosts;
    int nhosts;
    RateLimiter *lim_places, *lim_place, *lim_plan, *lim_reverse;
    char *web_dir;
    char *index_html;
    char version[16];
} App;

/* ================================================================ helpers */

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    Buf b;
    buf_init(&b);
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_putn(&b, chunk, n);
    fclose(f);
    if (len) *len = b.len;
    return buf_steal(&b);
}

static void send_json(Response *res, Buf *b) {
    res->status = 200;
    buf_free(&res->body);
    res->body = *b;
    b->s = NULL;
    res_header(res, "Content-Type", "application/json");
}

/* ---- query validation, with the same messages FastAPI gives ---- */

typedef struct {
    Buf b;
    int count;
} Problems;

static void problem(Problems *p, const char *type, const char *name, const char *msg, const char *input) {
    if (p->count++ == 0) buf_puts(&p->b, "{\"detail\":[");
    else buf_putc(&p->b, ',');
    buf_printf(&p->b, "{\"type\":\"%s\",\"loc\":[\"query\",", type);
    json_escape(&p->b, name);
    buf_puts(&p->b, "],\"msg\":");
    json_escape(&p->b, msg);
    buf_puts(&p->b, ",\"input\":");
    if (input) json_escape(&p->b, input);
    else buf_puts(&p->b, "null");
    buf_putc(&p->b, '}');
}

static int problems_found(Problems *p, Response *res) {
    if (!p->count) {
        buf_free(&p->b);
        return 0;
    }
    buf_puts(&p->b, "]}");
    res_json(res, 422, p->b.s);
    buf_free(&p->b);
    return 1;
}

static const char *q_text(const Request *req, Problems *p, const char *name, int required, const char *fallback,
                          int min_len, int max_len) {
    const char *v = req_param(req, name);
    if (!v) {
        if (required) problem(p, "missing", name, "Field required", NULL);
        return fallback;
    }
    int n = utf8_chars(v);
    char msg[96];
    if (n < min_len) {
        snprintf(msg, sizeof msg, "String should have at least %d character%s", min_len, min_len == 1 ? "" : "s");
        problem(p, "string_too_short", name, msg, v);
    } else if (max_len && n > max_len) {
        snprintf(msg, sizeof msg, "String should have at most %d characters", max_len);
        problem(p, "string_too_long", name, msg, v);
    }
    return v;
}

static const char *q_choice(const Request *req, Problems *p, const char *name, const char *fallback,
                            const char *const *choices, int nchoices, const char *pattern) {
    const char *v = req_param(req, name);
    if (!v) return fallback;
    for (int i = 0; i < nchoices; i++)
        if (strcmp(v, choices[i]) == 0) return v;
    char msg[160];
    snprintf(msg, sizeof msg, "String should match pattern '%s'", pattern);
    problem(p, "string_pattern_mismatch", name, msg, v);
    return fallback;
}

static double q_float(const Request *req, Problems *p, const char *name) {
    const char *v = req_param(req, name);
    double x = 0;
    if (!v) problem(p, "missing", name, "Field required", NULL);
    else if (!parse_double(v, &x))
        problem(p, "float_parsing", name, "Input should be a valid number, unable to parse string as a number", v);
    return x;
}

static long q_int(const Request *req, Problems *p, const char *name, long fallback, long ge, long le) {
    const char *v = req_param(req, name);
    if (!v) return fallback;
    long x;
    char msg[96];
    if (!parse_long(v, &x)) {
        problem(p, "int_parsing", name, "Input should be a valid integer, unable to parse string as an integer", v);
        return fallback;
    }
    if (x < ge) {
        snprintf(msg, sizeof msg, "Input should be greater than or equal to %ld", ge);
        problem(p, "greater_than_equal", name, msg, v);
    } else if (x > le) {
        snprintf(msg, sizeof msg, "Input should be less than or equal to %ld", le);
        problem(p, "less_than_equal", name, msg, v);
    }
    return x;
}

static const char *const PRIORITY_CHOICES[] = {"cheapest", "balanced", "fastest"};
#define PRIORITY_PATTERN "^(cheapest|balanced|fastest)$"

static int ola_http_status(const OlaError *e) {
    return e->status == OLA_NOT_FOUND ? 404 : e->status == OLA_REJECTED ? 422 : 502;
}

/* 'HH:MM' (today, or tomorrow if that time has already passed by over an hour), or
   'YYYY-MM-DDTHH:MM[:SS][Z|+HH:MM]'. Returns 0 and the time, or -1 if it cannot be read. */
static int parse_depart(const char *text, time_t *out) {
    time_t now = time(NULL);
    if (!text) {
        *out = now;
        return 0;
    }
    char *t = collapse_spaces(text);
    int rc = -1;
    if (strchr(t, 'T')) {
        int y, mo, d, h, mi = 0, s = 0, used = 0;
        if (sscanf(t, "%4d-%2d-%2dT%2d%n", &y, &mo, &d, &h, &used) == 4) {
            const char *p = t + used;
            int ok = 1;
            if (*p == ':') {
                if (sscanf(p, ":%2d%n", &mi, &used) == 1) p += used;
                else ok = 0;
                if (ok && *p == ':') {
                    if (sscanf(p, ":%2d%n", &s, &used) == 1) p += used;
                    else ok = 0;
                    if (ok && (*p == '.' || *p == ',')) {
                        p++;
                        while (isdigit((unsigned char)*p)) p++;
                    }
                }
            }
            long offset = 0;
            int has_zone = 0;
            if (ok && (*p == 'Z' || *p == 'z')) {
                has_zone = 1;
                p++;
            } else if (ok && (*p == '+' || *p == '-')) {
                int oh = 0, om = 0;
                char sign = *p++;
                if (sscanf(p, "%2d:%2d%n", &oh, &om, &used) == 2 || sscanf(p, "%2d%2d%n", &oh, &om, &used) == 2) p += used;
                else ok = 0;
                offset = (oh * 3600L + om * 60L) * (sign == '-' ? -1 : 1);
                has_zone = 1;
            }
            long days_in[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
            if (ok && !*p && mo >= 1 && mo <= 12 && d >= 1 && d <= days_in[mo - 1] && !(mo == 2 && d == 29 && !leap) &&
                h >= 0 && h <= 23 && mi >= 0 && mi <= 59 && s >= 0 && s <= 59) {
                long secs = days_from_civil(y, mo, d) * 86400L + h * 3600L + mi * 60L + s;
                *out = has_zone ? (time_t)(secs - offset) : (time_t)(secs - IST_OFFSET);
                rc = 0;
            }
        }
    } else {
        char *colon = strchr(t, ':');
        long hh, mm;
        if (colon && !strchr(colon + 1, ':')) {
            *colon = '\0';
            if (parse_long(t, &hh) && parse_long(colon + 1, &mm) && hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59) {
                Clock c;
                ist_clock(now, &c);
                time_t when = ist_time(c.year, c.month, c.day, (int)hh, (int)mm, 0);
                if (when < now - 3600) when += 86400;
                *out = when;
                rc = 0;
            }
        }
    }
    free(t);
    return rc;
}

static int in_box(double lat, double lng) {
    return lat >= LAT_MIN && lat <= LAT_MAX && lng >= LNG_MIN && lng <= LNG_MAX;
}

/* 0 if the point is a real place inside Bengaluru; otherwise answers 422 and returns -1. */
static int check_point(Response *res, const char *name, double lat, double lng) {
    char msg[200];
    if (!isfinite(lat) || !isfinite(lng)) {
        snprintf(msg, sizeof msg, "%s: coordinates must be real numbers.", name);
        res_detail(res, 422, msg);
        return -1;
    }
    if (!in_box(lat, lng)) {
        snprintf(msg, sizeof msg, "%s is outside Bengaluru%s. Live mode covers Bengaluru only.", name,
                 in_box(lng, lat) ? " (latitude and longitude look swapped)" : "");
        res_detail(res, 422, msg);
        return -1;
    }
    return 0;
}

static int require_maps(App *app, Response *res) {
    if (app->maps) return 0;
    fprintf(stderr, "warning: live request without OLA_MAPS_API_KEY configured\n");
    res_detail(res, 503, "Live mode is not set up on this server yet.");
    return -1;
}

/* ============================================================ demo city */

static void api_health(App *app, Response *res) {
    Buf b;
    buf_init(&b);
    JW w;
    jw_init(&w, &b);
    jw_obj(&w);
    jw_key(&w, "status"), jw_str(&w, "ok");
    jw_key(&w, "locations"), jw_int(&w, app->graph->n);
    jw_key(&w, "connections"), jw_int(&w, DEMO_NCONN);
    jw_key(&w, "maps_configured"), jw_bool(&w, app->maps != NULL);
    jw_key(&w, "metro_loaded"), jw_bool(&w, app->metro != NULL);
    jw_key(&w, "metro_feed_days_left");
    int has = 0;
    long days = app->metro ? metro_feed_days_left(app->metro, time(NULL), &has) : 0;
    if (has) jw_int(&w, days);
    else jw_null(&w);
    jw_obj_end(&w);
    send_json(res, &b);
}

static void api_locations(Response *res) {
    Buf b;
    buf_init(&b);
    JW w;
    jw_init(&w, &b);
    jw_arr(&w);
    for (int i = 0; i < DEMO_NLOC; i++) {
        jw_obj(&w);
        jw_key(&w, "name"), jw_str(&w, DEMO_LOCATIONS[i]);
        jw_key(&w, "x"), jw_num(&w, DEMO_COORDS[i][0]);
        jw_key(&w, "y"), jw_num(&w, DEMO_COORDS[i][1]);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    send_json(res, &b);
}

static void api_fares(Response *res) {
    Buf b;
    buf_init(&b);
    JW w;
    jw_init(&w, &b);
    jw_arr(&w);
    for (int m = 0; m < NMODES; m++) {
        jw_obj(&w);
        jw_key(&w, "mode"), jw_str(&w, MODE_NAMES[m]);
        jw_key(&w, "label"), jw_str(&w, FARE_RULES[m].label);
        jw_key(&w, "base"), jw_num(&w, FARE_RULES[m].base);
        jw_key(&w, "per_km"), jw_num(&w, FARE_RULES[m].per_km);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    send_json(res, &b);
}

static void api_network(Response *res) {
    Buf b;
    buf_init(&b);
    JW w;
    jw_init(&w, &b);
    jw_arr(&w);
    for (int i = 0; i < DEMO_NCONN; i++) {
        const Connection *c = &DEMO_CONNECTIONS[i];
        jw_obj(&w);
        jw_key(&w, "from"), jw_str(&w, c->a);
        jw_key(&w, "to"), jw_str(&w, c->b);
        jw_key(&w, "mode"), jw_str(&w, MODE_NAMES[c->mode]);
        jw_key(&w, "label"), jw_str(&w, mode_label(c->mode));
        jw_key(&w, "distance_km"), jw_num(&w, c->km);
        jw_key(&w, "time_min"), jw_num(&w, c->min);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    send_json(res, &b);
}

/* A place name from the query: 422 if blank, 404 if unknown, else the vertex number. */
static int resolve(App *app, Response *res, const char *name, const char *label) {
    char *trimmed = collapse_spaces(name);
    char msg[160];
    if (!*trimmed) {
        snprintf(msg, sizeof msg, "%s cannot be empty", label);
        res_detail(res, 422, msg);
        free(trimmed);
        return -1;
    }
    free(trimmed);
    int v = find_location(app->graph, name);
    if (v < 0) {
        /* Show at most 60 printable characters of what was typed. */
        const char *s = name;
        while (isspace((unsigned char)*s)) s++;
        size_t n = strlen(s);
        while (n && isspace((unsigned char)s[n - 1])) n--;
        Buf shown;
        buf_init(&shown);
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)s[i] >= 0x20 && s[i] != 0x7F) buf_putc(&shown, s[i]);
        utf8_truncate(shown.s, 60);
        snprintf(msg, sizeof msg, "Unknown location: %s", shown.s);
        buf_free(&shown);
        res_detail(res, 404, msg);
    }
    return v;
}

static void write_path(JW *w, const Graph *g, const int *path, int n) {
    jw_arr(w);
    for (int i = 0; i < n; i++) jw_str(w, g->names[path[i]]);
    jw_arr_end(w);
}

static void write_option(JW *w, const Graph *g, const Journey *o, int rank, const char *cheapest_id,
                         const char *fastest_id) {
    char id[48];
    journey_id(o, id, sizeof id);
    jw_obj(w);
    jw_key(w, "rank"), jw_int(w, rank);
    jw_key(w, "id"), jw_str(w, id);
    jw_key(w, "mode"), jw_str(w, o->mode >= 0 ? MODE_NAMES[o->mode] : "mixed");
    jw_key(w, "variant"), jw_str(w, o->variant == VAR_MIXED ? "mixed" : o->variant == VAR_FASTEST ? "fastest" : "cheapest");
    jw_key(w, "label"), jw_str(w, o->label);
    jw_key(w, "path"), write_path(w, g, o->path, o->npath);
    jw_key(w, "distance_km"), jw_num(w, o->km);
    jw_key(w, "time_min"), jw_num(w, o->min);
    jw_key(w, "fare"), jw_num(w, o->fare);
    jw_key(w, "is_cheapest"), jw_bool(w, cheapest_id && strcmp(id, cheapest_id) == 0);
    jw_key(w, "is_fastest"), jw_bool(w, fastest_id && strcmp(id, fastest_id) == 0);
    jw_key(w, "legs"), jw_arr(w);
    for (int i = 0; i < o->nedges; i++) {
        const Edge *e = o->edges[i];
        jw_obj(w);
        jw_key(w, "from"), jw_str(w, g->names[o->path[i]]);
        jw_key(w, "to"), jw_str(w, g->names[e->to]);
        jw_key(w, "mode"), jw_str(w, MODE_NAMES[e->mode]);
        jw_key(w, "distance_km"), jw_num(w, e->km);
        jw_key(w, "time_min"), jw_num(w, e->min);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    if (o->mode >= 0) {
        jw_key(w, "fare_breakdown"), jw_obj(w);
        jw_key(w, "base"), jw_num(w, FARE_RULES[o->mode].base);
        jw_key(w, "per_km"), jw_num(w, FARE_RULES[o->mode].per_km);
        jw_obj_end(w);
    }
    if (o->nlegs) {
        jw_key(w, "parts"), jw_arr(w);
        for (int i = 0; i < o->nlegs; i++) {
            const MixLeg *leg = &o->legs[i];
            jw_obj(w);
            jw_key(w, "mode"), jw_str(w, MODE_NAMES[leg->mode]);
            jw_key(w, "label"), jw_str(w, mode_label(leg->mode));
            jw_key(w, "path"), write_path(w, g, leg->path, leg->npath);
            jw_key(w, "distance_km"), jw_num(w, leg->km);
            jw_key(w, "time_min"), jw_num(w, leg->min);
            jw_key(w, "fare"), jw_num(w, leg->fare);
            jw_obj_end(w);
        }
        jw_arr_end(w);
    }
    jw_obj_end(w);
}

static void api_routes(App *app, const Request *req, Response *res) {
    Problems p = {0};
    buf_init(&p.b);
    const char *source = q_text(req, &p, "source", 1, NULL, 1, 60);
    const char *destination = q_text(req, &p, "destination", 1, NULL, 1, 60);
    const char *priority = q_choice(req, &p, "priority", "cheapest", PRIORITY_CHOICES, 3, PRIORITY_PATTERN);
    if (problems_found(&p, res)) return;
    int src = resolve(app, res, source, "Source");
    if (src < 0) return;
    int dst = resolve(app, res, destination, "Destination");
    if (dst < 0) return;

    Comparison c;
    compare_routes(app->graph, src, dst, priority_from_name(priority), 1, &c);
    int ci = comparison_cheapest(&c), fi = comparison_fastest(&c);
    char cheapest_id[48] = "", fastest_id[48] = "";
    if (ci >= 0) journey_id(&c.opts[ci], cheapest_id, sizeof cheapest_id);
    if (fi >= 0) journey_id(&c.opts[fi], fastest_id, sizeof fastest_id);
    if (src != dst)
        history_add(app->history, app->graph->names[src], app->graph->names[dst], ci >= 0 ? cheapest_id : NULL, ci >= 0,
                    ci >= 0 ? c.opts[ci].fare : 0);

    Buf b;
    buf_init(&b);
    JW w;
    jw_init(&w, &b);
    jw_obj(&w);
    jw_key(&w, "source"), jw_str(&w, app->graph->names[src]);
    jw_key(&w, "destination"), jw_str(&w, app->graph->names[dst]);
    jw_key(&w, "same_location"), jw_bool(&w, src == dst);
    jw_key(&w, "priority"), jw_str(&w, priority);
    jw_key(&w, "options"), jw_arr(&w);
    for (int i = 0; i < c.nopts; i++)
        write_option(&w, app->graph, &c.opts[i], i + 1, ci >= 0 ? cheapest_id : NULL, fi >= 0 ? fastest_id : NULL);
    jw_arr_end(&w);
    jw_key(&w, "unavailable"), jw_arr(&w);
    for (int i = 0; i < c.nunavailable; i++) {
        jw_obj(&w);
        jw_key(&w, "mode"), jw_str(&w, MODE_NAMES[c.unavailable[i]]);
        jw_key(&w, "label"), jw_str(&w, mode_label(c.unavailable[i]));
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_key(&w, "cheapest_mode"), jw_str(&w, ci >= 0 ? cheapest_id : NULL);
    jw_key(&w, "fastest_mode"), jw_str(&w, fi >= 0 ? fastest_id : NULL);
    jw_key(&w, "notes"), jw_arr(&w);
    jw_str(&w, "Sample city data and illustrative fares, not live or official values.");
    jw_str(&w, "Journey time is the sum of connection times; waiting and traffic are not included.");
    jw_arr_end(&w);
    jw_obj_end(&w);
    comparison_free(&c);
    send_json(res, &b);
}

static void api_alternatives(App *app, const Request *req, Response *res) {
    static const char *const modes[] = {"bus", "metro", "bike_taxi", "car_taxi"};
    Problems p = {0};
    buf_init(&p.b);
    const char *source = q_text(req, &p, "source", 1, NULL, 1, 60);
    const char *destination = q_text(req, &p, "destination", 1, NULL, 1, 60);
    const char *mode_text = req_param(req, "mode");
    if (!mode_text) problem(&p, "missing", "mode", "Field required", NULL);
    else q_choice(req, &p, "mode", NULL, modes, 4, "^(bus|metro|bike_taxi|car_taxi)$");
    long k = q_int(req, &p, "k", 3, 1, 5);
    if (problems_found(&p, res)) return;
    int src = resolve(app, res, source, "Source");
    if (src < 0) return;
    int dst = resolve(app, res, destination, "Destination");
    if (dst < 0) return;
    int mode = mode_from_name(mode_text);

    Buf b;
    buf_init(&b);
    JW w;
    jw_init(&w, &b);
    jw_arr(&w);
    if (src != dst) {
        PathResult *routes[8];
        int n = k_shortest_paths(app->graph, src, dst, mode, (int)k, routes);
        for (int i = 0; i < n; i++) {
            jw_obj(&w);
            jw_key(&w, "rank"), jw_int(&w, i + 1);
            jw_key(&w, "path"), write_path(&w, app->graph, routes[i]->path, routes[i]->npath);
            jw_key(&w, "distance_km"), jw_num(&w, routes[i]->km);
            jw_key(&w, "time_min"), jw_num(&w, routes[i]->min);
            jw_key(&w, "fare"), jw_num(&w, calculate_fare(mode, routes[i]->km));
            jw_obj_end(&w);
            path_free(routes[i]);
        }
    }
    jw_arr_end(&w);
    send_json(res, &b);
}

static void api_history(App *app, const Request *req, Response *res) {
    Problems p = {0};
    buf_init(&p.b);
    long limit = q_int(req, &p, "limit", 8, 1, 50);
    if (problems_found(&p, res)) return;
    Buf b;
    buf_init(&b);
    JW w;
    jw_init(&w, &b);
    jw_obj(&w);
    jw_key(&w, "recent"), history_write_recent(app->history, &w, (int)limit);
    jw_key(&w, "popular"), history_write_popular(app->history, &w, 5);
    jw_obj_end(&w);
    send_json(res, &b);
}

/* ============================================================ live mode */

static void api_live_status(App *app, Response *res) {
    Buf b;
    buf_init(&b);
    JW w;
    jw_init(&w, &b);
    int has = 0;
    long days = app->metro ? metro_feed_days_left(app->metro, time(NULL), &has) : 0;
    jw_obj(&w);
    jw_key(&w, "maps_configured"), jw_bool(&w, app->maps != NULL);
    jw_key(&w, "metro_loaded"), jw_bool(&w, app->metro != NULL);
    jw_key(&w, "metro_stations"), jw_int(&w, app->metro ? app->metro->nstations : 0);
    jw_key(&w, "metro_lines"), jw_int(&w, app->metro ? app->metro->nlines : 0);
    jw_key(&w, "metro_states"), jw_int(&w, app->metro ? app->metro->graph->n : 0);
    jw_key(&w, "metro_feed_end");
    if (app->metro && app->metro->has_feed_end) {
        time_t t = (time_t)app->metro->feed_end_days * 86400;
        struct tm tm;
        gmtime_r(&t, &tm);
        char date[16];
        strftime(date, sizeof date, "%Y-%m-%d", &tm);
        jw_str(&w, date);
    } else jw_null(&w);
    jw_key(&w, "metro_feed_days_left");
    if (has) jw_int(&w, days);
    else jw_null(&w);
    jw_key(&w, "demo_locations"), jw_int(&w, app->graph->n);
    jw_key(&w, "demo_connections"), jw_int(&w, DEMO_NCONN);
    jw_key(&w, "map_calls");
    if (app->maps) {
        long today, total;
        ola_usage(app->maps, &today, &total);
        jw_obj(&w);
        jw_key(&w, "calls_today"), jw_int(&w, today);
        jw_key(&w, "calls_total"), jw_int(&w, total);
        jw_obj_end(&w);
    } else jw_null(&w);
    jw_key(&w, "rate_limits_per_minute"), jw_obj(&w);
    jw_key(&w, "/api/live/places"), jw_int(&w, LIMIT_PLACES);
    jw_key(&w, "/api/live/place"), jw_int(&w, LIMIT_PLACE);
    jw_key(&w, "/api/live/plan"), jw_int(&w, LIMIT_PLAN);
    jw_key(&w, "/api/live/reverse"), jw_int(&w, LIMIT_REVERSE);
    jw_obj_end(&w);
    jw_key(&w, "warnings"), jw_arr(&w);
    if (has && days < 30) {
        char msg[120];
        snprintf(msg, sizeof msg, "Metro timetable data expires in %ld days. Download a newer feed.", days);
        jw_str(&w, msg);
    }
    jw_arr_end(&w);
    jw_key(&w, "sources"), jw_arr(&w);
    jw_str(&w, "Places, road distance and live traffic times: Ola Maps");
    jw_str(&w, "Namma Metro stations, timetable and official fares: BMRCL via github.com/Vonter/bmrcl-gtfs (ODbL)");
    jw_str(&w, "Auto and cab fares: Govt. of Karnataka notified rates");
    jw_str(&w, "BMTC fares: first stages of the Jan 2025 revision, longer trips estimated");
    jw_arr_end(&w);
    jw_obj_end(&w);
    send_json(res, &b);
}

static void api_live_places(App *app, const Request *req, Response *res) {
    Problems p = {0};
    buf_init(&p.b);
    const char *q = q_text(req, &p, "q", 1, NULL, 1, 100);
    if (problems_found(&p, res) || require_maps(app, res)) return;
    char *json = NULL;
    OlaError err;
    if (ola_autocomplete(app->maps, q, &json, &err) != 0) {     /* short or symbol-only text costs nothing */
        res_detail(res, ola_http_status(&err), err.message);
        return;
    }
    res_json(res, 200, json);
    free(json);
}

static void api_live_place(App *app, const Request *req, Response *res) {
    Problems p = {0};
    buf_init(&p.b);
    const char *place_id = q_text(req, &p, "place_id", 1, NULL, 1, 300);
    if (problems_found(&p, res) || require_maps(app, res)) return;
    double lat, lng;
    OlaError err;
    if (ola_place_location(app->maps, place_id, &lat, &lng, &err) != 0) {
        res_detail(res, ola_http_status(&err), err.message);
        return;
    }
    Buf b;
    buf_init(&b);
    JW w;
    jw_init(&w, &b);
    jw_obj(&w);
    jw_key(&w, "lat"), jw_num(&w, lat);
    jw_key(&w, "lng"), jw_num(&w, lng);
    jw_obj_end(&w);
    send_json(res, &b);
}

static void api_live_reverse(App *app, const Request *req, Response *res) {
    Problems p = {0};
    buf_init(&p.b);
    double lat = q_float(req, &p, "lat"), lng = q_float(req, &p, "lng");
    if (problems_found(&p, res) || require_maps(app, res)) return;
    if (check_point(res, "That point", lat, lng)) return;
    char *name, *address;
    OlaError err;
    if (ola_reverse(app->maps, lat, lng, &name, &address, &err) != 0) {
        res_detail(res, ola_http_status(&err), err.message);
        return;
    }
    Buf b;
    buf_init(&b);
    JW w;
    jw_init(&w, &b);
    jw_obj(&w);
    jw_key(&w, "name"), jw_str(&w, name);
    jw_key(&w, "address"), jw_str(&w, address);
    jw_key(&w, "lat"), jw_num(&w, lat);
    jw_key(&w, "lng"), jw_num(&w, lng);
    jw_obj_end(&w);
    free(name);
    free(address);
    send_json(res, &b);
}

static void api_live_plan(App *app, const Request *req, Response *res) {
    Problems p = {0};
    buf_init(&p.b);
    double from_lat = q_float(req, &p, "from_lat"), from_lng = q_float(req, &p, "from_lng");
    double to_lat = q_float(req, &p, "to_lat"), to_lng = q_float(req, &p, "to_lng");
    const char *from_name = q_text(req, &p, "from_name", 0, "Start", 0, 120);
    const char *to_name = q_text(req, &p, "to_name", 0, "Destination", 0, 120);
    const char *priority = q_choice(req, &p, "priority", "cheapest", PRIORITY_CHOICES, 3, PRIORITY_PATTERN);
    const char *at = q_text(req, &p, "at", 0, NULL, 0, 20);
    if (problems_found(&p, res) || require_maps(app, res)) return;
    if (check_point(res, "The start", from_lat, from_lng) || check_point(res, "The destination", to_lat, to_lng)) return;
    if (fabs(from_lat - to_lat) < 1e-4 && fabs(from_lng - to_lng) < 1e-4) {
        res_detail(res, 422, "The start and destination are the same place.");
        return;
    }
    time_t depart;
    if (parse_depart(at, &depart) != 0) {
        res_detail(res, 422, "Departure time must look like 18:30");
        return;
    }
    Place origin = {collapse_spaces(from_name), from_lat, from_lng};
    Place dest = {collapse_spaces(to_name), to_lat, to_lng};
    if (!*origin.name) {
        free(origin.name);
        origin.name = xstrdup("Start");
    }
    if (!*dest.name) {
        free(dest.name);
        dest.name = xstrdup("Destination");
    }
    Buf b;
    buf_init(&b);
    char cheapest_mode[16];
    double cheapest_fare = 0;
    OlaError err;
    if (plan_trip(app->maps, app->metro, &origin, &dest, depart, priority_from_name(priority), &b, cheapest_mode,
                  &cheapest_fare, &err) != 0) {
        res_detail(res, ola_http_status(&err), err.message);
        buf_free(&b);
    } else {
        history_add_live(app->history, origin.name, origin.lat, origin.lng, dest.name, dest.lat, dest.lng, cheapest_mode,
                         cheapest_fare);
        send_json(res, &b);
    }
    free(origin.name);
    free(dest.name);
}

static void api_live_metro(App *app, Response *res) {
    Buf b;
    buf_init(&b);
    if (!app->metro) {
        buf_puts(&b, "{\"stations\":[],\"lines\":[]}");
    } else {
        JW w;
        jw_init(&w, &b);
        metro_write_network(app->metro, &w);
    }
    send_json(res, &b);
}

static void api_live_trace(App *app, const Request *req, Response *res) {
    static const char *const algorithms[] = {"dijkstra", "astar"};
    Problems p = {0};
    buf_init(&p.b);
    const char *from = q_text(req, &p, "from", 1, NULL, 1, 12);
    const char *to = q_text(req, &p, "to", 1, NULL, 1, 12);
    const char *at = q_text(req, &p, "at", 0, NULL, 0, 20);
    const char *algorithm = q_choice(req, &p, "algorithm", "dijkstra", algorithms, 2, "^(dijkstra|astar)$");
    if (problems_found(&p, res)) return;
    if (!app->metro) {
        res_detail(res, 503, "Metro data is not loaded");
        return;
    }
    char *a = collapse_spaces(from), *bname = collapse_spaces(to);
    for (char *s = a; *s; s++) *s = (char)toupper((unsigned char)*s);
    for (char *s = bname; *s; s++) *s = (char)toupper((unsigned char)*s);
    int o = metro_station(app->metro, a), d = metro_station(app->metro, bname);
    free(a);
    free(bname);
    if (o < 0 || d < 0) {
        res_detail(res, 404, "Unknown station");
        return;
    }
    if (o == d) {
        res_detail(res, 422, "Pick two different stations");
        return;
    }
    time_t when;
    if (parse_depart(at, &when) != 0) {
        res_detail(res, 422, "Departure time must look like 18:30");
        return;
    }
    Buf b;
    buf_init(&b);
    JW w;
    jw_init(&w, &b);
    metro_write_trace(app->metro, o, d, when, strcmp(algorithm, "astar") == 0, &w);
    send_json(res, &b);
}

static void fare_rule(JW *w, const char *mode, const char *label, const char *kind, const char *rule,
                      const char *example, const char *source) {
    jw_obj(w);
    jw_key(w, "mode"), jw_str(w, mode);
    jw_key(w, "label"), jw_str(w, label);
    jw_key(w, "kind"), jw_str(w, kind);
    jw_key(w, "rule"), jw_str(w, rule);
    jw_key(w, "example"), jw_str(w, example);
    jw_key(w, "source"), jw_str(w, source);
    jw_obj_end(w);
}

/* Live-mode fare rules with their sources, plus a worked 10 km example. */
static void api_live_fares(App *app, Response *res) {
    char metro_ex[96], auto_ex[64], cab_ex[64], bus_ex[64], bike_ex[64];
    if (app->metro) {
        int o = metro_station(app->metro, "IDN"), d = metro_station(app->metro, "MAGR");
        double token = 0;
        if (o >= 0 && d >= 0) {
            MetroFare f;
            metro_fare(app->metro, o, d, time(NULL), &f);
            if (f.has_token) token = f.token;
        }
        snprintf(metro_ex, sizeof metro_ex, "Indiranagar to MG Road: ₹%g", token);
    } else {
        snprintf(metro_ex, sizeof metro_ex, "Indiranagar to MG Road: n/a");
    }
    int exact;
    snprintf(auto_ex, sizeof auto_ex, "10 km by day: ₹%ld", auto_fare(10, 12, NULL));
    snprintf(cab_ex, sizeof cab_ex, "10 km: ₹%ld", cab_fare(10));
    snprintf(bus_ex, sizeof bus_ex, "10 km: about ₹%ld", bmtc_fare(10, &exact));
    snprintf(bike_ex, sizeof bike_ex, "10 km: ₹%ld", bike_fare(10));
    Buf b;
    buf_init(&b);
    JW w;
    jw_init(&w, &b);
    jw_arr(&w);
    fare_rule(&w, "metro", "Namma Metro", "official", "Fixed fare for each pair of stations (BMRCL table)", metro_ex,
              "Official BMRCL fare table in the Namma Metro GTFS feed");
    fare_rule(&w, "auto", "Auto", "regulated", "₹36 for the first 2 km, then ₹18 per km. +50% from 10 PM to 5 AM",
              auto_ex, AUTO_SOURCE);
    fare_rule(&w, "cab", "Cab", "regulated", "₹100 for the first 4 km, then ₹24 per km", cab_ex, CAB_SOURCE);
    fare_rule(&w, "bus", "BMTC bus", "estimated",
              "₹6, ₹12, ₹18 up to 6 km. Longer trips estimated (2020 slabs × 1.2)", bus_ex, BMTC_SOURCE);
    fare_rule(&w, "bike_taxi", "Bike taxi", "indicative", "₹20 + ₹8 per km (no official rate card)", bike_ex,
              BIKE_SOURCE);
    jw_arr_end(&w);
    send_json(res, &b);
}

static void api_live_history(App *app, const Request *req, Response *res) {
    Problems p = {0};
    buf_init(&p.b);
    long limit = q_int(req, &p, "limit", 6, 1, 30);
    if (problems_found(&p, res)) return;
    Buf b;
    buf_init(&b);
    JW w;
    jw_init(&w, &b);
    history_write_recent_live(app->history, &w, (int)limit);
    send_json(res, &b);
}

/* =========================================================== static files */

static const char *content_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    static const char *const types[][2] = {
        {".html", "text/html; charset=utf-8"}, {".css", "text/css; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"}, {".json", "application/json"},
        {".map", "application/json"}, {".svg", "image/svg+xml"}, {".png", "image/png"},
        {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".gif", "image/gif"}, {".webp", "image/webp"},
        {".ico", "image/vnd.microsoft.icon"}, {".woff2", "font/woff2"}, {".woff", "font/woff"},
        {".ttf", "font/ttf"}, {".otf", "font/otf"}, {".txt", "text/plain; charset=utf-8"},
        {".md", "text/markdown; charset=utf-8"}, {".xml", "application/xml"},
    };
    for (size_t i = 0; i < sizeof types / sizeof *types; i++)
        if (strcasecmp(dot, types[i][0]) == 0) return types[i][1];
    return "application/octet-stream";
}

static void serve_index(App *app, const Request *req, Response *res) {
    char etag[32];
    snprintf(etag, sizeof etag, "\"%s\"", app->version);
    const char *inm = req_header(req, "if-none-match");
    res_header(res, "ETag", etag);
    if (inm && strcmp(inm, etag) == 0) {
        res->status = 304;
        return;
    }
    res->status = 200;
    buf_puts(&res->body, app->index_html);
    res_header(res, "Content-Type", "text/html; charset=utf-8");
}

static void serve_static(App *app, const Request *req, Response *res) {
    const char *path = req->path;
    /* Refuse anything that could step outside the web folder. */
    if (path[0] != '/' || strstr(path, "..") || strchr(path, '\\') || strlen(path) > 1024) {
        res_detail(res, 404, "Not Found");
        return;
    }
    Buf full;
    buf_init(&full);
    buf_printf(&full, "%s%s", app->web_dir, path);
    struct stat st;
    if (stat(full.s, &st) == 0 && S_ISDIR(st.st_mode)) {
        buf_puts(&full, full.s[full.len - 1] == '/' ? "index.html" : "/index.html");
        if (stat(full.s, &st) != 0) st.st_mode = 0;
    }
    if (stat(full.s, &st) != 0 || !S_ISREG(st.st_mode)) {
        buf_free(&full);
        res_detail(res, 404, "Not Found");
        return;
    }
    char etag[64], modified[64];
    snprintf(etag, sizeof etag, "\"%lx-%llx\"", (unsigned long)st.st_mtime, (unsigned long long)st.st_size);
    struct tm tm;
    gmtime_r(&st.st_mtime, &tm);
    strftime(modified, sizeof modified, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    res_header(res, "ETag", etag);
    res_header(res, "Last-Modified", modified);
    const char *inm = req_header(req, "if-none-match");
    if (inm && strcmp(inm, etag) == 0) {
        res->status = 304;
        buf_free(&full);
        return;
    }
    size_t len = 0;
    char *data = read_file(full.s, &len);
    if (!data) {
        buf_free(&full);
        res_detail(res, 404, "Not Found");
        return;
    }
    res->status = 200;
    buf_putn(&res->body, data, len);
    free(data);
    res_header(res, "Content-Type", content_type(full.s));
    buf_free(&full);
}

/* index.html with ?v=<content hash> on every script and stylesheet link. */
static void build_index(App *app) {
    char path[1024];
    snprintf(path, sizeof path, "%s/index.html", app->web_dir);
    size_t len = 0;
    char *html = read_file(path, &len);
    if (!html) {
        fprintf(stderr, "warning: %s not found; the web page will not be served\n", path);
        app->index_html = xstrdup("<!doctype html><title>Smart Travel</title><p>web/index.html is missing.</p>");
        snprintf(app->version, sizeof app->version, "0");
        return;
    }
    unsigned long long h = fnv1a(html, len, 0);
    for (size_t i = 0; i < sizeof VERSIONED_ASSETS / sizeof *VERSIONED_ASSETS; i++) {
        snprintf(path, sizeof path, "%s/%s", app->web_dir, VERSIONED_ASSETS[i]);
        size_t n = 0;
        char *data = read_file(path, &n);
        if (data) h = fnv1a(data, n, h);
        free(data);
    }
    snprintf(app->version, sizeof app->version, "%012llx", h & 0xFFFFFFFFFFFFULL);
    Buf out;
    buf_init(&out);
    buf_puts(&out, html);
    for (size_t i = 0; i < sizeof VERSIONED_ASSETS / sizeof *VERSIONED_ASSETS; i++) {
        static const char *const attrs[] = {"src", "href"};
        for (int a = 0; a < 2; a++) {
            char from[256], to[256];
            snprintf(from, sizeof from, "%s=\"%s\"", attrs[a], VERSIONED_ASSETS[i]);
            snprintf(to, sizeof to, "%s=\"%s?v=%s\"", attrs[a], VERSIONED_ASSETS[i], app->version);
            Buf next;
            buf_init(&next);
            const char *s = out.s, *hit;
            while ((hit = strstr(s, from))) {
                buf_putn(&next, s, (size_t)(hit - s));
                buf_puts(&next, to);
                s = hit + strlen(from);
            }
            buf_puts(&next, s);
            buf_free(&out);
            out = next;
        }
    }
    free(html);
    app->index_html = buf_steal(&out);
}

/* ================================================================ security */

static void host_name(const char *host, char *out, size_t n) {
    out[0] = '\0';
    if (!host) return;
    if (host[0] == '[') {                              /* [::1]:8000 */
        const char *close = strchr(host, ']');
        size_t len = close ? (size_t)(close - host) + 1 : strlen(host);
        snprintf(out, n, "%.*s", (int)len, host);
    } else {
        snprintf(out, n, "%.*s", (int)strcspn(host, ":"), host);
    }
    str_lower(out);
}

/* Only answer to our own host names: blocks DNS-rebinding pages. */
static int host_allowed(App *app, const Request *req) {
    char host[256];
    host_name(req_header(req, "host"), host, sizeof host);
    for (int i = 0; i < app->nhosts; i++) {
        const char *h = app->hosts[i];
        if (strcmp(h, "*") == 0 || str_ieq(h, host)) return 1;
        if (h[0] == '*' && h[1] == '.') {
            size_t hl = strlen(host), sl = strlen(h + 1);
            if (hl > sl && str_ieq(host + hl - sl, h + 1)) return 1;
        }
    }
    return 0;
}

/* The hostname of an Origin header ("https://example.com:8000" -> "example.com"). */
static int origin_allowed(App *app, const char *origin) {
    const char *p = strstr(origin, "://");
    if (!p) return 0;
    p += 3;
    char host[256];
    if (*p == '[') {
        const char *close = strchr(p, ']');
        if (!close) return 0;
        snprintf(host, sizeof host, "%.*s", (int)(close - p - 1), p + 1);
    } else {
        snprintf(host, sizeof host, "%.*s", (int)strcspn(p, ":/?#"), p);
    }
    str_lower(host);
    if (!*host) return 0;
    for (int i = 0; i < app->nhosts; i++) {
        char bare[256];
        const char *h = app->hosts[i];
        size_t len = strlen(h);
        if (h[0] == '[' && len > 1 && h[len - 1] == ']') snprintf(bare, sizeof bare, "%.*s", (int)(len - 2), h + 1);
        else snprintf(bare, sizeof bare, "%s", h);
        if (str_ieq(bare, host)) return 1;
    }
    return 0;
}

/* ================================================================ routing */

typedef enum { GET_ONLY, GET_OR_DELETE } Methods;

static int method_ok(const Request *req, Response *res, Methods allowed) {
    const char *m = req->method;
    if (strcmp(m, "GET") == 0 || strcmp(m, "HEAD") == 0) return 1;
    if (allowed == GET_OR_DELETE && strcmp(m, "DELETE") == 0) return 1;
    res_header(res, "Allow", allowed == GET_OR_DELETE ? "GET, DELETE" : "GET");
    res_detail(res, 405, "Method Not Allowed");
    return 0;
}

static void route(App *app, const Request *req, Response *res) {
    const char *path = req->path;
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        if (method_ok(req, res, GET_ONLY)) serve_index(app, req, res);
        return;
    }
    if (strncmp(path, "/api/", 5) != 0) {
        if (method_ok(req, res, GET_ONLY)) serve_static(app, req, res);
        return;
    }
    if (strcmp(path, "/api/history") == 0) {
        if (!method_ok(req, res, GET_OR_DELETE)) return;
        if (strcmp(req->method, "DELETE") == 0) {
            history_clear(app->history);
            res_json(res, 200, "{\"cleared\":true}");
        } else {
            api_history(app, req, res);
        }
        return;
    }
    static const char *const get_routes[] = {
        "/api/health", "/api/locations", "/api/fares", "/api/network", "/api/routes", "/api/routes/alternatives",
        "/api/live/status", "/api/live/places", "/api/live/place", "/api/live/reverse", "/api/live/plan",
        "/api/live/metro", "/api/live/metro/trace", "/api/live/fares", "/api/live/history",
    };
    int which = -1;
    for (size_t i = 0; i < sizeof get_routes / sizeof *get_routes; i++)
        if (strcmp(path, get_routes[i]) == 0) which = (int)i;
    if (which < 0) {
        res_detail(res, 404, "Not Found");
        return;
    }
    if (!method_ok(req, res, GET_ONLY)) return;
    switch (which) {
    case 0: api_health(app, res); break;
    case 1: api_locations(res); break;
    case 2: api_fares(res); break;
    case 3: api_network(res); break;
    case 4: api_routes(app, req, res); break;
    case 5: api_alternatives(app, req, res); break;
    case 6: api_live_status(app, res); break;
    case 7: api_live_places(app, req, res); break;
    case 8: api_live_place(app, req, res); break;
    case 9: api_live_reverse(app, req, res); break;
    case 10: api_live_plan(app, req, res); break;
    case 11: api_live_metro(app, res); break;
    case 12: api_live_trace(app, req, res); break;
    case 13: api_live_fares(app, res); break;
    case 14: api_live_history(app, req, res); break;
    }
}

/* Every request passes through here first: the security guard, then the route, then headers. */
static void handle(const Request *req, Response *res, void *ctx) {
    App *app = ctx;
    const char *path = req->path;
    int is_api = strncmp(path, "/api/", 5) == 0;
    if (is_api) {
        /* Other websites must not be able to make this server spend quota or edit data. */
        const char *site = req_header(req, "sec-fetch-site");
        const char *origin = req_header(req, "origin");
        if ((site && strcmp(site, "cross-site") == 0) || (origin && *origin && !origin_allowed(app, origin))) {
            res_detail(res, 403, "Cross-site requests are not allowed");
            goto headers;
        }
        const char *m = req->method;
        const char *xrw = req_header(req, "x-requested-with");
        if (strcmp(m, "GET") && strcmp(m, "HEAD") && strcmp(m, "OPTIONS") && !(xrw && strcmp(xrw, "smart-travel") == 0)) {
            res_detail(res, 403, "Missing X-Requested-With header");
            goto headers;
        }
        RateLimiter *lim = NULL;
        if (strcmp(path, "/api/live/places") == 0) lim = app->lim_places;
        else if (strcmp(path, "/api/live/place") == 0) lim = app->lim_place;
        else if (strcmp(path, "/api/live/plan") == 0) lim = app->lim_plan;
        else if (strcmp(path, "/api/live/reverse") == 0) lim = app->lim_reverse;
        if (lim && !ratelimit_allow(lim, req->client)) {
            char retry[16];
            snprintf(retry, sizeof retry, "%d", ratelimit_retry_after(lim, req->client));
            res_detail(res, 429, "Too many requests. Please wait a moment.");
            res_header(res, "Retry-After", retry);
            goto headers;
        }
    }
    if (!host_allowed(app, req)) {
        res->status = 400;
        buf_puts(&res->body, "Invalid host header");
        res_header(res, "Content-Type", "text/plain; charset=utf-8");
    } else {
        route(app, req, res);
    }

headers:
    res_header_default(res, "X-Content-Type-Options", "nosniff");
    res_header_default(res, "Referrer-Policy", "strict-origin-when-cross-origin");
    res_header_default(res, "X-Frame-Options", "DENY");
    res_header_default(res, "Permissions-Policy", "geolocation=(self), camera=(), microphone=()");
    if (is_api) {
        res_header(res, "Cache-Control", "no-store");
    } else {
        const char *v = req_param(req, "v");
        if (res->status == 200 && v && strcmp(v, app->version) == 0) res_header(res, "Cache-Control", LONG_CACHE);
        else if (res->status == 200 && strncmp(path, "/vendor/", 8) == 0) res_header(res, "Cache-Control", VENDOR_CACHE);
        else res_header(res, "Cache-Control", "no-cache");      /* always revalidate (ETag) */
        res_header_default(res, "Content-Security-Policy", CSP);
    }
}

/* ==================================================================== main */

/* Read KEY=VALUE lines from .env into the environment (existing variables win). */
static void load_env_file(const char *path) {
    char *text = read_file(path, NULL);
    if (!text) return;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        char *eq = strchr(s, '=');
        if (!*s || *s == '#' || !eq) continue;
        *eq = '\0';
        char *key = s, *value = eq + 1;
        size_t kl = strlen(key);
        while (kl && isspace((unsigned char)key[kl - 1])) key[--kl] = '\0';
        while (isspace((unsigned char)*value)) value++;
        size_t vl = strlen(value);
        while (vl && isspace((unsigned char)value[vl - 1])) value[--vl] = '\0';
        if (vl >= 2 && (value[0] == '"' || value[0] == '\'') && value[vl - 1] == value[0]) {
            value[vl - 1] = '\0';
            value++;
        }
        setenv(key, value, 0);
    }
    free(text);
}

static const char *env_or(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return v && *v ? v : fallback;
}

static void add_host(App *app, const char *host) {
    char *h = collapse_spaces(host);
    if (!*h) {
        free(h);
        return;
    }
    app->hosts = xrealloc(app->hosts, (size_t)(app->nhosts + 1) * sizeof *app->hosts);
    app->hosts[app->nhosts++] = h;
}

int main(void) {
    setvbuf(stderr, NULL, _IOLBF, 0);
    load_env_file(".env");
    curl_global_init(CURL_GLOBAL_DEFAULT);

    App app = {0};
    app.graph = build_demo_graph();
    app.history = history_open(env_or("TRAVEL_DB_PATH", "travel_history.db"));
    if (!app.history) return 1;
    app.web_dir = xstrdup(env_or("WEB_DIR", "web"));
    build_index(&app);

    const char *key = getenv("OLA_MAPS_API_KEY");
    app.maps = ola_new(key, getenv("OLA_BASE_URL"));

    const char *gtfs = env_or("METRO_GTFS_ZIP", "data/bmrcl-gtfs/bmrcl.zip");
    char err[256] = "";
    double t0 = now_monotonic();
    app.metro = metro_load(gtfs, err, sizeof err);
    if (app.metro)
        fprintf(stderr, "Metro: %d stations, %d lines, %d trips, %d graph states (loaded in %.0f ms)\n",
                app.metro->nstations, app.metro->nlines, app.metro->ntrips, app.metro->graph->n,
                (now_monotonic() - t0) * 1000);
    else fprintf(stderr, "warning: metro data not loaded (%s)\n", err);

    char *hosts = xstrdup(env_or("ALLOWED_HOSTS", "localhost,127.0.0.1,[::1],testserver"));
    for (char *h = strtok(hosts, ","); h; h = strtok(NULL, ",")) add_host(&app, h);
    free(hosts);
    if (getenv("RENDER_EXTERNAL_HOSTNAME")) add_host(&app, getenv("RENDER_EXTERNAL_HOSTNAME"));

    app.lim_places = ratelimit_new(LIMIT_PLACES, 60);
    app.lim_place = ratelimit_new(LIMIT_PLACE, 60);
    app.lim_plan = ratelimit_new(LIMIT_PLAN, 60);
    app.lim_reverse = ratelimit_new(LIMIT_REVERSE, 60);

    long port = 8000;
    parse_long(env_or("PORT", "8000"), &port);
    const char *host = env_or("HOST", "127.0.0.1");              /* 0.0.0.0 on a server */
    int trust_proxy = strcmp(env_or("FORWARDED_ALLOW_IPS", ""), "*") == 0;
    fprintf(stderr, "Live mode: %s\n", app.maps ? "on (Ola Maps key found)" : "off (set OLA_MAPS_API_KEY)");
    fprintf(stderr, "Smart Travel Cost Finder (C) running at http://localhost:%ld\n", port);
    return http_serve(host, (int)port, handle, &app, trust_proxy) == 0 ? 0 : 1;
}
