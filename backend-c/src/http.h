/*
 * http.h - a small HTTP/1.1 server on POSIX sockets.
 *
 * One thread per connection, keep-alive, request-line and header parsing,
 * URL decoding of the path and the query string, gzip compression of text
 * answers (zlib), and a handler callback that fills in the response.
 */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#include "util.h"

#define HTTP_MAX_HEADERS 64
#define HTTP_MAX_PARAMS 64

typedef struct {
    char *name, *value;
} Header;

typedef struct {
    char method[16];
    char *path;                  /* URL-decoded, without the query string */
    char *raw_path;              /* as sent */
    Header headers[HTTP_MAX_HEADERS];
    int nheaders;
    char *keys[HTTP_MAX_PARAMS], *values[HTTP_MAX_PARAMS];
    int nparams;
    char client[64];             /* the visitor's IP address */
} Request;

typedef struct {
    int status;
    Buf body;
    Header headers[32];
    int nheaders;
} Response;

typedef void (*HttpHandler)(const Request *req, Response *res, void *ctx);

/* Blocks forever serving requests. trust_proxy: take the client IP from X-Forwarded-For. */
int http_serve(const char *host, int port, HttpHandler handler, void *ctx, int trust_proxy);

const char *req_header(const Request *req, const char *name);    /* case-insensitive; NULL if absent */
const char *req_param(const Request *req, const char *name);     /* the last value given; NULL if absent */

void res_header(Response *res, const char *name, const char *value);       /* replaces */
void res_header_default(Response *res, const char *name, const char *value); /* only if not set yet */
const char *res_get_header(const Response *res, const char *name);
void res_json(Response *res, int status, const char *json);
void res_detail(Response *res, int status, const char *message);          /* {"detail": message} */

const char *http_reason(int status);

#endif
