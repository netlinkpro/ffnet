// SPDX-License-Identifier: LGPL-2.1-or-later
#include "http.h"

#include <string.h>

/* Tiny MIME table covering the extensions most commonly served by a
 * static file server. Anything else gets application/octet-stream. */

static const struct {
    const char *ext;
    const char *mime;
} g_mime[] = {
    { "html", "text/html; charset=utf-8" },
    { "htm",  "text/html; charset=utf-8" },
    { "css",  "text/css" },
    { "js",   "application/javascript" },
    { "json", "application/json" },
    { "xml",  "application/xml" },
    { "txt",  "text/plain; charset=utf-8" },
    { "md",   "text/markdown; charset=utf-8" },
    { "csv",  "text/csv" },
    { "svg",  "image/svg+xml" },
    { "png",  "image/png" },
    { "jpg",  "image/jpeg" },
    { "jpeg", "image/jpeg" },
    { "gif",  "image/gif" },
    { "webp", "image/webp" },
    { "ico",  "image/x-icon" },
    { "pdf",  "application/pdf" },
    { "zip",  "application/zip" },
    { "gz",   "application/gzip" },
    { "tar",  "application/x-tar" },
    { "mp3",  "audio/mpeg" },
    { "ogg",  "audio/ogg" },
    { "wav",  "audio/wav" },
    { "mp4",  "video/mp4" },
    { "webm", "video/webm" },
    { "woff", "font/woff" },
    { "woff2","font/woff2" },
    { "ttf",  "font/ttf" },
    { "otf",  "font/otf" },
    { "wasm", "application/wasm" },
};
static const size_t g_mime_count = sizeof g_mime / sizeof g_mime[0];

const char *http_mime_for_ext(const char *ext)
{
    if (!ext) return "application/octet-stream";
    for (size_t i = 0; i < g_mime_count; i++) {
        if (strcmp(g_mime[i].ext, ext) == 0) return g_mime[i].mime;
    }
    return "application/octet-stream";
}
