// SPDX-License-Identifier: LGPL-2.1-or-later
#include "http.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Allocation helpers.
 * --------------------------------------------------------------------------*/

HttpRequest *http_request_new(void)
{
    return (HttpRequest *)calloc(1, sizeof(HttpRequest));
}

void http_request_free(HttpRequest *r)
{
    if (!r) return;
    free(r->method);
    free(r->path);
    free(r->query);
    free(r->version);
    free(r->header_arena);
    free(r->headers);
    free(r->body);
    free(r);
}

/* ---------------------------------------------------------------------------
 * Percent-decoding (in place into dst).
 * --------------------------------------------------------------------------*/

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int http_pct_decode(char *dst, size_t cap, const char *src, size_t src_len)
{
    size_t w = 0;
    for (size_t i = 0; i < src_len; i++) {
        if (w >= cap) return FFE_NOMEM;
        char c = src[i];
        if (c == '%') {
            if (i + 2 >= src_len) return FFE_PROTOCOL;
            int hi = hexval((unsigned char)src[i + 1]);
            int lo = hexval((unsigned char)src[i + 2]);
            if (hi < 0 || lo < 0) return FFE_PROTOCOL;
            dst[w++] = (char)((hi << 4) | lo);
            i += 2;
        } else {
            dst[w++] = c;
        }
    }
    return (int)w;
}

int http_path_safe(const char *path)
{
    if (!path || path[0] != '/') return 0;
    /* Reject NUL bytes (caller passed a NUL-terminated string but the
     * decoded content may contain embedded NULs). */
    /* Scan for "/../" or trailing "/.." or starting "../" — anything
     * that would let a path segment of ".." appear. */
    const char *p = path;
    while (*p) {
        if (p[0] == '/' && p[1] == '.' && p[2] == '.' &&
            (p[3] == '/' || p[3] == '\0')) {
            return 0;
        }
        if (*p == '\0') break;
        p++;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * Request line + headers parser.
 * --------------------------------------------------------------------------*/

static char *strdup_n(const char *s, size_t n)
{
    char *r = (char *)malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

/* Grow headers array (geometric). */
static int grow_headers(HttpRequest *req)
{
    if (req->header_count < req->header_cap) return FFE_OK;
    size_t nc = req->header_cap ? req->header_cap * 2 : 16;
    HeaderPair *p = (HeaderPair *)realloc(req->headers, nc * sizeof(HeaderPair));
    if (!p) return FFE_NOMEM;
    req->headers = p;
    req->header_cap = nc;
    return FFE_OK;
}

/* Append (name, value) into the arena. Both are stored verbatim; case-
 * insensitive comparison happens at lookup time. */
static int append_header(HttpRequest *req,
                         const uint8_t *name, size_t nlen,
                         const uint8_t *value, size_t vlen)
{
    int rc = grow_headers(req);
    if (rc != FFE_OK) return rc;

    /* The arena is pre-sized at parse start; we never grow it here. */
    HeaderPair *hp = &req->headers[req->header_count];
    hp->name_off  = req->header_arena_len;
    hp->name_len  = nlen;
    memcpy(req->header_arena + req->header_arena_len, name, nlen);
    req->header_arena_len += nlen;

    hp->value_off = req->header_arena_len;
    hp->value_len = vlen;
    memcpy(req->header_arena + req->header_arena_len, value, vlen);
    req->header_arena_len += vlen;

    req->header_count++;
    return FFE_OK;
}

int http_parse_request(const uint8_t *buf, size_t eoh, HttpRequest *req)
{
    /* Find request-line end. */
    int64_t line_end = _ff_asm_http_find_eol(buf, eoh);
    if (line_end < 0) return FFE_PROTOCOL;

    /* Split request line: METHOD SP URI SP VERSION CRLF. */
    size_t line_len = (size_t)line_end - 2;     /* exclude CRLF */
    const uint8_t *sp1 = (const uint8_t *)memchr(buf, ' ', line_len);
    if (!sp1) return FFE_PROTOCOL;
    size_t method_len = (size_t)(sp1 - buf);
    const uint8_t *uri_start = sp1 + 1;
    size_t after_method = method_len + 1;
    if (after_method >= line_len) return FFE_PROTOCOL;
    const uint8_t *sp2 = (const uint8_t *)memchr(uri_start, ' ',
                                                 line_len - after_method);
    if (!sp2) return FFE_PROTOCOL;
    size_t uri_len = (size_t)(sp2 - uri_start);
    const uint8_t *ver_start = sp2 + 1;
    size_t ver_len = line_len - (method_len + 1 + uri_len + 1);

    /* Save method + version. */
    req->method  = strdup_n((const char *)buf, method_len);
    req->version = strdup_n((const char *)ver_start, ver_len);
    if (!req->method || !req->version) return FFE_NOMEM;

    /* Split URI into path?query. */
    const uint8_t *q = (const uint8_t *)memchr(uri_start, '?', uri_len);
    size_t path_raw_len = q ? (size_t)(q - uri_start) : uri_len;
    size_t query_len    = q ? uri_len - path_raw_len - 1 : 0;

    /* Percent-decode the path. */
    char *path = (char *)malloc(path_raw_len + 1);
    if (!path) return FFE_NOMEM;
    int dl = http_pct_decode(path, path_raw_len, (const char *)uri_start,
                              path_raw_len);
    if (dl < 0) { free(path); return FFE_PROTOCOL; }
    path[dl] = '\0';
    req->path = path;

    /* Query stays as raw bytes (not decoded). */
    if (query_len > 0) {
        req->query = strdup_n((const char *)(q + 1), query_len);
        if (!req->query) return FFE_NOMEM;
    } else {
        req->query = strdup_n("", 0);
        if (!req->query) return FFE_NOMEM;
    }

    /* Allocate header arena sized to fit the entire header block. */
    if (eoh >= (size_t)line_end + 2) {
        size_t arena_cap = eoh - line_end;
        req->header_arena = (char *)malloc(arena_cap);
        if (!req->header_arena) return FFE_NOMEM;
    }

    /* Walk headers line by line until empty line. */
    size_t off = (size_t)line_end;
    while (off + 2 <= eoh) {
        if (buf[off] == '\r' && buf[off + 1] == '\n') {
            /* Empty line -> end of headers. */
            return FFE_OK;
        }

        int64_t le = _ff_asm_http_find_eol(buf + off, eoh - off);
        if (le < 0) return FFE_PROTOCOL;
        size_t hdr_len = (size_t)le - 2;        /* exclude CRLF */

        /* Find colon. */
        const uint8_t *colon = (const uint8_t *)memchr(buf + off, ':', hdr_len);
        if (!colon) return FFE_PROTOCOL;
        size_t name_len = (size_t)(colon - (buf + off));
        const uint8_t *vstart = colon + 1;
        size_t vlen = hdr_len - name_len - 1;

        /* Trim leading whitespace on value. */
        while (vlen > 0 && (*vstart == ' ' || *vstart == '\t')) {
            vstart++; vlen--;
        }
        /* Trim trailing whitespace on value. */
        while (vlen > 0 && (vstart[vlen - 1] == ' ' || vstart[vlen - 1] == '\t')) {
            vlen--;
        }

        int rc = append_header(req, buf + off, name_len, vstart, vlen);
        if (rc != FFE_OK) return rc;

        off += (size_t)le;
    }

    return FFE_OK;
}

/* ---------------------------------------------------------------------------
 * Header lookup (called via the public ffhttp_req_header accessor).
 * Linear scan with case-insensitive compare via the asm helper.
 * --------------------------------------------------------------------------*/

static const char *header_lookup(const HttpRequest *r, const char *name,
                                  size_t *out_len)
{
    size_t nlen = strlen(name);
    for (size_t i = 0; i < r->header_count; i++) {
        if (r->headers[i].name_len != nlen) continue;
        if (_ff_asm_http_ascii_lower_eq(
                (const uint8_t *)(r->header_arena + r->headers[i].name_off),
                (const uint8_t *)name, nlen)) {
            if (out_len) *out_len = r->headers[i].value_len;
            return r->header_arena + r->headers[i].value_off;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Public accessors (declared in ffhttp.h).
 * --------------------------------------------------------------------------*/

const char *ffhttp_req_method(const HttpRequest *r) { return r ? r->method  : NULL; }
const char *ffhttp_req_path  (const HttpRequest *r) { return r ? r->path    : NULL; }
const char *ffhttp_req_query (const HttpRequest *r) { return r ? r->query   : ""; }

const char *ffhttp_req_header(const HttpRequest *r, const char *name)
{
    if (!r || !name) return NULL;
    size_t vlen;
    const char *v = header_lookup(r, name, &vlen);
    if (!v) return NULL;
    /* Header values are not NUL-terminated in the arena. We return a
     * pointer that's only valid because the lookup happened *and* the
     * arena keeps growing only on parse; during the handler call the
     * arena is frozen and the next byte after the value is some other
     * header's name (or arena end). For caller convenience we return a
     * static buffer with the NUL-terminated copy. NOT thread-safe by
     * itself; one buffer per call site would be needed for that. Slice
     * 2 keeps it simple — most handlers compare against expected
     * values rather than reading raw. */
    static __thread char tbuf[4096];
    if (vlen + 1 > sizeof tbuf) return NULL;
    memcpy(tbuf, v, vlen);
    tbuf[vlen] = '\0';
    return tbuf;
}

const uint8_t *ffhttp_req_body(const HttpRequest *r, size_t *out_len)
{
    if (!r) { if (out_len) *out_len = 0; return NULL; }
    if (out_len) *out_len = r->body_len;
    return r->body;
}

/* ---------------------------------------------------------------------------
 * Chunked decoder.
 *
 * State values (chunk_state):
 *   0 — expecting size line
 *   1 — reading data
 *   2 — expecting CRLF after data
 *   3 — done (zero-length chunk consumed)
 * --------------------------------------------------------------------------*/

int http_chunk_decode(const uint8_t *buf, size_t len, size_t *consumed,
                      int *chunk_state, size_t *chunk_remaining,
                      FrameBuf *out)
{
    size_t i = 0;
    while (i < len) {
        switch (*chunk_state) {
        case 0: {
            /* Find CRLF ending the size line. */
            int64_t le = _ff_asm_http_find_eol(buf + i, len - i);
            if (le < 0) { *consumed = i; return FFE_AGAIN; }

            /* Parse hex up to the first ';' (chunk extension) or CRLF. */
            size_t sz = 0;
            size_t j = i;
            while (j < i + (size_t)le - 2) {
                int v = hexval(buf[j]);
                if (v < 0) {
                    if (buf[j] == ';') break;       /* chunk-ext */
                    *consumed = i; return FFE_PROTOCOL;
                }
                if (sz > (SIZE_MAX >> 4)) { *consumed = i; return FFE_PROTOCOL; }
                sz = (sz << 4) | (size_t)v;
                j++;
            }
            *chunk_remaining = sz;
            i += (size_t)le;
            if (sz == 0) {
                /* Zero-length chunk — consume optional trailer headers. */
                *chunk_state = 3;
            } else {
                *chunk_state = 1;
            }
            break;
        }
        case 1: {
            /* Reading data bytes. */
            size_t avail = len - i;
            size_t take = avail < *chunk_remaining ? avail : *chunk_remaining;
            int rc = framebuf_append(out, buf + i, take);
            if (rc != FFE_OK) { *consumed = i; return rc; }
            i += take;
            *chunk_remaining -= take;
            if (*chunk_remaining == 0) {
                *chunk_state = 2;
            } else {
                *consumed = i; return FFE_AGAIN;
            }
            break;
        }
        case 2: {
            /* Expect CRLF. */
            if (len - i < 2) { *consumed = i; return FFE_AGAIN; }
            if (buf[i] != '\r' || buf[i + 1] != '\n') {
                *consumed = i; return FFE_PROTOCOL;
            }
            i += 2;
            *chunk_state = 0;
            break;
        }
        case 3: {
            /* Drain trailer headers until empty line. Slice 2 ignores trailer values. */
            int64_t le = _ff_asm_http_find_eol(buf + i, len - i);
            if (le < 0) { *consumed = i; return FFE_AGAIN; }
            if (le == 2) {
                /* Bare CRLF — body is fully consumed. */
                i += 2;
                *consumed = i;
                return FFE_OK;
            }
            i += (size_t)le;
            break;
        }
        default:
            *consumed = i;
            return FFE_PROTOCOL;
        }
    }
    *consumed = i;
    return FFE_AGAIN;
}
