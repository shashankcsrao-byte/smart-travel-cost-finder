/*
 * csv.c - see csv.h.
 */
#include "csv.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

/* Split one record starting at *pos. Fields are unquoted in place. Returns the field count. */
static int parse_record(char *data, size_t len, size_t *pos, char ***fields_out, int *cap) {
    char **fields = *fields_out;
    int n = 0;
    size_t r = *pos;
    for (;;) {
        if (n == *cap) {
            *cap = *cap ? *cap * 2 : 16;
            fields = xrealloc(fields, (size_t)*cap * sizeof *fields);
        }
        char *start = data + r;
        char *w = start;
        if (r < len && data[r] == '"') {                /* quoted field */
            r++;
            while (r < len) {
                if (data[r] == '"') {
                    if (r + 1 < len && data[r + 1] == '"') {
                        *w++ = '"';
                        r += 2;
                        continue;
                    }
                    r++;
                    break;
                }
                *w++ = data[r++];
            }
            while (r < len && data[r] != ',' && data[r] != '\n' && data[r] != '\r') r++;
        } else {
            while (r < len && data[r] != ',' && data[r] != '\n' && data[r] != '\r') *w++ = data[r++];
        }
        char end = r < len ? data[r] : '\n';
        *w = '\0';
        fields[n++] = start;
        if (end == ',') {
            r++;
            continue;
        }
        /* End of the record: step over \n, \r or \r\n (decided from the saved character,
           because the terminator has just been overwritten with '\0'). */
        if (r < len) {
            r++;
            if (end == '\r' && r < len && data[r] == '\n') r++;
        }
        break;
    }
    *pos = r;
    *fields_out = fields;
    return n;
}

Csv *csv_parse(char *data, size_t len) {
    Csv *c = xcalloc(1, sizeof *c);
    c->data = data;
    size_t pos = 0;
    if (len >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        pos = 3;                                         /* skip a UTF-8 byte-order mark */
    int cap = 0;
    char **fields = NULL;
    c->ncols = parse_record(data, len, &pos, &fields, &cap);
    c->header = xmalloc((size_t)c->ncols * sizeof *c->header);
    memcpy(c->header, fields, (size_t)c->ncols * sizeof *c->header);

    int rows_cap = 1024;
    c->rows = xmalloc((size_t)rows_cap * sizeof *c->rows);
    c->row_len = xmalloc((size_t)rows_cap * sizeof *c->row_len);
    while (pos < len) {
        int n = parse_record(data, len, &pos, &fields, &cap);
        if (n == 1 && fields[0][0] == '\0') continue;    /* blank line */
        if (c->nrows == rows_cap) {
            rows_cap *= 2;
            c->rows = xrealloc(c->rows, (size_t)rows_cap * sizeof *c->rows);
            c->row_len = xrealloc(c->row_len, (size_t)rows_cap * sizeof *c->row_len);
        }
        c->rows[c->nrows] = xmalloc((size_t)n * sizeof(char *));
        memcpy(c->rows[c->nrows], fields, (size_t)n * sizeof(char *));
        c->row_len[c->nrows] = n;
        c->nrows++;
    }
    free(fields);
    return c;
}

void csv_free(Csv *c) {
    if (!c) return;
    for (int i = 0; i < c->nrows; i++) free(c->rows[i]);
    free(c->rows);
    free(c->row_len);
    free(c->header);
    free(c->data);
    free(c);
}

int csv_col(const Csv *c, const char *name) {
    for (int i = 0; i < c->ncols; i++)
        if (strcmp(c->header[i], name) == 0) return i;
    return -1;
}

const char *csv_get(const Csv *c, int row, int col) {
    if (col < 0 || row < 0 || row >= c->nrows || col >= c->row_len[row]) return "";
    return c->rows[row][col];
}
