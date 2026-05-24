// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ffnet/ffutil.h"
#include "../src/libffproto/ws/utf8.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;

#define EXPECT_EQ(actual, expected, label) do {                       \
    if ((actual) != (expected)) {                                     \
        fprintf(stderr, "FAIL: %s: expected %d, got %d\n",            \
                (label), (int)(expected), (int)(actual));             \
        fails++;                                                      \
    } else {                                                          \
        printf("ok: %s\n", (label));                                  \
    }                                                                 \
} while (0)

#define EXPECT_STREQ(actual, expected, label) do {                    \
    if (strcmp((actual), (expected)) != 0) {                          \
        fprintf(stderr, "FAIL: %s: expected '%s', got '%s'\n",        \
                (label), (expected), (actual));                       \
        fails++;                                                      \
    } else {                                                          \
        printf("ok: %s\n", (label));                                  \
    }                                                                 \
} while (0)

static void test_map_errno(void)
{
    errno = 0;
    EXPECT_EQ(ffutil_map_errno(-EAGAIN),       FFE_AGAIN,    "map_errno EAGAIN");
    EXPECT_EQ(errno,                            EAGAIN,       "map_errno sets errno=EAGAIN");

    EXPECT_EQ(ffutil_map_errno(-ECONNREFUSED), FFE_REFUSED,  "map_errno ECONNREFUSED");
    EXPECT_EQ(errno,                            ECONNREFUSED, "map_errno sets errno=ECONNREFUSED");

    EXPECT_EQ(ffutil_map_errno(-ECONNRESET),   FFE_RESET,    "map_errno ECONNRESET");
    EXPECT_EQ(ffutil_map_errno(-EPIPE),        FFE_PIPE,     "map_errno EPIPE");
    EXPECT_EQ(ffutil_map_errno(-ETIMEDOUT),    FFE_TIMEOUT,  "map_errno ETIMEDOUT");
    EXPECT_EQ(ffutil_map_errno(-EACCES),       FFE_PERM,     "map_errno EACCES");
    EXPECT_EQ(ffutil_map_errno(-EPERM),        FFE_PERM,     "map_errno EPERM");
    EXPECT_EQ(ffutil_map_errno(-EINTR),        FFE_INTR,     "map_errno EINTR");

    /* Unknown errno falls back to FFE_IO; errno still set. */
    EXPECT_EQ(ffutil_map_errno(-EBADF),        FFE_IO,       "map_errno EBADF -> FFE_IO");
    EXPECT_EQ(errno,                            EBADF,        "map_errno sets errno=EBADF");
}

static void test_strerror_extended(void)
{
    EXPECT_STREQ(ffutil_strerror(FFE_AGAIN),   "FFE_AGAIN",   "strerror FFE_AGAIN");
    EXPECT_STREQ(ffutil_strerror(FFE_REFUSED), "FFE_REFUSED", "strerror FFE_REFUSED");
    EXPECT_STREQ(ffutil_strerror(FFE_RESET),   "FFE_RESET",   "strerror FFE_RESET");
    EXPECT_STREQ(ffutil_strerror(FFE_PIPE),    "FFE_PIPE",    "strerror FFE_PIPE");
    EXPECT_STREQ(ffutil_strerror(FFE_TIMEOUT), "FFE_TIMEOUT", "strerror FFE_TIMEOUT");
    EXPECT_STREQ(ffutil_strerror(FFE_PERM),    "FFE_PERM",    "strerror FFE_PERM");
    EXPECT_STREQ(ffutil_strerror(FFE_INTR),    "FFE_INTR",    "strerror FFE_INTR");
    EXPECT_STREQ(ffutil_strerror(-999),        "FFE_UNKNOWN", "strerror unknown");
}

static void test_strerror_v(void)
{
    /* Non-IO code: no errno suffix even if errno is set to noise. */
    errno = EBADF;
    EXPECT_STREQ(ffutil_strerror_v(FFE_BADKEY), "FFE_BADKEY",
                 "strerror_v non-IO ignores errno");

    /* IO code: includes strerror suffix. We don't pin the exact strerror
     * text (locale-dependent), just assert the prefix and that the suffix
     * is parenthesized. */
    errno = ECONNREFUSED;
    const char *s = ffutil_strerror_v(FFE_REFUSED);
    if (strncmp(s, "FFE_REFUSED (", 13) != 0 || s[strlen(s) - 1] != ')') {
        fprintf(stderr, "FAIL: strerror_v FFE_REFUSED: got '%s'\n", s);
        fails++;
    } else {
        printf("ok: strerror_v FFE_REFUSED has '(...)' suffix\n");
    }

    /* FFE_IO itself: also gets the suffix. */
    errno = EBADF;
    s = ffutil_strerror_v(FFE_IO);
    if (strncmp(s, "FFE_IO (", 8) != 0) {
        fprintf(stderr, "FAIL: strerror_v FFE_IO: got '%s'\n", s);
        fails++;
    } else {
        printf("ok: strerror_v FFE_IO has '(...)' suffix\n");
    }
}

static uint32_t feed(uint32_t state, const char *bytes, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        state = ff_utf8_decode(state, (uint8_t)bytes[i]);
        if (state == FF_UTF8_REJECT) return state;
    }
    return state;
}

static void test_utf8(void)
{
    /* Valid ASCII. */
    EXPECT_EQ(feed(FF_UTF8_ACCEPT, "hello", 5), (uint32_t)FF_UTF8_ACCEPT,
              "utf8 ASCII");

    /* Valid 2-byte (é = 0xC3 0xA9). */
    EXPECT_EQ(feed(FF_UTF8_ACCEPT, "\xC3\xA9", 2), (uint32_t)FF_UTF8_ACCEPT,
              "utf8 2-byte é");

    /* Valid 3-byte (€ = 0xE2 0x82 0xAC). */
    EXPECT_EQ(feed(FF_UTF8_ACCEPT, "\xE2\x82\xAC", 3), (uint32_t)FF_UTF8_ACCEPT,
              "utf8 3-byte €");

    /* Valid 4-byte (𐍈 = 0xF0 0x90 0x8D 0x88). */
    EXPECT_EQ(feed(FF_UTF8_ACCEPT, "\xF0\x90\x8D\x88", 4),
              (uint32_t)FF_UTF8_ACCEPT, "utf8 4-byte 𐍈");

    /* Standalone continuation byte. */
    EXPECT_EQ(feed(FF_UTF8_ACCEPT, "\x80", 1), (uint32_t)FF_UTF8_REJECT,
              "utf8 standalone 0x80 -> REJECT");

    /* Overlong / illegal lead 0xC0. */
    EXPECT_EQ(feed(FF_UTF8_ACCEPT, "\xC0\x80", 2), (uint32_t)FF_UTF8_REJECT,
              "utf8 overlong 0xC0 0x80 -> REJECT");

    /* Surrogate U+D800 is illegal in UTF-8. */
    EXPECT_EQ(feed(FF_UTF8_ACCEPT, "\xED\xA0\x80", 3), (uint32_t)FF_UTF8_REJECT,
              "utf8 surrogate U+D800 -> REJECT");

    /* Truncated sequence: state stays mid-sequence (not ACCEPT, not REJECT). */
    uint32_t s = ff_utf8_decode(FF_UTF8_ACCEPT, 0xE2);  /* expect 2 more bytes */
    if (s == FF_UTF8_ACCEPT || s == FF_UTF8_REJECT) {
        fprintf(stderr, "FAIL: utf8 truncated: state=%u (expected mid-seq)\n", s);
        fails++;
    } else {
        printf("ok: utf8 truncated stays mid-sequence\n");
    }

    /* Fragmented validation: feed valid sequence one byte per call. */
    s = FF_UTF8_ACCEPT;
    s = ff_utf8_decode(s, 0xE2);
    s = ff_utf8_decode(s, 0x82);
    s = ff_utf8_decode(s, 0xAC);
    EXPECT_EQ(s, (uint32_t)FF_UTF8_ACCEPT,
              "utf8 fragmented byte-by-byte -> ACCEPT");
}

int main(void)
{
    test_map_errno();
    test_strerror_extended();
    test_strerror_v();
    test_utf8();
    return fails == 0 ? 0 : 1;
}
