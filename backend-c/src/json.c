/*
 * json.c - see json.h.
 */
#include "json.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== writer */

void jw_init(JW *w, Buf *b) {
    w->b = b;
    w->depth = 0;
    w->first[0] = 1;
    w->after_key = 0;
}

static void before_value(JW *w) {
    if (w->after_key) {                     /* "key": value - no comma before the value */
        w->after_key = 0;
        return;
    }
    if (w->depth > 0) {
        if (!w->first[w->depth]) buf_putc(w->b, ',');
        w->first[w->depth] = 0;
    }
}

static void jw_open(JW *w, char c) {
    before_value(w);
    buf_putc(w->b, c);
    if (w->depth < (int)sizeof w->first - 1) w->depth++;
    w->first[w->depth] = 1;
}

void jw_obj(JW *w) { jw_open(w, '{'); }
void jw_arr(JW *w) { jw_open(w, '['); }

void jw_obj_end(JW *w) {
    buf_putc(w->b, '}');
    w->depth--;
}

void jw_arr_end(JW *w) {
    buf_putc(w->b, ']');
    w->depth--;
}

/* Length of a valid UTF-8 sequence starting at p, or 0 if it is not valid. */
static int utf8_seq(const unsigned char *p) {
    if (p[0] < 0x80) return 1;
    if (p[0] >= 0xC2 && p[0] <= 0xDF) return (p[1] & 0xC0) == 0x80 ? 2 : 0;
    if (p[0] >= 0xE0 && p[0] <= 0xEF) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
        if (p[0] == 0xE0 && p[1] < 0xA0) return 0;          /* overlong */
        if (p[0] == 0xED && p[1] >= 0xA0) return 0;         /* surrogate */
        return 3;
    }
    if (p[0] >= 0xF0 && p[0] <= 0xF4) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return 0;
        if (p[0] == 0xF0 && p[1] < 0x90) return 0;
        if (p[0] == 0xF4 && p[1] >= 0x90) return 0;
        return 4;
    }
    return 0;
}

void json_escape(Buf *b, const char *s) {
    buf_putc(b, '"');
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned char c = *p;
        if (c == '"') buf_puts(b, "\\\"");
        else if (c == '\\') buf_puts(b, "\\\\");
        else if (c == '\n') buf_puts(b, "\\n");
        else if (c == '\r') buf_puts(b, "\\r");
        else if (c == '\t') buf_puts(b, "\\t");
        else if (c < 0x20 || c == 0x7F) buf_printf(b, "\\u%04x", c);
        else if (c < 0x80) buf_putc(b, (char)c);
        else {
            int n = utf8_seq(p);
            if (n == 0) {
                buf_puts(b, "\xEF\xBF\xBD");              /* U+FFFD replacement character */
                p++;
                continue;
            }
            buf_putn(b, (const char *)p, (size_t)n);
            p += n;
            continue;
        }
        p++;
    }
    buf_putc(b, '"');
}

void jw_key(JW *w, const char *key) {
    before_value(w);
    json_escape(w->b, key);
    buf_putc(w->b, ':');
    w->after_key = 1;
}

void jw_str(JW *w, const char *s) {
    before_value(w);
    if (!s) buf_puts(w->b, "null");
    else json_escape(w->b, s);
}

void jw_num(JW *w, double x) {
    before_value(w);
    char text[64];
    num_repr(x, text, sizeof text);
    buf_puts(w->b, text);
}

void jw_int(JW *w, long x) {
    before_value(w);
    buf_printf(w->b, "%ld", x);
}

void jw_bool(JW *w, int v) {
    before_value(w);
    buf_puts(w->b, v ? "true" : "false");
}

void jw_null(JW *w) {
    before_value(w);
    buf_puts(w->b, "null");
}

void jw_raw(JW *w, const char *json) {
    before_value(w);
    buf_puts(w->b, json);
}

/* ================================================================== parser */

typedef struct {
    const char *p, *end;
    int depth;
} Parser;

static void skip_ws(Parser *ps) {
    while (ps->p < ps->end && isspace((unsigned char)*ps->p)) ps->p++;
}

static JVal *new_val(JType t) {
    JVal *v = xcalloc(1, sizeof *v);
    v->type = t;
    return v;
}

static void put_utf8(Buf *b, unsigned cp) {
    if (cp < 0x80) buf_putc(b, (char)cp);
    else if (cp < 0x800) {
        buf_putc(b, (char)(0xC0 | (cp >> 6)));
        buf_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        buf_putc(b, (char)(0xE0 | (cp >> 12)));
        buf_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        buf_putc(b, (char)(0xF0 | (cp >> 18)));
        buf_putc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        buf_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_putc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

static int hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static char *parse_string(Parser *ps) {
    if (ps->p >= ps->end || *ps->p != '"') return NULL;
    ps->p++;
    Buf b;
    buf_init(&b);
    while (ps->p < ps->end) {
        char c = *ps->p++;
        if (c == '"') return buf_steal(&b);
        if ((unsigned char)c < 0x20) break;
        if (c != '\\') {
            buf_putc(&b, c);
            continue;
        }
        if (ps->p >= ps->end) break;
        char e = *ps->p++;
        switch (e) {
        case '"': buf_putc(&b, '"'); break;
        case '\\': buf_putc(&b, '\\'); break;
        case '/': buf_putc(&b, '/'); break;
        case 'b': buf_putc(&b, '\b'); break;
        case 'f': buf_putc(&b, '\f'); break;
        case 'n': buf_putc(&b, '\n'); break;
        case 'r': buf_putc(&b, '\r'); break;
        case 't': buf_putc(&b, '\t'); break;
        case 'u': {
            unsigned cp;
            if (ps->end - ps->p < 4 || !hex4(ps->p, &cp)) goto fail;
            ps->p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                unsigned lo;
                if (hex4(ps->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ps->p += 6;
                }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;   /* lone surrogate */
            put_utf8(&b, cp);
            break;
        }
        default: goto fail;
        }
    }
fail:
    buf_free(&b);
    return NULL;
}

static JVal *parse_value(Parser *ps);

static void add_item(JVal *v, char *key, JVal *item) {
    v->items = xrealloc(v->items, (size_t)(v->n + 1) * sizeof *v->items);
    if (v->type == J_OBJ) v->keys = xrealloc(v->keys, (size_t)(v->n + 1) * sizeof *v->keys);
    v->items[v->n] = item;
    if (v->type == J_OBJ) v->keys[v->n] = key;
    v->n++;
}

static JVal *parse_container(Parser *ps, int is_obj) {
    if (++ps->depth > 64) return NULL;
    ps->p++;                                           /* skip { or [ */
    JVal *v = new_val(is_obj ? J_OBJ : J_ARR);
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == (is_obj ? '}' : ']')) {
        ps->p++;
        ps->depth--;
        return v;
    }
    for (;;) {
        char *key = NULL;
        skip_ws(ps);
        if (is_obj) {
            key = parse_string(ps);
            if (!key) goto fail;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                free(key);
                goto fail;
            }
            ps->p++;
        }
        JVal *item = parse_value(ps);
        if (!item) {
            free(key);
            goto fail;
        }
        add_item(v, key, item);
        skip_ws(ps);
        if (ps->p >= ps->end) goto fail;
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == (is_obj ? '}' : ']')) {
            ps->p++;
            ps->depth--;
            return v;
        }
        goto fail;
    }
fail:
    json_free(v);
    return NULL;
}

static JVal *parse_value(Parser *ps) {
    skip_ws(ps);
    if (ps->p >= ps->end) return NULL;
    char c = *ps->p;
    if (c == '{') return parse_container(ps, 1);
    if (c == '[') return parse_container(ps, 0);
    if (c == '"') {
        char *s = parse_string(ps);
        if (!s) return NULL;
        JVal *v = new_val(J_STR);
        v->str = s;
        return v;
    }
    if (ps->end - ps->p >= 4 && memcmp(ps->p, "true", 4) == 0) {
        ps->p += 4;
        JVal *v = new_val(J_BOOL);
        v->boolean = 1;
        return v;
    }
    if (ps->end - ps->p >= 5 && memcmp(ps->p, "false", 5) == 0) {
        ps->p += 5;
        return new_val(J_BOOL);
    }
    if (ps->end - ps->p >= 4 && memcmp(ps->p, "null", 4) == 0) {
        ps->p += 4;
        return new_val(J_NULL);
    }
    if (c == '-' || isdigit((unsigned char)c)) {
        char number[64];
        size_t n = 0;
        while (ps->p < ps->end && n < sizeof number - 1 && strchr("+-0123456789.eE", *ps->p))
            number[n++] = *ps->p++;
        number[n] = '\0';
        char *end;
        double x = strtod(number, &end);
        if (*end) return NULL;
        JVal *v = new_val(J_NUM);
        v->num = x;
        return v;
    }
    return NULL;
}

JVal *json_parse(const char *text, size_t len) {
    Parser ps = {text, text + len, 0};
    JVal *v = parse_value(&ps);
    if (!v) return NULL;
    skip_ws(&ps);
    if (ps.p != ps.end) {
        json_free(v);
        return NULL;
    }
    return v;
}

void json_free(JVal *v) {
    if (!v) return;
    for (int i = 0; i < v->n; i++) {
        json_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->str);
    free(v);
}

JVal *jget(const JVal *obj, const char *key) {
    if (!obj || obj->type != J_OBJ) return NULL;
    for (int i = obj->n - 1; i >= 0; i--)           /* the last duplicate wins, like Python */
        if (strcmp(obj->keys[i], key) == 0) return obj->items[i];
    return NULL;
}

JVal *jat(const JVal *arr, int i) {
    if (!arr || arr->type != J_ARR || i < 0 || i >= arr->n) return NULL;
    return arr->items[i];
}
