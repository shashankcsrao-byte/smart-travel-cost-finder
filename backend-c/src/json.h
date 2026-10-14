/*
 * json.h - writing JSON responses and reading Ola Maps answers.
 *
 * Writer: call jw_obj / jw_key / jw_str ... in order; commas are inserted for you.
 * Parser: a small recursive-descent parser that builds a tree of JVal nodes.
 */
#ifndef JSON_H
#define JSON_H

#include "util.h"

/* ------------------------------------------------------------------ writer */
typedef struct {
    Buf *b;
    int depth;
    unsigned char first[96];     /* is the next item the first in this container? */
    int after_key;
} JW;

void jw_init(JW *w, Buf *b);
void jw_obj(JW *w);
void jw_obj_end(JW *w);
void jw_arr(JW *w);
void jw_arr_end(JW *w);
void jw_key(JW *w, const char *key);
void jw_str(JW *w, const char *s);          /* NULL writes null */
void jw_num(JW *w, double x);               /* NaN / infinity write null */
void jw_int(JW *w, long x);
void jw_bool(JW *w, int v);
void jw_null(JW *w);
void jw_raw(JW *w, const char *json);       /* a value that is already JSON text */

void json_escape(Buf *b, const char *s);    /* "..." with escapes, invalid UTF-8 replaced */

/* ------------------------------------------------------------------ parser */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal {
    JType type;
    int boolean;
    double num;
    char *str;
    int n;                       /* items in an array or object */
    struct JVal **items;
    char **keys;                 /* object keys (NULL for arrays) */
} JVal;

JVal *json_parse(const char *text, size_t len);    /* NULL if the text is not valid JSON */
void json_free(JVal *v);
JVal *jget(const JVal *obj, const char *key);       /* NULL if missing or not an object */
JVal *jat(const JVal *arr, int i);                  /* NULL if out of range or not an array */

#endif
