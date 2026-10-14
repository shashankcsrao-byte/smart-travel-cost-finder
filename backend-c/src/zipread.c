/*
 * zipread.c - see zipread.h.
 */
#include "zipread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "util.h"

static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static unsigned long rd32(const unsigned char *p) {
    return (unsigned long)p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24;
}

int zip_open(ZipArchive *z, const char *path) {
    z->data = NULL;
    z->size = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long n = ftell(f);
    rewind(f);
    if (n <= 0) {
        fclose(f);
        return -1;
    }
    z->data = xmalloc((size_t)n);
    z->size = fread(z->data, 1, (size_t)n, f);
    fclose(f);
    return z->size == (size_t)n ? 0 : -1;
}

void zip_close(ZipArchive *z) {
    free(z->data);
    z->data = NULL;
    z->size = 0;
}

static char *inflate_raw(const unsigned char *src, size_t src_len, size_t out_len) {
    char *out = xmalloc(out_len + 1);
    z_stream s;
    memset(&s, 0, sizeof s);
    if (inflateInit2(&s, -MAX_WBITS) != Z_OK) {        /* negative: raw deflate, no zlib header */
        free(out);
        return NULL;
    }
    s.next_in = (unsigned char *)src;
    s.avail_in = (uInt)src_len;
    s.next_out = (unsigned char *)out;
    s.avail_out = (uInt)out_len;
    int rc = inflate(&s, Z_FINISH);
    inflateEnd(&s);
    if (rc != Z_STREAM_END || s.total_out != out_len) {
        free(out);
        return NULL;
    }
    out[out_len] = '\0';
    return out;
}

char *zip_read(const ZipArchive *z, const char *name, size_t *len) {
    const unsigned char *d = z->data;
    size_t n = z->size;
    if (n < 22) return NULL;

    /* 1. Find the "end of central directory" record (it may be followed by a comment). */
    size_t eocd = 0;
    int found = 0;
    for (size_t i = n - 22 + 1; i-- > 0 && n - i <= 65557;) {
        if (rd32(d + i) == 0x06054b50UL) {
            eocd = i;
            found = 1;
            break;
        }
    }
    if (!found) return NULL;
    unsigned entries = rd16(d + eocd + 10);
    size_t pos = rd32(d + eocd + 16);
    size_t name_len = strlen(name);

    /* 2. Walk the central directory looking for the member. */
    for (unsigned e = 0; e < entries; e++) {
        if (pos + 46 > n || rd32(d + pos) != 0x02014b50UL) return NULL;
        unsigned method = rd16(d + pos + 10);
        size_t comp = rd32(d + pos + 20), size = rd32(d + pos + 24);
        unsigned fn_len = rd16(d + pos + 28), extra = rd16(d + pos + 30), comment = rd16(d + pos + 32);
        size_t local = rd32(d + pos + 42);
        if (pos + 46 + fn_len > n) return NULL;
        if (fn_len == name_len && memcmp(d + pos + 46, name, name_len) == 0) {
            /* 3. The local header says where the data starts. */
            if (local + 30 > n || rd32(d + local) != 0x04034b50UL) return NULL;
            size_t start = local + 30 + rd16(d + local + 26) + rd16(d + local + 28);
            if (start + comp > n) return NULL;
            char *out;
            if (method == 0) {
                out = xmalloc(size + 1);
                memcpy(out, d + start, size);
                out[size] = '\0';
            } else if (method == 8) {
                out = inflate_raw(d + start, comp, size);
            } else {
                return NULL;
            }
            if (out && len) *len = size;
            return out;
        }
        pos += 46 + fn_len + extra + comment;
    }
    return NULL;
}
