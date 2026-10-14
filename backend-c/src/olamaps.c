/*
 * olamaps.c - see olamaps.h.
 */
#include "olamaps.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "json.h"
#include "util.h"

#define MAX_ANSWER (8u << 20)     /* refuse answers over 8 MB */

/* ----------------------------------------------------------- curl sharing */

static void share_lock(CURL *h, curl_lock_data data, curl_lock_access access, void *ctx) {
    (void)h, (void)access;
    pthread_mutex_lock(&((OlaClient *)ctx)->share_locks[data]);
}

static void share_unlock(CURL *h, curl_lock_data data, void *ctx) {
    (void)h;
    pthread_mutex_unlock(&((OlaClient *)ctx)->share_locks[data]);
}

OlaClient *ola_new(const char *api_key, const char *base_url) {
    if (!api_key || !*api_key) return NULL;
    OlaClient *c = xcalloc(1, sizeof *c);
    c->api_key = xstrdup(api_key);
    c->base_url = xstrdup(base_url && *base_url ? base_url : "https://api.olamaps.io");
    /* Place search results change rarely; routes use live traffic, so cache them briefly. */
    c->place_cache = cache_new(500, 6 * 3600);
    c->route_cache = cache_new(200, 60);
    c->miss_cache = cache_new(500, 600);           /* known "not found" answers */
    pthread_mutex_init(&c->count_lock, NULL);
    for (int i = 0; i < CURL_LOCK_DATA_LAST; i++) pthread_mutex_init(&c->share_locks[i], NULL);
    /* One shared pool: DNS answers, TLS sessions and open connections are reused across threads. */
    c->share = curl_share_init();
    curl_share_setopt(c->share, CURLSHOPT_LOCKFUNC, share_lock);
    curl_share_setopt(c->share, CURLSHOPT_UNLOCKFUNC, share_unlock);
    curl_share_setopt(c->share, CURLSHOPT_USERDATA, c);
    curl_share_setopt(c->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(c->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(c->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    return c;
}

void ola_free(OlaClient *c) {
    if (!c) return;
    curl_share_cleanup(c->share);
    for (int i = 0; i < CURL_LOCK_DATA_LAST; i++) pthread_mutex_destroy(&c->share_locks[i]);
    pthread_mutex_destroy(&c->count_lock);
    cache_free(c->place_cache);
    cache_free(c->route_cache);
    cache_free(c->miss_cache);
    free(c->api_key);
    free(c->base_url);
    free(c);
}

static long today_number(void) {
    Clock k;
    ist_clock(time(NULL), &k);
    return days_from_civil(k.year, k.month, k.day);
}

void ola_usage(OlaClient *c, long *today, long *total) {
    pthread_mutex_lock(&c->count_lock);
    long day = today_number();
    if (day != c->count_day) {
        c->count_day = day;
        c->calls_today = 0;
    }
    *today = c->calls_today;
    *total = c->calls_total;
    pthread_mutex_unlock(&c->count_lock);
}

/* ---------------------------------------------------------------- helpers */

static void set_error(OlaError *err, OlaStatus status, const char *message) {
    err->status = status;
    snprintf(err->message, sizeof err->message, "%s", message);
}

/* Place-search text with odd symbols removed and spaces collapsed:
   "  <script> MG   Road " -> "script MG Road". No place name needs angle brackets. */
char *clean_query(const char *text) {
    char *copy = xstrdup(text ? text : "");
    for (unsigned char *p = (unsigned char *)copy; *p; p++) {
        if (*p >= 0x80 || isalnum(*p) || *p == '_' || isspace(*p)) continue;   /* letters (any script), digits */
        if (strchr(".,'&/()#:-", *p)) continue;
        *p = ' ';
    }
    char *clean = collapse_spaces(copy);
    free(copy);
    return clean;
}

static void coord(double lat, double lng, char *out, size_t n) { snprintf(out, n, "%.6f,%.6f", lat, lng); }

static int truthy(const JVal *v) {
    if (!v) return 0;
    switch (v->type) {
    case J_NULL: return 0;
    case J_BOOL: return v->boolean;
    case J_NUM: return v->num != 0;
    case J_STR: return v->str[0] != '\0';
    default: return v->n > 0;
    }
}

/* Python's str() of a JSON value, for the few fields we show as text. */
static char *text_of(const JVal *v) {
    if (!v) return xstrdup("None");
    char num[64];
    switch (v->type) {
    case J_STR: return xstrdup(v->str);
    case J_NUM: num_repr(v->num, num, sizeof num); return xstrdup(num);
    case J_BOOL: return xstrdup(v->boolean ? "True" : "False");
    case J_NULL: return xstrdup("None");
    default: return xstrdup("");
    }
}

static char *strip(char *s) {
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    size_t n = strlen(start);
    while (n && isspace((unsigned char)start[n - 1])) n--;
    char *out = xstrndup(start, n);
    free(s);
    return out;
}

/* Accept 1234, "1234" or {"value": 1234}; a finite, non-negative number, or NAN if not usable. */
static double number_of(const JVal *v) {
    if (v && v->type == J_OBJ) v = jget(v, "value");
    if (!v) return NAN;
    double x;
    if (v->type == J_NUM) x = v->num;
    else if (v->type == J_STR && parse_double(v->str, &x)) {
    } else return NAN;
    return isfinite(x) && x >= 0 ? x : NAN;
}

static int coordinate_of(const JVal *v, double *out) {
    if (!v) return 0;
    if (v->type == J_NUM) {
        *out = v->num;
        return 1;
    }
    if (v->type == J_STR) return parse_double(v->str, out);
    return 0;
}

/* Find a (lat, lng) pair in a place object, whatever its exact shape. */
static int lat_lng(const JVal *obj, double *lat, double *lng) {
    if (!obj || obj->type != J_OBJ) return 0;
    const JVal *nodes[3] = {jget(jget(obj, "geometry"), "location"), jget(obj, "location"), obj};
    for (int i = 0; i < 3; i++) {
        const JVal *node = nodes[i];
        if (!node || node->type != J_OBJ) continue;
        const JVal *la = jget(node, "lat") ? jget(node, "lat") : jget(node, "latitude");
        const JVal *ln = jget(node, "lng") ? jget(node, "lng") : jget(node, "longitude");
        double a, b;
        if (!coordinate_of(la, &a) || !coordinate_of(ln, &b)) continue;
        if (isfinite(a) && isfinite(b) && a >= -90 && a <= 90 && b >= -180 && b <= 180) {
            *lat = a;
            *lng = b;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------- http */

static size_t on_data(char *ptr, size_t size, size_t nmemb, void *userdata) {
    Buf *b = userdata;
    size_t n = size * nmemb;
    if (b->len + n > MAX_ANSWER) return 0;      /* abort the transfer */
    buf_putn(b, ptr, n);
    return n;
}

static void uuid4(char out[37]) {
    unsigned char r[16];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(r, 1, 16, f) != 16) {
        for (int i = 0; i < 16; i++) r[i] = (unsigned char)(rand() & 0xFF);
    }
    if (f) fclose(f);
    r[6] = (unsigned char)((r[6] & 0x0F) | 0x40);
    r[8] = (unsigned char)((r[8] & 0x3F) | 0x80);
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2], r[3],
             r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}

/* params: key, value, key, value, ..., NULL */
static JVal *request(OlaClient *c, const char *method, const char *path, const char *const *params, OlaError *err) {
    pthread_mutex_lock(&c->count_lock);
    long day = today_number();
    if (day != c->count_day) {
        c->count_day = day;
        c->calls_today = 0;
    }
    c->calls_today++;
    c->calls_total++;
    pthread_mutex_unlock(&c->count_lock);

    CURL *curl = curl_easy_init();
    if (!curl) {
        set_error(err, OLA_ERROR, "Could not reach Ola Maps (NetworkError)");
        return NULL;
    }
    Buf url, body;
    buf_init(&url);
    buf_init(&body);
    buf_printf(&url, "%s%s?", c->base_url, path);
    for (int i = 0; params[i]; i += 2) {
        char *k = curl_easy_escape(curl, params[i], 0), *v = curl_easy_escape(curl, params[i + 1], 0);
        buf_printf(&url, "%s=%s&", k, v);
        curl_free(k);
        curl_free(v);
    }
    char *key = curl_easy_escape(curl, c->api_key, 0);
    buf_printf(&url, "api_key=%s", key);
    curl_free(key);

    char rid[37], header[64];
    uuid4(rid);
    snprintf(header, sizeof header, "X-Request-Id: %s", rid);
    struct curl_slist *headers = curl_slist_append(NULL, header);
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.s);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);              /* required in a threaded program */
    curl_easy_setopt(curl, CURLOPT_SHARE, c->share);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "smart-travel-c/2.0");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    }
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    buf_free(&url);                                 /* the URL holds the key: never logged */

    JVal *data = NULL;
    if (rc != CURLE_OK) {
        const char *kind = rc == CURLE_OPERATION_TIMEDOUT ? "Timeout"
                           : (rc == CURLE_COULDNT_CONNECT || rc == CURLE_COULDNT_RESOLVE_HOST) ? "ConnectError"
                                                                                                 : "NetworkError";
        char msg[100];
        snprintf(msg, sizeof msg, "Could not reach Ola Maps (%s)", kind);
        set_error(err, OLA_ERROR, msg);
    } else if (status == 401 || status == 403) {
        fprintf(stderr, "warning: Ola Maps refused %s with HTTP %ld (check OLA_MAPS_API_KEY)\n", path, status);
        set_error(err, OLA_ERROR, "The map service refused the request. The server's map key may be invalid.");
    } else if (status == 404) {
        set_error(err, OLA_NOT_FOUND, "Not found by the map service");
    } else if (status == 429) {
        set_error(err, OLA_ERROR, "The map service is busy. Please wait a moment and try again.");
    } else if (status == 400 || status == 422) {
        set_error(err, OLA_REJECTED, "The map service could not use that input");
    } else if (status >= 400) {
        char msg[64];
        snprintf(msg, sizeof msg, "Map service error %ld", status);
        set_error(err, OLA_ERROR, msg);
    } else {
        data = json_parse(body.s, body.len);
        if (!data) set_error(err, OLA_ERROR, "The map service returned an unreadable answer");
        else if (data->type != J_OBJ) {
            json_free(data);
            data = NULL;
            set_error(err, OLA_ERROR, "The map service returned an unexpected answer");
        }
    }
    buf_free(&body);
    return data;
}

/* ------------------------------------------------------------------ places */

int ola_autocomplete(OlaClient *c, const char *raw, char **json_out, OlaError *err) {
    char *text = clean_query(raw);
    if (utf8_chars(text) < 2) {                     /* nothing worth spending quota on */
        free(text);
        *json_out = xstrdup("[]");
        return 0;
    }
    Buf key;
    buf_init(&key);
    buf_puts(&key, "ac|");
    buf_puts(&key, text);
    str_lower(key.s);
    char *cached = cache_get(c->place_cache, key.s);
    if (cached) {
        free(text);
        buf_free(&key);
        *json_out = cached;
        return 0;
    }
    char near[64];
    coord(CITY_CENTRE_LAT, CITY_CENTRE_LNG, near, sizeof near);
    const char *params[] = {"input", text, "location", near, "radius", "60000", NULL};
    JVal *data = request(c, "GET", "/places/v1/autocomplete", params, err);
    free(text);
    if (!data) {
        buf_free(&key);
        return -1;
    }
    JVal *predictions = jget(data, "predictions");
    if (truthy(predictions) && predictions->type != J_ARR) {
        json_free(data);
        buf_free(&key);
        set_error(err, OLA_ERROR, "The map service returned an unexpected answer");
        return -1;
    }
    Buf out;
    buf_init(&out);
    JW w;
    jw_init(&w, &out);
    jw_arr(&w);
    for (int i = 0; truthy(predictions) && i < predictions->n; i++) {
        JVal *p = predictions->items[i];
        if (p->type != J_OBJ) continue;
        JVal *fmt = jget(p, "structured_formatting");
        if (fmt && fmt->type != J_OBJ) fmt = NULL;
        JVal *desc_v = jget(p, "description");
        char *description = truthy(desc_v) ? text_of(desc_v) : xstrdup("");
        double lat = 0, lng = 0;
        int has_point = lat_lng(p, &lat, &lng);
        char *name;
        if (truthy(jget(fmt, "main_text"))) name = text_of(jget(fmt, "main_text"));
        else name = xstrndup(description, strcspn(description, ","));
        name = strip(name);
        if (!*name) {
            free(name);
            free(description);
            continue;
        }
        utf8_truncate(name, 120);
        char *address = truthy(jget(fmt, "secondary_text")) ? text_of(jget(fmt, "secondary_text")) : xstrdup(description);
        utf8_truncate(address, 200);
        JVal *pid_v = jget(p, "place_id");
        char *place_id = truthy(pid_v) ? text_of(pid_v) : xstrdup("");
        utf8_truncate(place_id, 300);
        jw_obj(&w);
        jw_key(&w, "name"), jw_str(&w, name);
        jw_key(&w, "address"), jw_str(&w, address);
        jw_key(&w, "place_id"), jw_str(&w, *place_id ? place_id : NULL);
        jw_key(&w, "lat");
        if (has_point) jw_num(&w, lat);
        else jw_null(&w);
        jw_key(&w, "lng");
        if (has_point) jw_num(&w, lng);
        else jw_null(&w);
        jw_obj_end(&w);
        free(name);
        free(address);
        free(place_id);
        free(description);
    }
    jw_arr_end(&w);
    json_free(data);
    cache_set(c->place_cache, key.s, out.s);
    buf_free(&key);
    *json_out = buf_steal(&out);
    return 0;
}

int ola_place_location(OlaClient *c, const char *place_id, double *lat, double *lng, OlaError *err) {
    Buf key;
    buf_init(&key);
    buf_printf(&key, "pd|%s", place_id);
    char *cached = cache_get(c->place_cache, key.s);
    if (cached) {
        sscanf(cached, "%lf,%lf", lat, lng);
        free(cached);
        buf_free(&key);
        return 0;
    }
    char *miss = cache_get(c->miss_cache, key.s);
    if (miss) {
        free(miss);
        buf_free(&key);
        set_error(err, OLA_NOT_FOUND, "Place not found");
        return -1;
    }
    const char *params[] = {"place_id", place_id, NULL};
    JVal *data = request(c, "GET", "/places/v1/details", params, err);
    if (!data) {
        if (err->status == OLA_REJECTED) set_error(err, OLA_NOT_FOUND, "Place not found");
        if (err->status == OLA_NOT_FOUND) cache_set(c->miss_cache, key.s, "1");
        buf_free(&key);
        return -1;
    }
    JVal *result = jget(data, "result");
    int ok = lat_lng(result && result->type == J_OBJ ? result : data, lat, lng);
    json_free(data);
    if (!ok) {
        cache_set(c->miss_cache, key.s, "1");
        buf_free(&key);
        set_error(err, OLA_NOT_FOUND, "Place not found");
        return -1;
    }
    char value[80];
    snprintf(value, sizeof value, "%.17g,%.17g", *lat, *lng);
    cache_set(c->place_cache, key.s, value);
    buf_free(&key);
    return 0;
}

int ola_reverse(OlaClient *c, double lat, double lng, char **name, char **address, OlaError *err) {
    char a[64], b[64], key[160];
    num_repr(py_round(lat, 4), a, sizeof a);
    num_repr(py_round(lng, 4), b, sizeof b);
    snprintf(key, sizeof key, "rg|%s|%s", a, b);
    char *cached = cache_get(c->place_cache, key);
    if (cached) {
        char *sep = strchr(cached, '\x1f');
        *name = xstrndup(cached, (size_t)(sep - cached));
        *address = xstrdup(sep + 1);
        free(cached);
        return 0;
    }
    char latlng[64];
    coord(lat, lng, latlng, sizeof latlng);
    const char *params[] = {"latlng", latlng, NULL};
    JVal *data = request(c, "GET", "/places/v1/reverse-geocode", params, err);
    if (!data) return -1;
    JVal *results = jget(data, "results");
    JVal *first = (results && results->type == J_ARR && results->n && results->items[0]->type == J_OBJ)
                      ? results->items[0]
                      : NULL;
    char *addr = truthy(jget(first, "formatted_address")) ? text_of(jget(first, "formatted_address")) : xstrdup("");
    addr = strip(addr);
    char *nm = truthy(jget(first, "name")) ? text_of(jget(first, "name")) : xstrndup(addr, strcspn(addr, ","));
    nm = strip(nm);
    if (!*nm) {
        free(nm);
        nm = xstrdup("Pinned place");
    }
    utf8_truncate(nm, 120);
    utf8_truncate(addr, 200);
    json_free(data);
    Buf value;
    buf_init(&value);
    buf_printf(&value, "%s\x1f%s", nm, addr);
    cache_set(c->place_cache, key, value.s);
    buf_free(&value);
    *name = nm;
    *address = addr;
    return 0;
}

/* ----------------------------------------------------------------- routing */

/* Fastest road route with live traffic. */
int ola_directions(OlaClient *c, double olat, double olng, double dlat, double dlng, Road *out, OlaError *err) {
    char o[64], d[64];
    coord(olat, olng, o, sizeof o);
    coord(dlat, dlng, d, sizeof d);
    Buf key;
    buf_init(&key);
    buf_printf(&key, "dir|%s|%s", o, d);
    char *cached = cache_get(c->route_cache, key.s);
    if (cached) {
        char *p1 = strchr(cached, '\x1f'), *p2 = strchr(p1 + 1, '\x1f');
        out->km = strtod(cached, NULL);
        out->min = strtod(p1 + 1, NULL);
        out->polyline = p2[1] == '1' ? xstrdup(p2 + 2) : NULL;
        free(cached);
        buf_free(&key);
        return 0;
    }
    const char *params[] = {"origin", o, "destination", d, NULL};
    JVal *data = request(c, "POST", "/routing/v1/directions", params, err);
    if (!data) {
        if (err->status == OLA_REJECTED) set_error(err, OLA_NOT_FOUND, "No road route found between these places");
        buf_free(&key);
        return -1;
    }
    JVal *routes = jget(data, "routes");
    JVal *route = (routes && routes->type == J_ARR && routes->n && routes->items[0]->type == J_OBJ) ? routes->items[0] : NULL;
    if (!route) {
        json_free(data);
        buf_free(&key);
        set_error(err, OLA_NOT_FOUND, "No road route found between these places");
        return -1;
    }
    JVal *legs = jget(route, "legs");
    double metres = 0, seconds = 0;
    int nlegs = 0, bad = 0;
    for (int i = 0; legs && legs->type == J_ARR && i < legs->n; i++) {
        JVal *leg = legs->items[i];
        if (leg->type != J_OBJ) continue;
        double m = number_of(jget(leg, "distance")), s = number_of(jget(leg, "duration"));
        if (isnan(m) || isnan(s)) bad = 1;
        else {
            metres += m;
            seconds += s;
        }
        nlegs++;
    }
    if (!nlegs || bad || metres <= 0) {
        json_free(data);
        buf_free(&key);
        set_error(err, OLA_ERROR, "The map service returned a route without a usable distance");
        return -1;
    }
    JVal *poly = jget(route, "overview_polyline");
    if (poly && poly->type == J_OBJ) poly = jget(poly, "points");
    out->km = py_round(metres / 1000, 2);
    out->min = py_round(seconds / 60, 1);
    out->polyline = (poly && poly->type == J_STR && strlen(poly->str) < 200000) ? xstrdup(poly->str) : NULL;
    json_free(data);

    Buf value;
    buf_init(&value);
    char km[32], min[32];
    num_repr(out->km, km, sizeof km);
    num_repr(out->min, min, sizeof min);
    buf_printf(&value, "%s\x1f%s\x1f%s%s", km, min, out->polyline ? "1" : "0", out->polyline ? out->polyline : "");
    cache_set(c->route_cache, key.s, value.s);
    buf_free(&value);
    buf_free(&key);
    return 0;
}

/* Road distance and time for every origin-destination pair (out is no x nd, row by row). */
int ola_distance_matrix(OlaClient *c, const double (*origins)[2], int no, const double (*dests)[2], int nd,
                        RoadCell *out, OlaError *err) {
    Buf os, ds, key;
    buf_init(&os);
    buf_init(&ds);
    buf_init(&key);
    char p[64];
    for (int i = 0; i < no; i++) {
        coord(origins[i][0], origins[i][1], p, sizeof p);
        buf_printf(&os, "%s%s", i ? "|" : "", p);
    }
    for (int i = 0; i < nd; i++) {
        coord(dests[i][0], dests[i][1], p, sizeof p);
        buf_printf(&ds, "%s%s", i ? "|" : "", p);
    }
    buf_printf(&key, "dm|%s|%s", os.s, ds.s);
    int result = -1;
    char *cached = cache_get(c->route_cache, key.s);
    if (cached) {
        char *s = cached;
        for (int i = 0; i < no * nd; i++) {
            if (*s == '-') {
                out[i].ok = 0;
                s++;
            } else {
                out[i].ok = 1;
                out[i].km = strtod(s, &s);
                out[i].min = strtod(s + 1, &s);
            }
            if (*s) s++;
        }
        free(cached);
        result = 0;
        goto done;
    }
    const char *params[] = {"origins", os.s, "destinations", ds.s, NULL};
    JVal *data = request(c, "GET", "/routing/v1/distanceMatrix", params, err);
    if (!data) goto done;
    JVal *rows = jget(data, "rows");
    if (!rows || rows->type != J_ARR) {
        set_error(err, OLA_ERROR, "The map service returned an unexpected distance matrix");
        json_free(data);
        goto done;
    }
    if (rows->n != no) {
        set_error(err, OLA_ERROR, "The map service returned a distance matrix of the wrong size");
        json_free(data);
        goto done;
    }
    for (int r = 0; r < rows->n; r++) {
        JVal *elements = jget(rows->items[r], "elements");
        if (!elements || elements->type != J_ARR) {
            set_error(err, OLA_ERROR, "The map service returned an unexpected distance matrix");
            json_free(data);
            goto done;
        }
        if (elements->n != nd) {
            set_error(err, OLA_ERROR, "The map service returned a distance matrix of the wrong size");
            json_free(data);
            goto done;
        }
        for (int e = 0; e < elements->n; e++) {
            JVal *el = elements->items[e]->type == J_OBJ ? elements->items[e] : NULL;
            double m = number_of(jget(el, "distance")), s = number_of(jget(el, "duration"));
            JVal *st = jget(el, "status");
            int ok = !isnan(m) && !isnan(s);
            if (ok && st) {
                char *status = text_of(st);
                ok = str_ieq(status, "OK");
                free(status);
            }
            RoadCell *cell = &out[r * nd + e];
            cell->ok = ok;
            cell->km = ok ? py_round(m / 1000, 2) : 0;
            cell->min = ok ? py_round(s / 60, 1) : 0;
        }
    }
    json_free(data);
    Buf value;
    buf_init(&value);
    for (int i = 0; i < no * nd; i++) {
        if (i) buf_putc(&value, i % nd ? ';' : '|');
        if (!out[i].ok) buf_putc(&value, '-');
        else {
            char km[32], min[32];
            num_repr(out[i].km, km, sizeof km);
            num_repr(out[i].min, min, sizeof min);
            buf_printf(&value, "%s,%s", km, min);
        }
    }
    cache_set(c->route_cache, key.s, value.s);
    buf_free(&value);
    result = 0;
done:
    buf_free(&os);
    buf_free(&ds);
    buf_free(&key);
    return result;
}
