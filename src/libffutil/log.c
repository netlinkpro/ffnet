// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ffnet/ffutil.h"

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
    default:           return "FFE_UNKNOWN";
    }
}
