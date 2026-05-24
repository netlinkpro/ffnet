// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef FFNET_FFHTTP_H
#define FFNET_FFHTTP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HTTP/1.1 protocol module — public handler API.
 *
 * To run an HTTP server:
 *   1. ffhttp_set_handler(my_handler, my_ctx);
 *   2. ffnet_server_run(&ffproto_http, host, port, workers);
 *
 * The handler is invoked once per request, after the full request
 * (line + headers + body) has been received and parsed. Inside the
 * handler, set the response status and headers, write the body. The
 * framework serializes the response and manages keep-alive.
 *
 * The handler runs in the worker thread that's handling the request.
 * It MUST NOT block (workers are shared across many connections); it
 * MUST NOT retain `req` or `resp` pointers after returning. */

typedef struct HttpRequest  HttpRequest;
typedef struct HttpResponse HttpResponse;

/* ----- HttpRequest accessors --------------------------------------------- */

/* Returns "GET", "HEAD", "POST", or any other token from the request line.
 * Lifetime: valid for the duration of the handler call. */
const char    *ffhttp_req_method(const HttpRequest *r);

/* Decoded path portion (e.g. "/foo/bar.html"). Percent-decoded; "../"
 * segments are rejected by the framework before reaching the handler. */
const char    *ffhttp_req_path  (const HttpRequest *r);

/* Query string (everything after "?" in the request URI), or "" if none.
 * Not decoded — caller can split on "&" / "=" and decode as needed. */
const char    *ffhttp_req_query (const HttpRequest *r);

/* Returns the value of the named header (case-insensitive) or NULL if
 * the header is not present. If a header appears multiple times, the
 * value of the first occurrence is returned. */
const char    *ffhttp_req_header(const HttpRequest *r, const char *name);

/* Returns a pointer to the request body bytes (NULL if none) and writes
 * the body length to *out_len. The body has been fully accumulated by
 * the time the handler is called. */
const uint8_t *ffhttp_req_body  (const HttpRequest *r, size_t *out_len);

/* ----- HttpResponse builders --------------------------------------------- */

/* Set the response status. Default is 200 if never called. */
void ffhttp_resp_status(HttpResponse *r, int code);

/* Add a response header. Returns FFE_OK or negative. The framework
 * automatically adds Date, Server, Content-Length (or Transfer-Encoding:
 * chunked), and Connection — handlers should NOT set these. */
int  ffhttp_resp_header(HttpResponse *r, const char *name, const char *value);

/* Append `n` bytes to the response body. May be called multiple times.
 * If total body fits in memory, the framework sends Content-Length;
 * otherwise it switches to chunked transfer encoding automatically. */
int  ffhttp_resp_body  (HttpResponse *r, const void *data, size_t n);

/* ----- Handler registration ---------------------------------------------- */

/* Handler signature. Return 0 on success; the framework sends whatever
 * status + body the handler set on `resp`. Negative return becomes a 500
 * response if the handler didn't already set a status. */
typedef int (*HttpHandler)(const HttpRequest *req, HttpResponse *resp, void *user);

/* Register the single global handler. Slice 2 keeps the model simple
 * (one handler per process). Call once at startup before
 * `ffnet_server_run(&ffproto_http, ...)`. */
void ffhttp_set_handler(HttpHandler handler, void *user);

/* ----- Built-in handlers ------------------------------------------------- */

/* Serves files from the directory passed as `user` (a const char *).
 * Supports GET / HEAD, Range, If-Modified-Since, If-None-Match. */
int  ffhttp_static_handler(const HttpRequest *req, HttpResponse *resp, void *user);

/* Echoes the request body back as the response body. Sets
 * Content-Type: application/octet-stream. Any method accepted. */
int  ffhttp_echo_handler  (const HttpRequest *req, HttpResponse *resp, void *user);

#ifdef __cplusplus
}
#endif
#endif /* FFNET_FFHTTP_H */
