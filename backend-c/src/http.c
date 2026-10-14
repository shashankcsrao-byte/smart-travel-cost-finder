/*
 * http.c - see http.h.
 */
#include "http.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <zlib.h>

#include "json.h"

#define MAX_HEAD 16384             /* request line + headers */
#define MAX_BODY (1u << 20)        /* we never need request bodies; read and drop up to 1 MB */
#define MAX_CONNECTIONS 512
#define KEEPALIVE_SECONDS 15
#define MAX_REQUESTS_PER_CONNECTION 200

static atomic_int open_connections;

/* ----------------------------------------------------------------- helpers */

const char *http_reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default: return "OK";
    }
}

const char *req_header(const Request *req, const char *name) {
    for (int i = 0; i < req->nheaders; i++)
        if (strcasecmp(req->headers[i].name, name) == 0) return req->headers[i].value;
    return NULL;
}

const char *req_param(const Request *req, const char *name) {
    for (int i = req->nparams - 1; i >= 0; i--)
        if (strcmp(req->keys[i], name) == 0) return req->values[i];
    return NULL;
}

const char *res_get_header(const Response *res, const char *name) {
    for (int i = 0; i < res->nheaders; i++)
        if (strcasecmp(res->headers[i].name, name) == 0) return res->headers[i].value;
    return NULL;
}

void res_header(Response *res, const char *name, const char *value) {
    for (int i = 0; i < res->nheaders; i++) {
        if (strcasecmp(res->headers[i].name, name) == 0) {
            free(res->headers[i].value);
            res->headers[i].value = xstrdup(value);
            return;
        }
    }
    if (res->nheaders == 32) return;
    res->headers[res->nheaders].name = xstrdup(name);
    res->headers[res->nheaders].value = xstrdup(value);
    res->nheaders++;
}

void res_header_default(Response *res, const char *name, const char *value) {
    if (!res_get_header(res, name)) res_header(res, name, value);
}

void res_json(Response *res, int status, const char *json) {
    res->status = status;
    res->body.len = 0;
    buf_puts(&res->body, json);
    res_header(res, "Content-Type", "application/json");
}

void res_detail(Response *res, int status, const char *message) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"detail\":");
    json_escape(&b, message);
    buf_putc(&b, '}');
    res_json(res, status, b.s);
    buf_free(&b);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* %XX decoding; with plus_is_space, '+' means ' ' (query strings). */
static char *url_decode(const char *s, size_t n, int plus_is_space) {
    char *out = xmalloc(n + 1);
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            out[w++] = (char)(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        } else if (plus_is_space && s[i] == '+') {
            out[w++] = ' ';
        } else {
            out[w++] = s[i];
        }
    }
    out[w] = '\0';
    return out;
}

static void parse_query(Request *req, const char *q) {
    while (*q && req->nparams < HTTP_MAX_PARAMS) {
        size_t len = strcspn(q, "&;");
        if (len) {
            const char *eq = memchr(q, '=', len);
            size_t klen = eq ? (size_t)(eq - q) : len;
            req->keys[req->nparams] = url_decode(q, klen, 1);
            req->values[req->nparams] = eq ? url_decode(eq + 1, len - klen - 1, 1) : xstrdup("");
            req->nparams++;
        }
        q += len;
        if (*q) q++;
    }
}

static void request_clear(Request *req) {
    free(req->path);
    free(req->raw_path);
    for (int i = 0; i < req->nheaders; i++) {
        free(req->headers[i].name);
        free(req->headers[i].value);
    }
    for (int i = 0; i < req->nparams; i++) {
        free(req->keys[i]);
        free(req->values[i]);
    }
    char client[64];
    memcpy(client, req->client, sizeof client);
    memset(req, 0, sizeof *req);
    memcpy(req->client, client, sizeof client);
}

static void response_clear(Response *res) {
    buf_free(&res->body);
    for (int i = 0; i < res->nheaders; i++) {
        free(res->headers[i].name);
        free(res->headers[i].value);
    }
    res->nheaders = 0;
}

static int send_all(int fd, const char *data, size_t len) {
    while (len) {
        ssize_t n = send(fd, data, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

static int is_text(const char *type) {
    return type && (strncmp(type, "text/", 5) == 0 || strstr(type, "json") || strstr(type, "javascript") ||
                    strstr(type, "svg") || strstr(type, "xml"));
}

static int gzip_body(const Buf *in, Buf *out) {
    z_stream s;
    memset(&s, 0, sizeof s);
    if (deflateInit2(&s, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return -1;   /* +16 = gzip */
    size_t cap = deflateBound(&s, (uLong)in->len) + 32;
    buf_init(out);
    free(out->s);
    out->s = xmalloc(cap);
    out->cap = cap;
    s.next_in = (unsigned char *)in->s;
    s.avail_in = (uInt)in->len;
    s.next_out = (unsigned char *)out->s;
    s.avail_out = (uInt)cap;
    int rc = deflate(&s, Z_FINISH);
    out->len = s.total_out;
    deflateEnd(&s);
    if (rc != Z_STREAM_END) {
        buf_free(out);
        return -1;
    }
    return 0;
}

static int write_response(int fd, const Request *req, Response *res, int keep_alive) {
    const char *type = res_get_header(res, "Content-Type");
    const char *accept = req_header(req, "accept-encoding");
    int head_only = strcmp(req->method, "HEAD") == 0;
    Buf zipped = {0};
    const Buf *body = &res->body;
    if (res->status != 304 && body->len > 1024 && is_text(type) && accept && strstr(accept, "gzip") &&
        !res_get_header(res, "Content-Encoding") && gzip_body(&res->body, &zipped) == 0) {
        body = &zipped;
        res_header(res, "Content-Encoding", "gzip");
        res_header(res, "Vary", "Accept-Encoding");
    }
    Buf head;
    buf_init(&head);
    buf_printf(&head, "HTTP/1.1 %d %s\r\n", res->status, http_reason(res->status));
    for (int i = 0; i < res->nheaders; i++) buf_printf(&head, "%s: %s\r\n", res->headers[i].name, res->headers[i].value);
    if (res->status != 304) buf_printf(&head, "Content-Length: %zu\r\n", body->len);
    buf_printf(&head, "Connection: %s\r\n\r\n", keep_alive ? "keep-alive" : "close");
    int rc = send_all(fd, head.s, head.len);
    if (rc == 0 && !head_only && res->status != 304) rc = send_all(fd, body->s, body->len);
    buf_free(&head);
    if (zipped.s) buf_free(&zipped);
    return rc;
}

/* ------------------------------------------------------------ connections */

typedef struct {
    int fd;
    char peer[64];
    HttpHandler handler;
    void *ctx;
    int trust_proxy;
} Conn;

static void simple_error(int fd, int status) {
    Response res = {0};
    buf_init(&res.body);
    res_detail(&res, status, http_reason(status));
    Request req = {0};
    snprintf(req.method, sizeof req.method, "GET");
    write_response(fd, &req, &res, 0);
    response_clear(&res);
}

static void *serve_connection(void *arg) {
    Conn *c = arg;
    char *buf = xmalloc(MAX_HEAD + 1);
    size_t have = 0;
    Request req;
    memset(&req, 0, sizeof req);
    for (int served = 0; served < MAX_REQUESTS_PER_CONNECTION; served++) {
        /* 1. Read until the blank line that ends the headers. */
        char *end = NULL;
        while (!(end = (have ? strstr(buf, "\r\n\r\n") : NULL))) {
            if (have >= MAX_HEAD) {
                simple_error(c->fd, 431);
                goto done;
            }
            ssize_t n = recv(c->fd, buf + have, MAX_HEAD - have, 0);
            if (n <= 0) goto done;                   /* closed, or idle too long */
            have += (size_t)n;
            buf[have] = '\0';
        }
        size_t head_len = (size_t)(end - buf) + 4;
        *end = '\0';

        /* 2. Request line: METHOD target HTTP/1.x */
        request_clear(&req);
        char *line_end = strstr(buf, "\r\n");
        if (line_end) *line_end = '\0';
        char target[MAX_HEAD], version[32];
        if (sscanf(buf, "%15s %16000s %31s", req.method, target, version) != 3 || strncmp(version, "HTTP/1.", 7) != 0) {
            simple_error(c->fd, 400);
            goto done;
        }
        int keep_alive = strcmp(version, "HTTP/1.1") == 0;

        /* 3. Headers */
        char *p = line_end ? line_end + 2 : buf + strlen(buf);
        while (*p && req.nheaders < HTTP_MAX_HEADERS) {
            char *eol = strstr(p, "\r\n");
            if (eol) *eol = '\0';
            char *colon = strchr(p, ':');
            if (colon) {
                *colon = '\0';
                char *value = colon + 1;
                while (*value == ' ' || *value == '\t') value++;
                size_t vlen = strlen(value);
                while (vlen && (value[vlen - 1] == ' ' || value[vlen - 1] == '\t')) value[--vlen] = '\0';
                req.headers[req.nheaders].name = xstrdup(p);
                req.headers[req.nheaders].value = xstrdup(value);
                req.nheaders++;
            }
            if (!eol) break;
            p = eol + 2;
        }
        const char *connection = req_header(&req, "connection");
        if (connection && strcasecmp(connection, "close") == 0) keep_alive = 0;
        if (connection && strcasecmp(connection, "keep-alive") == 0) keep_alive = 1;

        /* 4. Body: not used by any endpoint, but read it so the next request lines up. */
        const char *te = req_header(&req, "transfer-encoding");
        if (te) {
            simple_error(c->fd, 400);
            goto done;
        }
        long body_len = 0;
        const char *cl = req_header(&req, "content-length");
        if (cl && (!parse_long(cl, &body_len) || body_len < 0 || body_len > (long)MAX_BODY)) {
            simple_error(c->fd, body_len > (long)MAX_BODY ? 413 : 400);
            goto done;
        }
        size_t leftover = have - head_len;
        size_t skip = (size_t)body_len;
        if (leftover >= skip) {
            memmove(buf, buf + head_len + skip, leftover - skip);
            have = leftover - skip;
        } else {
            skip -= leftover;
            have = 0;
            char drain[4096];
            while (skip) {
                ssize_t n = recv(c->fd, drain, skip < sizeof drain ? skip : sizeof drain, 0);
                if (n <= 0) goto done;
                skip -= (size_t)n;
            }
        }
        buf[have] = '\0';

        /* 5. Path and query string */
        char *q = strchr(target, '?');
        if (q) *q = '\0';
        req.raw_path = xstrdup(target);
        req.path = url_decode(target, strlen(target), 0);
        if (q) parse_query(&req, q + 1);

        snprintf(req.client, sizeof req.client, "%s", c->peer);
        const char *fwd = req_header(&req, "x-forwarded-for");
        if (c->trust_proxy && fwd && *fwd) {
            size_t n = strcspn(fwd, ",");
            while (n && fwd[n - 1] == ' ') n--;
            while (n && *fwd == ' ') fwd++, n--;
            if (n && n < sizeof req.client) {
                memcpy(req.client, fwd, n);
                req.client[n] = '\0';
            }
        }

        /* 6. Let the application answer, then send it. */
        Response res = {0};
        buf_init(&res.body);
        res.status = 200;
        double t0 = now_monotonic();
        c->handler(&req, &res, c->ctx);
        int rc = write_response(c->fd, &req, &res, keep_alive);
        fprintf(stderr, "%s %s %d %.0fms\n", req.method, req.path, res.status, (now_monotonic() - t0) * 1000);
        response_clear(&res);
        if (rc != 0 || !keep_alive) break;
    }
done:
    request_clear(&req);
    free(buf);
    close(c->fd);
    free(c);
    atomic_fetch_sub(&open_connections, 1);
    return NULL;
}

int http_serve(const char *host, int port, HttpHandler handler, void *ctx, int trust_proxy) {
    signal(SIGPIPE, SIG_IGN);                     /* a closed browser tab must not kill the server */
    struct addrinfo hints = {0}, *ai;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    int rc = getaddrinfo(host, portstr, &hints, &ai);
    if (rc != 0) {
        fprintf(stderr, "error: cannot resolve %s: %s\n", host, gai_strerror(rc));
        return -1;
    }
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (fd < 0 || bind(fd, ai->ai_addr, ai->ai_addrlen) != 0 || listen(fd, 256) != 0) {
        fprintf(stderr, "error: cannot listen on %s:%d (%s)\n", host, port, strerror(errno));
        freeaddrinfo(ai);
        return -1;
    }
    freeaddrinfo(ai);

    for (;;) {
        struct sockaddr_storage peer;
        socklen_t plen = sizeof peer;
        int client = accept(fd, (struct sockaddr *)&peer, &plen);
        if (client < 0) {
            if (errno == EINTR || errno == ECONNABORTED || errno == EMFILE || errno == ENFILE) continue;
            fprintf(stderr, "error: accept failed: %s\n", strerror(errno));
            continue;
        }
        struct timeval tv = {KEEPALIVE_SECONDS, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        if (atomic_load(&open_connections) >= MAX_CONNECTIONS) {
            simple_error(client, 503);
            close(client);
            continue;
        }
        Conn *c = xcalloc(1, sizeof *c);
        c->fd = client;
        c->handler = handler;
        c->ctx = ctx;
        c->trust_proxy = trust_proxy;
        if (peer.ss_family == AF_INET)
            inet_ntop(AF_INET, &((struct sockaddr_in *)&peer)->sin_addr, c->peer, sizeof c->peer);
        else if (peer.ss_family == AF_INET6)
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&peer)->sin6_addr, c->peer, sizeof c->peer);
        else snprintf(c->peer, sizeof c->peer, "unknown");
        atomic_fetch_add(&open_connections, 1);
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 1u << 20);
        if (pthread_create(&th, &attr, serve_connection, c) != 0) {
            atomic_fetch_sub(&open_connections, 1);
            close(client);
            free(c);
        }
        pthread_attr_destroy(&attr);
    }
}
