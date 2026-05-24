// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ffnet/ffutil.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int framebuf_init(FrameBuf *b, size_t initial_cap)
{
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
    if (initial_cap == 0) return FFE_OK;
    b->data = (uint8_t *)malloc(initial_cap);
    if (!b->data) return FFE_NOMEM;
    b->cap = initial_cap;
    return FFE_OK;
}

int framebuf_reserve(FrameBuf *b, size_t need)
{
    if (b->cap - b->len >= need) return FFE_OK;

    size_t want = b->len + need;
    if (want < b->len) return FFE_NOMEM;        /* overflow */

    size_t cap = b->cap ? b->cap : 64;
    while (cap < want) {
        if (cap > (SIZE_MAX / 2)) return FFE_NOMEM;
        cap *= 2;
    }

    uint8_t *p = (uint8_t *)realloc(b->data, cap);
    if (!p) return FFE_NOMEM;
    b->data = p;
    b->cap  = cap;
    return FFE_OK;
}

int framebuf_append(FrameBuf *b, const void *src, size_t n)
{
    int rc = framebuf_reserve(b, n);
    if (rc != FFE_OK) return rc;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return FFE_OK;
}

void framebuf_reset(FrameBuf *b)
{
    b->len = 0;
}

void framebuf_free(FrameBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}
