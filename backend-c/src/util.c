/*
 * util.c - see util.h.
 */
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ memory */

static void out_of_memory(void) {
    fprintf(stderr, "fatal: out of memory\n");
    abort();
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) out_of_memory();
    return p;
}

void *xcalloc(size_t count, size_t size) {
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) out_of_memory();
    return q;
}

char *xstrdup(const char *s) {
    return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* --------------------------------------------------------------------- Buf */

void buf_init(Buf *b) {
    b->cap = 64;
    b->len = 0;
    b->s = xmalloc(b->cap);
    b->s[0] = '\0';
}

void buf_free(Buf *b) {
    free(b->s);
    b->s = NULL;
    b->len = b->cap = 0;
}

static void buf_grow(Buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    while (b->len + extra + 1 > b->cap) b->cap *= 2;      /* doubling: amortised O(1) appends */
    b->s = xrealloc(b->s, b->cap);
}

void buf_putn(Buf *b, const char *s, size_t n) {
    buf_grow(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

void buf_puts(Buf *b, const char *s) { buf_putn(b, s, strlen(s)); }

void buf_putc(Buf *b, char c) { buf_putn(b, &c, 1); }

void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char small[256];
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof small) {
        buf_putn(b, small, (size_t)n);
        return;
    }
    buf_grow(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

char *buf_steal(Buf *b) {
    char *s = b->s;
    b->s = NULL;
    b->len = b->cap = 0;
    return s;
}

/* ----------------------------------------------------------------- numbers */

/* printf rounds the exact binary value, ties to even, exactly like Python's round(). */
double py_round(double x, int ndigits) {
    if (!isfinite(x)) return x;
    char text[512];
    snprintf(text, sizeof text, "%.*f", ndigits, x);
    return strtod(text, NULL);
}

long py_round_int(double x) {
    if (!isfinite(x)) return 0;
    char text[512];
    snprintf(text, sizeof text, "%.0f", x);
    return strtol(text, NULL, 10);
}

void num_repr(double x, char *out, size_t n) {
    if (!isfinite(x)) {
        snprintf(out, n, "null");
        return;
    }
    if (x == 0) x = 0;                     /* print -0 as 0 */
    for (int precision = 1; precision <= 17; precision++) {
        snprintf(out, n, "%.*g", precision, x);
        if (strtod(out, NULL) == x) return;
    }
}

int parse_double(const char *s, double *out) {
    if (!s) return 0;
    while (isspace((unsigned char)*s)) s++;
    if (!*s) return 0;
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    while (isspace((unsigned char)*end)) end++;
    if (*end || end == s) return 0;
    *out = v;
    return 1;
}

int parse_long(const char *s, long *out) {
    if (!s) return 0;
    while (isspace((unsigned char)*s)) s++;
    if (!*s) return 0;
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    while (isspace((unsigned char)*end)) end++;
    if (*end || end == s || errno) return 0;
    *out = v;
    return 1;
}

/* ------------------------------------------------------- geography and time */

double haversine_km(double lat1, double lng1, double lat2, double lng2) {
    const double r = M_PI / 180.0;
    double p1 = lat1 * r, p2 = lat2 * r, dp = (lat2 - lat1) * r, dl = (lng2 - lng1) * r;
    double h = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    return 2 * 6371.0 * asin(sqrt(h));
}

double now_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

long days_from_civil(int y, int m, int d) {
    /* Howard Hinnant's algorithm: proleptic Gregorian calendar, no time zones. */
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

void ist_clock(time_t t, Clock *c) {
    struct tm tm;
    time_t local = t + IST_OFFSET;
    gmtime_r(&local, &tm);
    c->year = tm.tm_year + 1900;
    c->month = tm.tm_mon + 1;
    c->day = tm.tm_mday;
    c->hour = tm.tm_hour;
    c->minute = tm.tm_min;
    c->second = tm.tm_sec;
    c->weekday = (tm.tm_wday + 6) % 7;
}

time_t ist_time(int y, int mo, int d, int h, int mi, int s) {
    long days = days_from_civil(y, mo, d);
    return (time_t)(days * 86400L + h * 3600L + mi * 60L + s) - IST_OFFSET;
}

/* -------------------------------------------------------------------- text */

int utf8_chars(const char *s) {
    int n = 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

void utf8_truncate(char *s, int max_chars) {
    int n = 0;
    for (char *p = s; *p; p++) {
        if (((unsigned char)*p & 0xC0) != 0x80) {
            if (n == max_chars) {
                *p = '\0';
                return;
            }
            n++;
        }
    }
}

char *collapse_spaces(const char *s) {
    Buf b;
    buf_init(&b);
    int pending = 0;
    for (; *s; s++) {
        if (isspace((unsigned char)*s)) {
            pending = b.len > 0;
            continue;
        }
        if (pending) buf_putc(&b, ' ');
        pending = 0;
        buf_putc(&b, *s);
    }
    return buf_steal(&b);
}

void str_lower(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

int str_ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}
