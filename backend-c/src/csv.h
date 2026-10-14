/*
 * csv.h - a CSV reader (RFC 4180: quoted fields, "" inside quotes, CRLF).
 *
 * The text is split in place: fields point into the buffer the table owns.
 */
#ifndef CSV_H
#define CSV_H

#include <stddef.h>

typedef struct {
    char *data;
    int ncols;
    char **header;
    int nrows;
    char ***rows;
    int *row_len;
} Csv;

Csv *csv_parse(char *data, size_t len);          /* takes ownership of data */
void csv_free(Csv *c);
int csv_col(const Csv *c, const char *name);     /* column number, -1 if missing */
const char *csv_get(const Csv *c, int row, int col);   /* "" when missing */

#endif
