# Slice 2 — Full HTTP/1.1 server (second protocol module)

Status: approved 2026-05-24

## Context

Slice 1 family (initial + 1.5 errors + 1.6 Autobahn + 1.7 event loop) is
complete. The Protocol ABI v3 from 1.7 was designed to support multiple
protocols; the registry has one entry (`ffproto_websocket`). Slice 2 adds
the second: `ffproto_http`.

Per the plan, slice 2 was originally framed as the slice that would force
the first ABI evolution. With ABI v3 already in place, slice 2 is purely
additive — HTTP/1.1 fits the v3 contract without any ABI change.

User chose "Full HTTP/1.1" in one bundled slice (vs the 2a/2b/2c
decomposition). User also chose to include asm hot paths for the
parser-side scanning operations.

## Goals

- `ffnet http-serve --listen <addr>:<port> --root <dir> [--workers N]` is a
  production-shaped static file server. `curl http://host:port/file.html`
  works.
- `ffnet http-echo --listen <addr>:<port> [--workers N]` echoes POST bodies
  back, exercising chunked decoding and request body handling.
- A pluggable handler API (`include/ffnet/ffhttp.h`) lets future code register
  custom HTTP handlers without modifying the module.
- HTTP feature coverage: GET, HEAD, POST; keep-alive; Content-Length AND
  chunked request bodies; Content-Length OR chunked response bodies (auto);
  single-range `Range` with 206; cache validators (`Last-Modified` +
  `If-Modified-Since`, `ETag` + `If-None-Match`) with 304; `Expect:
  100-continue`; status codes 100, 200, 206, 304, 400, 404, 405, 411,
  413, 414, 416, 500, 501; standard headers (`Date`, `Server`,
  `Content-Type`, `Content-Length`, `Connection`, `Transfer-Encoding`).
- Asm hot paths: `find_eoh`, `find_eol`, `ascii_lower_eq` in
  `src/libffproto/http/http_parse.asm` (plain x86_64; SIMD deferred).
- Existing slice-1 work continues to pass: `make test`, `make itest`,
  `make autobahn` (≥ 297/301 strict).

## Non-goals

- HTTPS / TLS (slice 3+).
- HTTP/2 or HTTP/3 (later slice family).
- PUT, DELETE, PATCH, OPTIONS, CONNECT, TRACE (return 405 / 501).
- Compression (`Content-Encoding: gzip`/`br`).
- Multiple ranges per request (single only; if multipart range requested,
  return whole resource with 200).
- Conditional `If-Range`.
- Request pipelining: the server processes one request at a time per
  connection. If a client pipelines, requests serialize naturally. We
  do NOT reject pipelining, just don't interleave.
- Trailer headers.
- WebDAV, cookies, sessions, auth.
- Routing tables. Slice 2 supports one global handler; later slices can
  add path-based routing on top.

## Architecture

```
         ┌─────────────────────────────────────────────────────┐
         │  cli/cmd_http_serve.c  cli/cmd_http_echo.c           │
         │  • parse args                                        │
         │  • ffhttp_set_handler(...)                           │
         │  • ffnet_server_run(&ffproto_http, ...)              │
         └─────────────────────────────────────────────────────┘
                                  │
                                  ▼  (slice 1.7 server runs the loop)
         ┌─────────────────────────────────────────────────────┐
         │  ffproto_http (Protocol v3, src/libffproto/http/)    │
         │  • http_module.c  on_event state machine:            │
         │      WAIT_REQ → READ_HEAD → READ_BODY → HANDLE →     │
         │      WRITE_RESP → (keep-alive ? WAIT_REQ : CLOSING)  │
         │  • http_parse.c   request line + headers + chunked   │
         │  • http_parse.asm find_eoh / find_eol / lower_eq     │
         │  • http_resp.c    HttpResponse buffer + auto-encode  │
         └─────────────────────────────────────────────────────┘
                                  │
                                  ▼  (dispatch to registered handler)
         ┌─────────────────────────────────────────────────────┐
         │  HttpHandler — one of:                               │
         │  • http_static.c   serves files from --root          │
         │  • http_echo.c     returns POST body                 │
         │  • user-provided   ffhttp_set_handler(my_fn, ctx)    │
         └─────────────────────────────────────────────────────┘
```

## Components

### Public header — `include/ffnet/ffhttp.h`

```c
typedef struct HttpRequest  HttpRequest;
typedef struct HttpResponse HttpResponse;

const char    *ffhttp_req_method(const HttpRequest *r);
const char    *ffhttp_req_path  (const HttpRequest *r);
const char    *ffhttp_req_query (const HttpRequest *r);
const char    *ffhttp_req_header(const HttpRequest *r, const char *name);
const uint8_t *ffhttp_req_body  (const HttpRequest *r, size_t *out_len);

void ffhttp_resp_status (HttpResponse *r, int code);
int  ffhttp_resp_header (HttpResponse *r, const char *name, const char *value);
int  ffhttp_resp_body   (HttpResponse *r, const void *data, size_t n);

typedef int (*HttpHandler)(const HttpRequest *req, HttpResponse *resp, void *user);

/* Register the single global handler. May be called once at startup or
 * replaced; not thread-safe by itself, but workers don't replace it once
 * running. */
void ffhttp_set_handler(HttpHandler handler, void *user);
```

The handler's return value is reserved for future use; for slice 2,
returning 0 means "framework will send whatever status + body the
handler set on `resp`." Negative returns become 500 if `resp->status`
wasn't already set.

### State machine — `src/libffproto/http/http_module.c`

Per-connection `HttpConn`:

```c
typedef enum {
    HTTP_STATE_WAIT_REQ,    /* between requests on a keep-alive connection */
    HTTP_STATE_READ_HEAD,   /* accumulating until \r\n\r\n */
    HTTP_STATE_READ_BODY,   /* accumulating Content-Length or chunked body */
    HTTP_STATE_HANDLE,      /* handler runs, builds HttpResponse */
    HTTP_STATE_WRITE_RESP,  /* draining serialized response from wr_buf */
    HTTP_STATE_CLOSING,     /* flush wr_buf then close */
} HttpState;

typedef struct HttpConn {
    HttpState state;
    FrameBuf  rd_buf;
    FrameBuf  wr_buf;
    size_t    wr_pos;

    /* Per-request scratch (reset at each WAIT_REQ → READ_HEAD transition): */
    HttpRequest  *req;          /* allocated/owned by this conn */
    HttpResponse *resp;
    int           keep_alive;   /* 1 unless client/server requested close */
    size_t        body_remaining;  /* bytes still to accumulate */
    int           chunked;      /* request body uses Transfer-Encoding chunked */
    size_t        chunk_remaining; /* bytes remaining in current chunk */
    int           chunk_state;  /* 0=size line, 1=data, 2=trailer line, 3=done */
} HttpConn;
```

`on_event`:

1. **READ:** drain inbound bytes to `rd_buf` until EAGAIN or EOF.
2. **Drive state machine:**
   - `WAIT_REQ`/`READ_HEAD`: scan rd_buf for `\r\n\r\n` via `_ff_asm_http_find_eoh`. If found, parse request line + headers (`http_parse.c`), allocate `req` and `resp`, set `body_remaining` / `chunked` from headers, transition to `READ_BODY` (or `HANDLE` if no body).
   - `READ_BODY`: consume from rd_buf either `Content-Length` bytes or chunked frames, append to `req->body`. When complete, transition to `HANDLE`.
   - `HANDLE`: invoke registered handler. On return, serialize `resp` into `wr_buf` (status line + headers + body, with `Content-Length` if known or `Transfer-Encoding: chunked` if unknown). Transition to `WRITE_RESP`.
   - `WRITE_RESP`: drain wr_buf. When empty, free per-request scratch; if `keep_alive`, → `WAIT_REQ`; else → `CLOSING`.
   - `CLOSING`: drain wr_buf, then return 0.
3. **WRITE:** drain wr_buf to fd.
4. Compute next event mask + return.

### Parser — `src/libffproto/http/http_parse.{c,h}`

C-side responsibilities:
- `http_parse_request(const uint8_t *buf, size_t len, HttpRequest *req)`:
  validates request line, splits method/path/query/version, parses
  headers into a list (name + value pairs), returns body offset.
- `http_chunk_step(HttpConn *c, ...)`: incrementally decodes chunked
  bodies (state-machine: reads hex length, then chunk data, then CRLF,
  loop until zero-length terminator).
- `http_pct_decode(char *dst, size_t cap, const char *src)`: URI percent-decoding.
- `http_path_safe(const char *path)`: rejects `..` segments and absolute paths.

### Asm hot paths — `src/libffproto/http/http_parse.asm`

```asm
; ssize_t _ff_asm_http_find_eoh(const uint8_t *buf, uint64_t len);
;     returns offset of "\r\n\r\n" + 4 (i.e. end of header block), or -1.
;
; ssize_t _ff_asm_http_find_eol(const uint8_t *buf, uint64_t len);
;     returns offset of "\r\n" + 2 (start of next line), or -1.
;
; int _ff_asm_http_ascii_lower_eq(const uint8_t *a, const uint8_t *b, uint64_t n);
;     returns 1 if a and b match case-insensitively for ASCII letters, else 0.
;     Both inputs must be n bytes; uses 'or al, 0x20' for tolower.
```

Plain x86_64; no SIMD. ~75 lines total. Standard SysV AMD64 ABI.

### Response builder — `src/libffproto/http/http_resp.c`

`HttpResponse` holds:
- status code (default 200 if handler didn't set)
- list of headers (name + value)
- body buffer (`FrameBuf body`)
- a flag for "handler used streaming write_body" — determines chunked-vs-Content-Length

Serialization writes to the connection's wr_buf:
- Status line: `HTTP/1.1 <code> <reason>\r\n`
- Headers: `Name: value\r\n` each, auto-add `Server: ffnet/0.1`, `Date: <RFC 7231 IMF-fixdate>`, `Content-Length: N` (if body length known) or `Transfer-Encoding: chunked` + chunked-encoded body, `Connection: close` if client requested or server decided to close.
- Empty line: `\r\n`
- Body bytes.

For chunked output the body buffer holds the raw bytes; serialization wraps each segment with `hex_len\r\nDATA\r\n` and terminates with `0\r\n\r\n`. For slice 2 simplicity, the handler writes complete body into body buffer; we either compute the length up-front (Content-Length path) or chunk-encode it in a single chunk + terminator (chunked path). True streaming (handler emits chunks over time) is deferred — not needed for static files or echo.

### Built-in handlers

`http_static.c::ffhttp_static_handler`:
- Path resolution: `--root` + decoded URI path; reject `..` via
  `http_path_safe`; reject if resolved path doesn't begin with --root.
- `stat()` the path. 404 if missing, 403 if not a regular file
  (directory → 404 — no listings in slice 2).
- MIME via `http_mime_for_ext()`.
- Compute ETag = `"sha1(<size> <mtime_ns>)"` first 16 hex chars (uses
  libffutil's `ff_sha1`).
- Conditional handling:
  - `If-None-Match: <etag>` → 304 with no body if match.
  - `If-Modified-Since: <date>` → 304 if file unchanged.
- Range handling:
  - `Range: bytes=START-END` (or `START-` or `-SUFFIX`) → 206 with
    `Content-Range` header and partial body. Invalid → 416.
- Otherwise: 200 with full body, Content-Length, Last-Modified, ETag.
- HEAD: same headers, no body.

`http_echo.c::ffhttp_echo_handler`:
- Any method: read req body, write back as response body.
- `Content-Type: application/octet-stream`.
- Sets status 200.

`http_mime.c`: ~30 common extensions → MIME strings. Fallback
`application/octet-stream`.

## CLI

`cli/cmd_http_serve.c`:

```c
int cmd_http_serve(int argc, char **argv)
{
    /* parse --listen, --root, --workers, --help */
    if (!root) { usage(stderr); return 2; }
    ffhttp_set_handler(ffhttp_static_handler, (void *)root);
    return ffnet_server_run(&ffproto_http, host, port, workers);
}
```

`cli/cmd_http_echo.c`:

```c
int cmd_http_echo(int argc, char **argv)
{
    /* parse --listen, --workers, --help */
    ffhttp_set_handler(ffhttp_echo_handler, NULL);
    return ffnet_server_run(&ffproto_http, host, port, workers);
}
```

`cli/ffnet.c` registers both in `g_cmds[]`.

## Files

| File | Action | Responsibility |
|------|--------|----------------|
| `include/ffnet/ffhttp.h` | create | Public Http{Request,Response} accessors + HttpHandler + ffhttp_set_handler |
| `src/libffproto/http/http_module.c` | create | Protocol v3 entry points; per-conn state machine |
| `src/libffproto/http/http_parse.c` + `.h` | create | Request line + headers parser; chunked decoder; URI decode + safe-path |
| `src/libffproto/http/http_parse.asm` | create | find_eoh, find_eol, ascii_lower_eq |
| `src/libffproto/http/http_resp.c` + `.h` | create | HttpResponse impl + serializer |
| `src/libffproto/http/http_static.c` + `.h` | create | Built-in static handler |
| `src/libffproto/http/http_echo.c` + `.h` | create | Built-in echo handler |
| `src/libffproto/http/http_mime.c` + `.h` | create | Extension → MIME table |
| `src/libffproto/registry.c` | modify | Add `&ffproto_http` to static table |
| `cli/cmd_http_serve.c` | create | http-serve subcommand |
| `cli/cmd_http_echo.c` | create | http-echo subcommand |
| `cli/ffnet.c` | modify | Register both in g_cmds[] |
| `tests/test_http_parse.c` | create | Parser unit tests + asm helper tests |
| `tests/test_http_serve.py` | create | GET / HEAD / 404 / 405 / keep-alive / Range / If-Modified-Since |
| `tests/test_http_echo.py` | create | POST with Content-Length and chunked round-trips |
| `Makefile` | modify | New libffproto/http sources + test binaries |

## Verification

```sh
make clean && make                    # builds clean
make test                             # incl. test_http_parse
make itest                            # ws-echo still passes
python3 tests/test_http_serve.py build/ffnet
python3 tests/test_http_echo.py build/ffnet
make autobahn                         # ws-echo still 297/301 strict
```

Manual smoke:

```sh
build/ffnet http-serve --listen 127.0.0.1:8080 --root /tmp/data &
curl -i http://127.0.0.1:8080/index.html              # 200
curl -I http://127.0.0.1:8080/index.html              # HEAD
curl -i -H "Range: bytes=0-99" http://127.0.0.1:8080/index.html   # 206
curl -i http://127.0.0.1:8080/../etc/passwd           # 400
curl -X POST -d "x" http://127.0.0.1:8080/anything    # 405
build/ffnet http-echo --listen 127.0.0.1:8081 &
curl -X POST --data-binary @/etc/hostname http://127.0.0.1:8081/  # echo
curl -X POST -T - --header "Transfer-Encoding: chunked" http://127.0.0.1:8081/  # chunked
```

## Risks and open questions

1. **Scope creep.** "Full HTTP/1.1" has long tails. The non-goals list above
   is the hard cut. If I find a hidden requirement mid-implementation
   (e.g., specific header parsing behavior that affects a test), I'll
   either fix-as-needed or flag for a follow-up.

2. **Chunked decoder correctness.** Hex parsing + CRLF + CRLF inside a
   streaming decoder is the most error-prone piece. Unit tests cover
   partial-chunk-across-event-callback, zero-length terminator,
   trailer-headers (we ignore them), oversized chunk lengths.

3. **Handler ownership of HttpResponse.** The handler returns; the
   framework serializes. The handler must not retain `req` or `resp`
   pointers after returning. Documented in `ffhttp.h`.

4. **Date header.** RFC 7231 IMF-fixdate format requires a strftime
   format that's locale-sensitive on `glibc`. We'll either set the C
   locale to "C" in `http_resp.c` once, or hand-format the date (more
   reliable). Going with hand-formatted (~30 lines).

5. **`Server` header value.** Hard-coded as `Server: ffnet/0.1` for now;
   future slice can plumb a version constant.

6. **Single global handler.** `ffhttp_set_handler` is process-global. Both
   handlers (`http_static`, `http_echo`) work this way and the demo CLIs
   each call it once at startup before invoking `ffnet_server_run`. With
   multiple workers, all workers see the same handler (it's a static
   pointer + ctx). Documented.

7. **`HttpResponse` body buffering.** The handler builds the full body
   in memory before the framework serializes. For 100 MB downloads this
   is wasteful. True streaming responses are deferred to a future slice
   (would require extending the handler API to support callback-driven
   body production).

## Estimate

Large slice. ~15 new files, ~3000-4000 line diff. 3-5 focused sessions.
One bundled commit + push at the end per the project's cadence rule.
