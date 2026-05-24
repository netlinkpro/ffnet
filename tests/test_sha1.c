// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ffnet/ffutil.h"

#include <stdio.h>
#include <string.h>

static int hex_eq(const uint8_t digest[20], const char *expected_hex)
{
    char actual[41];
    for (int i = 0; i < 20; i++) snprintf(actual + 2 * i, 3, "%02x", digest[i]);
    actual[40] = '\0';
    if (strcmp(actual, expected_hex) == 0) return 1;
    fprintf(stderr, "  expected: %s\n  actual:   %s\n", expected_hex, actual);
    return 0;
}

static int test_vec(const char *name, const void *data, size_t n,
                    const char *expected)
{
    uint8_t out[20];
    ff_sha1((const uint8_t *)data, (uint64_t)n, out);
    if (!hex_eq(out, expected)) {
        fprintf(stderr, "FAIL: %s\n", name);
        return 1;
    }
    printf("ok: %s\n", name);
    return 0;
}

int main(void)
{
    int rc = 0;

    /* RFC 3174 test vectors */
    rc |= test_vec("empty", "", 0,
        "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    rc |= test_vec("abc", "abc", 3,
        "a9993e364706816aba3e25717850c26c9cd0d89c");
    rc |= test_vec("rfc3174-tv2",
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
        "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

    /* RFC 6455 §1.3 — the handshake string the WS server hashes.
     * Also a 60-byte input → exercises the two-block padding path. */
    rc |= test_vec("rfc6455-handshake-input",
        "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 60,
        "b37a4f2cc0624f1690f64606cf385945b2bec4ea");

    return rc;
}
