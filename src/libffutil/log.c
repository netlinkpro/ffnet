// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ffnet/ffutil.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_log_level = -1;   /* lazy init from FFNET_LOG_LEVEL */

static int parse_level(const char *s)
{
    if (!s || !*s)            return FF_LOG_WARN;
    if (!strcmp(s, "error"))  return FF_LOG_ERROR;
    if (!strcmp(s, "warn"))   return FF_LOG_WARN;
    if (!strcmp(s, "info"))   return FF_LOG_INFO;
    if (!strcmp(s, "debug"))  return FF_LOG_DEBUG;
    return FF_LOG_WARN;
}

int ff_log_enabled(int level)
{
    if (g_log_level < 0) g_log_level = parse_level(getenv("FFNET_LOG_LEVEL"));
    return level <= g_log_level;
}

void ff_log_emit(int level, const char *file, int line, const char *fmt, ...)
{
    static const char tag[] = { '?', 'E', 'W', 'I', 'D' };
    char ch = (level >= 1 && level <= 4) ? tag[level] : '?';

    fprintf(stderr, "[ffnet %c %s:%d] ", ch, file, line);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}

int ffutil_map_errno(int negative_errno)
{
    int e = -negative_errno;
    errno = e;
    switch (e) {
    case EAGAIN:        return FFE_AGAIN;
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:   return FFE_AGAIN;
#endif
    case ECONNREFUSED:  return FFE_REFUSED;
    case ECONNRESET:    return FFE_RESET;
    case EPIPE:         return FFE_PIPE;
    case ETIMEDOUT:     return FFE_TIMEOUT;
    case EACCES:
    case EPERM:         return FFE_PERM;
    case EINTR:         return FFE_INTR;
    default:            return FFE_IO;
    }
}

const char *ffutil_strerror(int code)
{
    switch (code) {
    case FFE_OK:       return "FFE_OK";
    case FFE_IO:       return "FFE_IO";
    case FFE_PROTOCOL: return "FFE_PROTOCOL";
    case FFE_BADKEY:   return "FFE_BADKEY";
    case FFE_NOMEM:    return "FFE_NOMEM";
    case FFE_CLOSED:   return "FFE_CLOSED";
    case FFE_INVAL:    return "FFE_INVAL";
    case FFE_AGAIN:    return "FFE_AGAIN";
    case FFE_REFUSED:  return "FFE_REFUSED";
    case FFE_RESET:    return "FFE_RESET";
    case FFE_PIPE:     return "FFE_PIPE";
    case FFE_TIMEOUT:  return "FFE_TIMEOUT";
    case FFE_PERM:     return "FFE_PERM";
    case FFE_INTR:     return "FFE_INTR";
    default:           return "FFE_UNKNOWN";
    }
}

const char *ffutil_strerror_v(int rc)
{
    static char buf[128];

    /* I/O-class range: FFE_IO (-1) and FFE_AGAIN..FFE_INTR (-7..-13). */
    int is_io = (rc == FFE_IO) || (rc <= FFE_AGAIN && rc >= FFE_INTR);

    const char *name = ffutil_strerror(rc);
    if (!is_io) return name;

    snprintf(buf, sizeof buf, "%s (%s)", name, strerror(errno));
    return buf;
}
