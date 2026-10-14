/*
 * zipread.h - read one file out of a .zip archive held in memory.
 *
 * The Namma Metro GTFS feed is a zip of CSV files. This reader walks the zip's
 * central directory and unpacks a member with zlib (deflate) or copies it
 * (stored). That is all the feed needs.
 */
#ifndef ZIPREAD_H
#define ZIPREAD_H

#include <stddef.h>

typedef struct {
    unsigned char *data;
    size_t size;
} ZipArchive;

int zip_open(ZipArchive *z, const char *path);      /* 0 on success */
void zip_close(ZipArchive *z);
char *zip_read(const ZipArchive *z, const char *name, size_t *len);   /* NULL if missing; caller frees */

#endif
