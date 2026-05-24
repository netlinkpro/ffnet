// SPDX-License-Identifier: LGPL-2.1-or-later
#include "http.h"
#include "ffnet/ffhttp.h"
#include "ffnet/ffutil.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ---------------------------------------------------------------------------
 * Helpers.
 * --------------------------------------------------------------------------*/

static const char *path_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot || dot == path) return "";
    /* Make sure the dot isn't inside an earlier path component. */
    if (strchr(dot, '/') != NULL) return "";
    return dot + 1;
}

/* Compute ETag = first 16 hex chars of SHA-1("<size>:<mtime_sec>:<mtime_ns>")
 * Wrapped in double quotes, NUL-terminated. Buffer must be ≥ 20 chars. */
static void compute_etag(const struct stat *st, char out[20])
{
    char meta[64];
    int n = snprintf(meta, sizeof meta, "%lld:%lld:%lld",
                     (long long)st->st_size,
                     (long long)st->st_mtim.tv_sec,
                     (long long)st->st_mtim.tv_nsec);
    uint8_t digest[20];
    ff_sha1((const uint8_t *)meta, (uint64_t)n, digest);

    out[0] = '"';
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        out[1 + 2 * i]     = hex[digest[i] >> 4];
        out[1 + 2 * i + 1] = hex[digest[i] & 0xF];
    }
    out[17] = '"';
    out[18] = '\0';
}

/* Format Last-Modified header value (IMF-fixdate). */
static void format_imf(char out[40], time_t t)
{
    static const char *days[]   = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                    "Jul","Aug","Sep","Oct","Nov","Dec"};
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(out, 40, "%s, %02d %s %04d %02d:%02d:%02d GMT",
             days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon],
             tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* Parse IMF-fixdate "Sun, 06 Nov 1994 08:49:37 GMT" into time_t (UTC).
 * Returns 0 on success, -1 on parse failure or non-canonical format. */
static int parse_imf(const char *s, time_t *out)
{
    if (!s || strlen(s) < 29) return -1;
    /* skip "Day, " */
    if (s[3] != ',' || s[4] != ' ') return -1;
    s += 5;

    int day = (s[0] - '0') * 10 + (s[1] - '0');
    if (s[2] != ' ') return -1;
    s += 3;

    static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                    "Jul","Aug","Sep","Oct","Nov","Dec"};
    int mon = -1;
    for (int i = 0; i < 12; i++) {
        if (memcmp(s, months[i], 3) == 0) { mon = i; break; }
    }
    if (mon < 0) return -1;
    s += 4;

    int year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 +
               (s[2] - '0') * 10 + (s[3] - '0');
    s += 5;

    int hh = (s[0] - '0') * 10 + (s[1] - '0');
    int mm = (s[3] - '0') * 10 + (s[4] - '0');
    int ss = (s[6] - '0') * 10 + (s[7] - '0');

    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = year - 1900;
    tm.tm_mon  = mon;
    tm.tm_mday = day;
    tm.tm_hour = hh;
    tm.tm_min  = mm;
    tm.tm_sec  = ss;
    *out = timegm(&tm);
    return 0;
}

/* Parse Range header value "bytes=START-END" / "bytes=START-" / "bytes=-SUFFIX".
 * Returns 0 if Range parsed and writes *start, *end (inclusive); -1 if invalid. */
static int parse_range(const char *r, off_t file_size, off_t *start, off_t *end)
{
    if (memcmp(r, "bytes=", 6) != 0) return -1;
    r += 6;
    /* Reject multi-range. */
    if (strchr(r, ',')) return -1;

    const char *dash = strchr(r, '-');
    if (!dash) return -1;

    if (dash == r) {
        /* "-N" : last N bytes. */
        char *e;
        long long n = strtoll(dash + 1, &e, 10);
        if (e == dash + 1 || *e != '\0' || n <= 0) return -1;
        if (n > file_size) n = file_size;
        *start = file_size - n;
        *end   = file_size - 1;
        return 0;
    }
    char *e;
    long long s = strtoll(r, &e, 10);
    if (e != dash) return -1;
    if (*(dash + 1) == '\0') {
        /* "N-" : from N to end. */
        if (s >= file_size) return -1;
        *start = s;
        *end   = file_size - 1;
        return 0;
    }
    long long ev = strtoll(dash + 1, &e, 10);
    if (*e != '\0') return -1;
    if (s < 0 || ev < s || s >= file_size) return -1;
    if (ev >= file_size) ev = file_size - 1;
    *start = s;
    *end   = ev;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Handler.
 * --------------------------------------------------------------------------*/

int ffhttp_static_handler(const HttpRequest *req, HttpResponse *resp, void *user)
{
    const char *root = (const char *)user;
    if (!root) { ffhttp_resp_status(resp, 500); return 0; }

    const char *method = ffhttp_req_method(req);
    int is_head = strcmp(method, "HEAD") == 0;
    if (!is_head && strcmp(method, "GET") != 0) {
        ffhttp_resp_status(resp, 405);
        ffhttp_resp_header(resp, "Allow", "GET, HEAD");
        return 0;
    }

    /* Build full path. */
    const char *rel = ffhttp_req_path(req);
    while (*rel == '/') rel++;             /* don't double-slash */
    char full[PATH_MAX];
    int  n = snprintf(full, sizeof full, "%s/%s", root, rel);
    if (n < 0 || (size_t)n >= sizeof full) {
        ffhttp_resp_status(resp, 414);
        return 0;
    }

    struct stat st;
    if (stat(full, &st) != 0) {
        ffhttp_resp_status(resp, 404);
        return 0;
    }
    if (!S_ISREG(st.st_mode)) {
        /* Directory (no listings) or other non-regular file. */
        ffhttp_resp_status(resp, 404);
        return 0;
    }

    /* ETag + Last-Modified. */
    char etag[20];   compute_etag(&st, etag);
    char lmod[40];   format_imf(lmod, st.st_mtim.tv_sec);

    /* Conditional handling. */
    const char *inm = ffhttp_req_header(req, "If-None-Match");
    if (inm && strcmp(inm, etag) == 0) {
        ffhttp_resp_status(resp, 304);
        ffhttp_resp_header(resp, "ETag", etag);
        ffhttp_resp_header(resp, "Last-Modified", lmod);
        return 0;
    }
    const char *ims = ffhttp_req_header(req, "If-Modified-Since");
    if (ims) {
        time_t ims_t;
        if (parse_imf(ims, &ims_t) == 0 && st.st_mtim.tv_sec <= ims_t) {
            ffhttp_resp_status(resp, 304);
            ffhttp_resp_header(resp, "ETag", etag);
            ffhttp_resp_header(resp, "Last-Modified", lmod);
            return 0;
        }
    }

    /* Range handling. */
    off_t range_start = 0;
    off_t range_end   = st.st_size - 1;
    int    is_range   = 0;
    const char *rh = ffhttp_req_header(req, "Range");
    if (rh) {
        if (parse_range(rh, st.st_size, &range_start, &range_end) != 0) {
            ffhttp_resp_status(resp, 416);
            char crh[64];
            snprintf(crh, sizeof crh, "bytes */%lld", (long long)st.st_size);
            ffhttp_resp_header(resp, "Content-Range", crh);
            return 0;
        }
        is_range = 1;
    }

    /* Common headers. */
    ffhttp_resp_header(resp, "Content-Type",
                       http_mime_for_ext(path_ext(rel)));
    ffhttp_resp_header(resp, "ETag", etag);
    ffhttp_resp_header(resp, "Last-Modified", lmod);
    ffhttp_resp_header(resp, "Accept-Ranges", "bytes");

    size_t content_len = (size_t)(range_end - range_start + 1);

    if (is_range) {
        ffhttp_resp_status(resp, 206);
        char crh[64];
        snprintf(crh, sizeof crh, "bytes %lld-%lld/%lld",
                 (long long)range_start, (long long)range_end,
                 (long long)st.st_size);
        ffhttp_resp_header(resp, "Content-Range", crh);
    } else {
        ffhttp_resp_status(resp, 200);
    }

    /* For HEAD, set Content-Length explicitly (we return before reading
     * the body, so the serializer's auto-Content-Length would be 0). */
    if (is_head) {
        char cl[32];
        snprintf(cl, sizeof cl, "%zu", content_len);
        ffhttp_resp_header(resp, "Content-Length", cl);
        return 0;
    }

    /* Read the file (full or range) into the response body. */
    int fd = open(full, O_RDONLY);
    if (fd < 0) {
        ffhttp_resp_status(resp, 500);
        return 0;
    }
    if (range_start > 0) {
        if (lseek(fd, range_start, SEEK_SET) < 0) {
            close(fd);
            ffhttp_resp_status(resp, 500);
            return 0;
        }
    }
    size_t remaining = (size_t)(range_end - range_start + 1);
    uint8_t buf[16384];
    while (remaining > 0) {
        size_t want = remaining < sizeof buf ? remaining : sizeof buf;
        ssize_t got = read(fd, buf, want);
        if (got <= 0) break;
        ffhttp_resp_body(resp, buf, (size_t)got);
        remaining -= (size_t)got;
    }
    close(fd);
    return 0;
}
