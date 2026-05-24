// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ffnet/ffutil.h"

#include <stdio.h>
#include <string.h>

static int test_enc(const char *name, const void *src, size_t n,
                    const char *expected)
{
    char out[128];
    int len = ff_base64_encode((const uint8_t *)src, n, out, sizeof out);
    if (len < 0) {
        fprintf(stderr, "FAIL: %s: encode returned %d\n", name, len);
        return 1;
    }
    if (strcmp(out, expected) != 0) {
        fprintf(stderr, "FAIL: %s\n  expected: %s\n  actual:   %s\n",
                name, expected, out);
        return 1;
    }
    if ((size_t)len != strlen(expected)) {
        fprintf(stderr, "FAIL: %s: len=%d, strlen=%zu\n",
                name, len, strlen(expected));
        return 1;
    }
    printf("ok: %s\n", name);
    return 0;
}

int main(void)
{
    int rc = 0;

    /* RFC 4648 §10 test vectors */
    rc |= test_enc("empty",   "",       0, "");
    rc |= test_enc("f",       "f",      1, "Zg==");
    rc |= test_enc("fo",      "fo",     2, "Zm8=");
    rc |= test_enc("foo",     "foo",    3, "Zm9v");
    rc |= test_enc("foob",    "foob",   4, "Zm9vYg==");
    rc |= test_enc("fooba",   "fooba",  5, "Zm9vYmE=");
    rc |= test_enc("foobar",  "foobar", 6, "Zm9vYmFy");

    /* RFC 6455 §1.3 accept-key: base64 of the documented SHA-1 digest. */
    const uint8_t sha1_out[20] = {
        0xb3, 0x7a, 0x4f, 0x2c, 0xc0, 0x62, 0x4f, 0x16,
        0x90, 0xf6, 0x46, 0x06, 0xcf, 0x38, 0x59, 0x45,
        0xb2, 0xbe, 0xc4, 0xea
    };
    rc |= test_enc("rfc6455-accept", sha1_out, 20,
        "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    /* Insufficient destination buffer must return FFE_NOMEM. */
    char tiny[2];
    int r = ff_base64_encode((const uint8_t *)"foo", 3, tiny, sizeof tiny);
    if (r != FFE_NOMEM) {
        fprintf(stderr, "FAIL: nomem-check: expected %d, got %d\n",
                FFE_NOMEM, r);
        rc = 1;
    } else {
        printf("ok: nomem-check\n");
    }

    return rc;
}
