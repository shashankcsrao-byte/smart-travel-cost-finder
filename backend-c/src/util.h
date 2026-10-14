/*
 * util.h - small helpers used everywhere: safe memory allocation, a growable
 * string buffer, Python-compatible rounding, time in IST and UTF-8 helpers.
 */
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <time.h>

/* ---- memory: these never return NULL (the program stops if memory runs out) ---- */
void *xmalloc(size_t n);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* ---- Buf: a growable, always NUL-terminated string ---- */
typedef struct {
    char *s;
    size_t len, cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_putn(Buf *b, const char *s, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_putc(Buf *b, char c);
void buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
char *buf_steal(Buf *b);                 /* hand over the string, leave the buffer empty */

/* ---- numbers ---- */
double py_round(double x, int ndigits); /* same result as Python's round(x, n) */
long py_round_int(double x);            /* same result as Python's round(x) */
void num_repr(double x, char *out, size_t n);   /* shortest text that reads back as x */
int parse_double(const char *s, double *out);   /* whole string must be a number */
int parse_long(const char *s, long *out);

/* ---- geography and time ---- */
double haversine_km(double lat1, double lng1, double lat2, double lng2);
double now_monotonic(void);

#define IST_OFFSET 19800                 /* India Standard Time = UTC + 5:30, no DST */
typedef struct {
    int year, month, day, hour, minute, second;
    int weekday;                         /* 0 = Monday ... 6 = Sunday (like Python) */
} Clock;

void ist_clock(time_t t, Clock *c);     /* split a UTC timestamp into IST fields */
time_t ist_time(int y, int mo, int d, int h, int mi, int s);   /* IST fields -> timestamp */
long days_from_civil(int y, int m, int d);   /* days since 1970-01-01 */

/* ---- text ---- */
int utf8_chars(const char *s);           /* number of characters, not bytes */
void utf8_truncate(char *s, int max_chars);
char *collapse_spaces(const char *s);    /* Python: " ".join(s.split()) */
void str_lower(char *s);
int str_ieq(const char *a, const char *b);

#endif
