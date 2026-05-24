# Slice 2 — Full HTTP/1.1 Server Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (inline). Steps use checkbox (`- [ ]`) syntax. One bundled commit at the end per `feedback-commit-cadence` memory.

**Goal:** Add `ffproto_http` (full HTTP/1.1 server) to the protocol registry. Two CLI subcommands (`http-serve` static files, `http-echo` POST echo). Asm hot paths for parser scanning.

**Spec:** [docs/specs/2026-05-24-slice-2-http1-design.md](../specs/2026-05-24-slice-2-http1-design.md).

---

## Implementation order

Bottom-up — each task produces compilable code, but the full slice only links when CLI is wired in (Task 11).

1. `include/ffnet/ffhttp.h` — public types + handler API.
2. `src/libffproto/http/http.h` — internal types (HttpConn, list of headers, etc.).
3. `src/libffproto/http/http_parse.asm` — find_eoh, find_eol, ascii_lower_eq.
4. `src/libffproto/http/http_parse.c` + `.h` — request parser, chunked decoder, URI helpers.
5. `src/libffproto/http/http_mime.c` + `.h` — extension → MIME table.
6. `src/libffproto/http/http_resp.c` + `.h` — HttpResponse impl + serializer + Date formatter.
7. `src/libffproto/http/http_module.c` — Protocol v3 state machine + ffproto_http struct + ffhttp_set_handler.
8. `src/libffproto/http/http_static.c` + `.h` — static file handler.
9. `src/libffproto/http/http_echo.c` + `.h` — echo handler.
10. `src/libffproto/registry.c` — add &ffproto_http to static table.
11. `cli/cmd_http_serve.c`, `cli/cmd_http_echo.c`, `cli/ffnet.c` — wire CLI.
12. `tests/test_http_parse.c` — parser + asm helpers.
13. `tests/test_http_serve.py`, `tests/test_http_echo.py` — integration.
14. `Makefile` — add new objects, test binaries.
15. Sync + verify on shaiman.
16. Bundled commit + push + memory update.

---

## Key implementation notes

### Asm: `src/libffproto/http/http_parse.asm`

Plain x86_64; no SIMD. All routines pure computation, no syscalls.

```asm
; ssize_t _ff_asm_http_find_eoh(const uint8_t *buf, uint64_t len)
;   Scans for "\r\n\r\n" sequence. Returns the byte offset AFTER the match
;   (i.e. start of body), or -1 if not found.
;   rdi = buf, rsi = len; returns rax.
;
; ssize_t _ff_asm_http_find_eol(const uint8_t *buf, uint64_t len)
;   Same but for "\r\n". Returns offset after match or -1.
;
; int _ff_asm_http_ascii_lower_eq(const uint8_t *a, const uint8_t *b, uint64_t n)
;   Case-insensitive ASCII compare. Letters A-Z and a-z compare equal;
;   non-letters compare strictly. Returns 1 if all n bytes match, else 0.
```

### Parser state machine (chunked decoder)

Chunked body format:
```
<hex-len>[;chunk-ext]\r\n
<data>\r\n
<hex-len>\r\n
<data>\r\n
...
0\r\n
[trailer headers]\r\n
\r\n
```

State machine in HttpConn: `chunk_state ∈ {SIZE_LINE, DATA, AFTER_DATA_CR, AFTER_DATA_LF, TRAILER, DONE}`.

Slice 2 simplification: ignore chunk-ext (skip to CRLF); ignore trailer headers (skip to bare CRLF).

### Date header

Hand-formatted IMF-fixdate (avoids locale dependence):

```
Sun, 06 Nov 1994 08:49:37 GMT
```

```c
static void format_imf_date(char out[30], time_t t) {
    static const char *days[]   = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                    "Jul","Aug","Sep","Oct","Nov","Dec"};
    struct tm tm; gmtime_r(&t, &tm);
    snprintf(out, 30, "%s, %02d %s %04d %02d:%02d:%02d GMT",
        days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon],
        tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
}
```

### Static handler ETag

```c
/* ETag = first 16 hex chars of SHA1("<size>:<mtime_sec>:<mtime_nsec>"). */
char etag_buf[40]; /* "" + 16 chars + " */
```

### Verification

```sh
ssh -i ~/.ssh/id_cwpi fooldev@shaiman 'cd ffnet && make clean && make && make test && make itest && python3 tests/test_http_serve.py build/ffnet && python3 tests/test_http_echo.py build/ffnet && make autobahn 2>&1 | tail -20'
```

Expected:
- Clean build (-lpthread linked).
- All existing tests pass + new test_http_parse passes.
- ws-echo itest passes (no regression).
- http-serve test passes (GET, HEAD, 404, 405, keep-alive, Range, If-Modified-Since).
- http-echo test passes (Content-Length and chunked POSTs).
- Autobahn unchanged at 297/301 strict (we touched nothing under WS).

---

## Single bundled commit

After verification, one commit containing: spec + plan + all new sources + test files + Makefile + memory update. Push.
