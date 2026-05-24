// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef FFNET_INTERNAL_HTTP_H
#define FFNET_INTERNAL_HTTP_H

#include "ffnet/ffhttp.h"
#include "ffnet/ffutil.h"

#include <stddef.h>
#include <stdint.h>

/* Header list — array of (name, value) pairs into a shared header arena.
 * The arena is a contiguous buffer holding all header strings; pairs
 * reference into it via offsets. */
typedef struct HeaderPair {
    size_t name_off;
    size_t name_len;
    size_t value_off;
    size_t value_len;
} HeaderPair;

/* HttpRequest — produced by http_parse, consumed by the handler. */
struct HttpRequest {
    char        *method;       /* "GET", "POST", ... */
    char        *path;         /* "/foo.html" (percent-decoded) */
    char        *query;        /* "" or "a=1&b=2" (not decoded) */
    char        *version;      /* "HTTP/1.1" */

    char        *header_arena; /* contiguous storage for header names + values */
    size_t       header_arena_len;
    HeaderPair  *headers;
    size_t       header_count;
    size_t       header_cap;

    uint8_t     *body;
    size_t       body_len;
};

/* HttpResponse — built by the handler, serialized by http_resp. */
struct HttpResponse {
    int          status;
    char        *header_arena;
    size_t       header_arena_len;
    size_t       header_arena_cap;
    HeaderPair  *headers;
    size_t       header_count;
    size_t       header_cap;
    FrameBuf     body;
};

/* Asm-side parser helpers. */
extern int64_t _ff_asm_http_find_eoh(const uint8_t *buf, uint64_t len);
extern int64_t _ff_asm_http_find_eol(const uint8_t *buf, uint64_t len);
extern int     _ff_asm_http_ascii_lower_eq(const uint8_t *a, const uint8_t *b,
                                            uint64_t n);

/* Parser entry points. */
HttpRequest  *http_request_new(void);
void          http_request_free(HttpRequest *r);
HttpResponse *http_response_new(void);
void          http_response_free(HttpResponse *r);

/* Parse request line + headers from buf[0..eoh). `eoh` is the offset
 * returned by find_eoh (one past the "\r\n\r\n"). On success, fills `req`
 * and returns FFE_OK. On parse failure, returns FFE_PROTOCOL. */
int  http_parse_request(const uint8_t *buf, size_t eoh, HttpRequest *req);

/* Chunked decoder — incremental. Advances state through buf, appends
 * decoded body bytes to `out`. Returns:
 *   FFE_OK    — body complete (zero-length terminator consumed); writes
 *               `*consumed` = bytes from buf consumed.
 *   FFE_AGAIN — need more bytes; consumed updated to bytes processed so far.
 *   FFE_PROTOCOL — malformed.
 * State carried in *chunk_state and *chunk_remaining; caller initializes
 * both to 0 at the start of a new chunked body. */
int  http_chunk_decode(const uint8_t *buf, size_t len, size_t *consumed,
                       int *chunk_state, size_t *chunk_remaining,
                       FrameBuf *out);

/* URI percent-decoding. Decodes src into dst (cap bytes). Returns the
 * length written (excluding NUL terminator if any) or negative on error.
 * dst is NOT NUL-terminated by this function. */
int  http_pct_decode(char *dst, size_t cap, const char *src, size_t src_len);

/* Returns 1 if path is safe (no .., no embedded NUL, no leading absolute
 * path, etc.); 0 otherwise. */
int  http_path_safe(const char *path);

/* MIME lookup by file extension (just the extension, no dot).
 * Returns "application/octet-stream" if unknown. */
const char *http_mime_for_ext(const char *ext);

/* Serialize `resp` into `out`. Adds Date, Server, Connection,
 * Content-Length (or Transfer-Encoding: chunked). If `keep_alive` is 1,
 * sets Connection: keep-alive (default for HTTP/1.1); else
 * Connection: close. */
int  http_response_serialize(const HttpResponse *resp, int keep_alive,
                             FrameBuf *out);

#endif /* FFNET_INTERNAL_HTTP_H */
