// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ffnet/ffutil.h"
#include "ffnet/ffhttp.h"
#include "../src/libffproto/http/http.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;

#define EXPECT_EQ(actual, expected, label) do {                     \
    long long _a = (long long)(actual), _e = (long long)(expected); \
    if (_a != _e) {                                                 \
        fprintf(stderr, "FAIL: %s: expected %lld, got %lld\n",      \
                (label), _e, _a);                                   \
        fails++;                                                    \
    } else { printf("ok: %s\n", (label)); }                         \
} while (0)

#define EXPECT_STREQ(actual, expected, label) do {                  \
    if (strcmp((actual), (expected)) != 0) {                        \
        fprintf(stderr, "FAIL: %s: expected '%s', got '%s'\n",      \
                (label), (expected), (actual));                     \
        fails++;                                                    \
    } else { printf("ok: %s\n", (label)); }                         \
} while (0)

static void test_find_eoh(void)
{
    const char *s = "GET / HTTP/1.1\r\nHost: x\r\n\r\nBODY";
    int64_t off = _ff_asm_http_find_eoh((const uint8_t *)s, strlen(s));
    EXPECT_EQ(off, (int64_t)27, "find_eoh basic");

    const char *no = "GET / HTTP/1.1\r\nHost: x\r\n";
    EXPECT_EQ(_ff_asm_http_find_eoh((const uint8_t *)no, strlen(no)),
              (int64_t)-1, "find_eoh missing");

    EXPECT_EQ(_ff_asm_http_find_eoh((const uint8_t *)"", 0),
              (int64_t)-1, "find_eoh empty");
}

static void test_find_eol(void)
{
    const char *s = "Host: x\r\nLine2";
    EXPECT_EQ(_ff_asm_http_find_eol((const uint8_t *)s, strlen(s)),
              (int64_t)9, "find_eol basic");

    const char *no = "no crlf here";
    EXPECT_EQ(_ff_asm_http_find_eol((const uint8_t *)no, strlen(no)),
              (int64_t)-1, "find_eol missing");
}

static void test_ascii_lower_eq(void)
{
    EXPECT_EQ(_ff_asm_http_ascii_lower_eq(
        (const uint8_t *)"Content-Length",
        (const uint8_t *)"content-length", 14), 1, "lower_eq case-fold");

    EXPECT_EQ(_ff_asm_http_ascii_lower_eq(
        (const uint8_t *)"GET", (const uint8_t *)"GET", 3), 1,
        "lower_eq identical");

    EXPECT_EQ(_ff_asm_http_ascii_lower_eq(
        (const uint8_t *)"FOO", (const uint8_t *)"BAR", 3), 0,
        "lower_eq mismatch");

    /* Non-letters compare strictly. */
    EXPECT_EQ(_ff_asm_http_ascii_lower_eq(
        (const uint8_t *)"123", (const uint8_t *)"124", 3), 0,
        "lower_eq digits differ");

    EXPECT_EQ(_ff_asm_http_ascii_lower_eq(
        (const uint8_t *)"", (const uint8_t *)"", 0), 1,
        "lower_eq empty");
}

static void test_pct_decode(void)
{
    char buf[64];
    int n = http_pct_decode(buf, sizeof buf, "/foo%20bar", 10);
    EXPECT_EQ(n, 8, "pct_decode len");
    buf[n] = 0;
    EXPECT_STREQ(buf, "/foo bar", "pct_decode value");

    n = http_pct_decode(buf, sizeof buf, "no-percent", 10);
    EXPECT_EQ(n, 10, "pct_decode no-op");

    n = http_pct_decode(buf, sizeof buf, "bad%", 4);
    EXPECT_EQ(n, FFE_PROTOCOL, "pct_decode truncated");
}

static void test_path_safe(void)
{
    EXPECT_EQ(http_path_safe("/foo/bar"),      1, "path_safe normal");
    EXPECT_EQ(http_path_safe("/foo/../bar"),   0, "path_safe traversal");
    EXPECT_EQ(http_path_safe("/.."),           0, "path_safe trailing ..");
    EXPECT_EQ(http_path_safe("/../etc"),       0, "path_safe leading ..");
    EXPECT_EQ(http_path_safe("foo"),           0, "path_safe non-absolute");
    EXPECT_EQ(http_path_safe(NULL),            0, "path_safe NULL");
}

static void test_request_parser(void)
{
    const char *raw =
        "GET /foo?a=1 HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 4\r\n"
        "\r\n";
    size_t eoh = (size_t)_ff_asm_http_find_eoh((const uint8_t *)raw, strlen(raw));

    HttpRequest *r = http_request_new();
    int rc = http_parse_request((const uint8_t *)raw, eoh, r);
    EXPECT_EQ(rc, FFE_OK, "parse_request OK");

    EXPECT_STREQ(r->method,  "GET",      "parse_request method");
    EXPECT_STREQ(r->path,    "/foo",     "parse_request path");
    EXPECT_STREQ(r->query,   "a=1",      "parse_request query");
    EXPECT_STREQ(r->version, "HTTP/1.1", "parse_request version");

    const char *host = ffhttp_req_header(r, "Host");
    EXPECT_STREQ(host ? host : "NULL", "example.com", "header Host");

    const char *cl = ffhttp_req_header(r, "content-LENGTH");  /* case-insensitive */
    EXPECT_STREQ(cl ? cl : "NULL", "4", "header Content-Length (case-insensitive)");

    http_request_free(r);
}

static void test_chunk_decode(void)
{
    /* "5\r\nhello\r\n0\r\n\r\n" → body = "hello" */
    const char *raw = "5\r\nhello\r\n0\r\n\r\n";
    int    state = 0;
    size_t rem   = 0;
    size_t consumed = 0;
    FrameBuf out; framebuf_init(&out, 0);

    int rc = http_chunk_decode((const uint8_t *)raw, strlen(raw),
                               &consumed, &state, &rem, &out);
    EXPECT_EQ(rc, FFE_OK, "chunk_decode complete");
    EXPECT_EQ(out.len, 5, "chunk_decode body len");
    if (out.len == 5) {
        out.data[5] = 0;
        EXPECT_STREQ((char *)out.data, "hello", "chunk_decode body value");
    }
    framebuf_free(&out);

    /* Multi-chunk: "3\r\nfoo\r\n3\r\nbar\r\n0\r\n\r\n" */
    const char *raw2 = "3\r\nfoo\r\n3\r\nbar\r\n0\r\n\r\n";
    state = 0; rem = 0; consumed = 0;
    framebuf_init(&out, 0);
    rc = http_chunk_decode((const uint8_t *)raw2, strlen(raw2),
                           &consumed, &state, &rem, &out);
    EXPECT_EQ(rc, FFE_OK, "chunk_decode multi");
    EXPECT_EQ(out.len, 6, "chunk_decode multi len");
    if (out.len == 6) {
        out.data[6] = 0;
        EXPECT_STREQ((char *)out.data, "foobar", "chunk_decode multi value");
    }
    framebuf_free(&out);
}

int main(void)
{
    test_find_eoh();
    test_find_eol();
    test_ascii_lower_eq();
    test_pct_decode();
    test_path_safe();
    test_request_parser();
    test_chunk_decode();
    return fails == 0 ? 0 : 1;
}
