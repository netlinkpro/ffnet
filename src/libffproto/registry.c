// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ffnet/ffproto.h"
#include "ffnet/ffutil.h"

#include <stdlib.h>
#include <string.h>

extern const Protocol ffproto_http;

/* Compiled-in protocols. Add new entries here when new modules ship. */
static const Protocol *const g_static[] = {
    &ffproto_websocket,
    &ffproto_http,
};
static const size_t g_static_count = sizeof(g_static) / sizeof(g_static[0]);

/* Runtime-registered protocols. Grown geometrically.
 * NOTE: not thread-safe; slice 1 is single-threaded. */
static const Protocol **g_dynamic = NULL;
static size_t g_dyn_len = 0;
static size_t g_dyn_cap = 0;

const Protocol *ffproto_find(const char *name)
{
    if (!name) return NULL;

    for (size_t i = 0; i < g_static_count; i++) {
        if (strcmp(g_static[i]->name, name) == 0) return g_static[i];
    }
    for (size_t i = 0; i < g_dyn_len; i++) {
        if (strcmp(g_dynamic[i]->name, name) == 0) return g_dynamic[i];
    }
    return NULL;
}

int ffproto_register(const Protocol *proto)
{
    if (!proto || !proto->name)              return FFE_INVAL;
    if (ffproto_find(proto->name) != NULL)   return FFE_INVAL;

    if (g_dyn_len == g_dyn_cap) {
        size_t new_cap = g_dyn_cap ? g_dyn_cap * 2 : 4;
        const Protocol **np = (const Protocol **)realloc(
            (void *)g_dynamic, new_cap * sizeof(*g_dynamic));
        if (!np) return FFE_NOMEM;
        g_dynamic = np;
        g_dyn_cap = new_cap;
    }
    g_dynamic[g_dyn_len++] = proto;
    return FFE_OK;
}
