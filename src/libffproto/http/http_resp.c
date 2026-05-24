// SPDX-License-Identifier: LGPL-2.1-or-later
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------------------
 * HttpResponse allocation + accessor implementations (ffhttp.h surface).
 * --------------------------------------------------------------------------*/

HttpResponse *http_response_new(void)
{
    HttpResponse *r = (HttpResponse *)calloc(1, sizeof(HttpResponse));
    if (!r) return NULL;
    r->status = 200;
    return r;
}

void http_response_free(HttpResponse *r)
{
    if (!r) return;
    free(r->header_arena);
    free(r->headers);
    framebuf_free(&r->body);
    free(r);
}

void ffhttp_resp_status(HttpResponse *r, int code)
{
    if (r) r->status = code;
}

static int grow_resp_headers(HttpResponse *r)
{
    if (r->header_count < r->header_cap) return FFE_OK;
    size_t nc = r->header_cap ? r->header_cap * 2 : 8;
    HeaderPair *p = (HeaderPair *)realloc(r->headers, nc * sizeof(HeaderPair));
    if (!p) return FFE_NOMEM;
    r->headers = p;
    r->header_cap = nc;
    return FFE_OK;
}

static int grow_resp_arena(HttpResponse *r, size_t need)
{
    if (r->header_arena_len + need <= r->header_arena_cap) return FFE_OK;
    size_t nc = r->header_arena_cap ? r->header_arena_cap : 256;
    while (nc < r->header_arena_len + need) nc *= 2;
    char *p = (char *)realloc(r->header_arena, nc);
    if (!p) return FFE_NOMEM;
    r->header_arena = p;
    r->header_arena_cap = nc;
    return FFE_OK;
}

int ffhttp_resp_header(HttpResponse *r, const char *name, const char *value)
{
    if (!r || !name || !value) return FFE_INVAL;
    size_t nlen = strlen(name);
    size_t vlen = strlen(value);
    int rc = grow_resp_arena(r, nlen + vlen);
    if (rc != FFE_OK) return rc;
    rc = grow_resp_headers(r);
    if (rc != FFE_OK) return rc;

    HeaderPair *hp = &r->headers[r->header_count];
    hp->name_off = r->header_arena_len;
    hp->name_len = nlen;
    memcpy(r->header_arena + r->header_arena_len, name, nlen);
    r->header_arena_len += nlen;

    hp->value_off = r->header_arena_len;
    hp->value_len = vlen;
    memcpy(r->header_arena + r->header_arena_len, value, vlen);
    r->header_arena_len += vlen;

    r->header_count++;
    return FFE_OK;
}

int ffhttp_resp_body(HttpResponse *r, const void *data, size_t n)
{
    if (!r) return FFE_INVAL;
    return framebuf_append(&r->body, data, n);
}

/* ---------------------------------------------------------------------------
 * IMF-fixdate formatter. Hand-formatted to avoid locale dependence
 * that strftime("%a, %d %b %Y...") exhibits on some glibcs.
 * --------------------------------------------------------------------------*/

static void format_imf_date(char out[40])
{
    static const char *days[]   = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                    "Jul","Aug","Sep","Oct","Nov","Dec"};
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    snprintf(out, 40, "%s, %02d %s %04d %02d:%02d:%02d GMT",
             days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon],
             tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* ---------------------------------------------------------------------------
 * Status-line formatting.
 * --------------------------------------------------------------------------*/

static const char *reason_phrase(int code)
{
    switch (code) {
    case 100: return "Continue";
    case 200: return "OK";
    case 206: return "Partial Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 416: return "Range Not Satisfiable";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default:  return "OK";
    }
}

/* ---------------------------------------------------------------------------
 * Serialize the response into out.
 *
 * Auto headers (we add unless the handler explicitly set them):
 *   Server, Date, Content-Length, Connection
 *
 * Slice 2 uses Content-Length always (handler-buffered body). Chunked-
 * encoded responses are deferred to a future slice (would need a
 * streaming handler interface).
 * --------------------------------------------------------------------------*/

static int header_set_by_handler(const HttpResponse *r, const char *name)
{
    size_t nlen = strlen(name);
    for (size_t i = 0; i < r->header_count; i++) {
        if (r->headers[i].name_len != nlen) continue;
        if (_ff_asm_http_ascii_lower_eq(
                (const uint8_t *)(r->header_arena + r->headers[i].name_off),
                (const uint8_t *)name, nlen)) {
            return 1;
        }
    }
    return 0;
}

int http_response_serialize(const HttpResponse *resp, int keep_alive,
                            FrameBuf *out)
{
    char line[256];
    int n;

    /* Status line. */
    n = snprintf(line, sizeof line, "HTTP/1.1 %d %s\r\n",
                 resp->status, reason_phrase(resp->status));
    int rc = framebuf_append(out, line, (size_t)n);
    if (rc != FFE_OK) return rc;

    /* Handler-set headers (verbatim from the arena). */
    for (size_t i = 0; i < resp->header_count; i++) {
        rc = framebuf_append(out,
            resp->header_arena + resp->headers[i].name_off,
            resp->headers[i].name_len);
        if (rc != FFE_OK) return rc;
        rc = framebuf_append(out, ": ", 2);
        if (rc != FFE_OK) return rc;
        rc = framebuf_append(out,
            resp->header_arena + resp->headers[i].value_off,
            resp->headers[i].value_len);
        if (rc != FFE_OK) return rc;
        rc = framebuf_append(out, "\r\n", 2);
        if (rc != FFE_OK) return rc;
    }

    /* Auto headers. */
    if (!header_set_by_handler(resp, "Server")) {
        rc = framebuf_append(out, "Server: ffnet/0.1\r\n", 19);
        if (rc != FFE_OK) return rc;
    }
    if (!header_set_by_handler(resp, "Date")) {
        char dbuf[40];
        format_imf_date(dbuf);
        n = snprintf(line, sizeof line, "Date: %s\r\n", dbuf);
        rc = framebuf_append(out, line, (size_t)n);
        if (rc != FFE_OK) return rc;
    }
    if (!header_set_by_handler(resp, "Content-Length")) {
        n = snprintf(line, sizeof line, "Content-Length: %zu\r\n", resp->body.len);
        rc = framebuf_append(out, line, (size_t)n);
        if (rc != FFE_OK) return rc;
    }
    if (!header_set_by_handler(resp, "Connection")) {
        rc = framebuf_append(out,
            keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n",
            keep_alive ? 24 : 19);
        if (rc != FFE_OK) return rc;
    }

    /* End of headers. */
    rc = framebuf_append(out, "\r\n", 2);
    if (rc != FFE_OK) return rc;

    /* Body. */
    if (resp->body.len > 0) {
        rc = framebuf_append(out, resp->body.data, resp->body.len);
        if (rc != FFE_OK) return rc;
    }

    return FFE_OK;
}
