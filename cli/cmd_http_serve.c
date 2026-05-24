// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ffnet/ffnet.h"
#include "ffnet/ffhttp.h"
#include "ffnet/ffproto.h"
#include "ffnet/ffutil.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out)
{
    fprintf(out, "ffnet http-serve — HTTP/1.1 static file server\n\n");
    fprintf(out, "usage:\n");
    fprintf(out, "  ffnet http-serve --listen <addr>:<port> --root <dir> [--workers N]\n\n");
    fprintf(out,
        "Serves files from <dir> over HTTP/1.1. Supports GET, HEAD, keep-alive,\n"
        "Range requests, conditional requests (If-Modified-Since, If-None-Match).\n"
        "Multi-worker via pthread + SO_REUSEPORT.\n\n");
    fprintf(out, "arguments:\n");
    fprintf(out,
        "  --listen <addr>:<port>   IPv4 dotted-quad and TCP port.\n"
        "  --root   <dir>           Directory to serve.\n"
        "  --workers N              pthread workers (default 1).\n\n");
    fprintf(out, "example:\n");
    fprintf(out, "  ffnet http-serve --listen 127.0.0.1:8080 --root /tmp/public --workers 4\n");
}

static int parse_listen(const char *spec, char *host_out, size_t host_cap,
                        uint16_t *port_out)
{
    const char *colon = strrchr(spec, ':');
    if (!colon) return FFE_INVAL;
    size_t hlen = (size_t)(colon - spec);
    if (hlen == 0 || hlen + 1 > host_cap) return FFE_INVAL;
    memcpy(host_out, spec, hlen);
    host_out[hlen] = '\0';
    char *end = NULL;
    unsigned long p = strtoul(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || p > 65535) return FFE_INVAL;
    *port_out = (uint16_t)p;
    return FFE_OK;
}

int cmd_http_serve(int argc, char **argv)
{
    const char *listen_spec = NULL;
    const char *root = NULL;
    int workers = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            listen_spec = argv[++i];
        } else if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            root = argv[++i];
        } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            workers = atoi(argv[++i]);
            if (workers < 1) { fprintf(stderr, "--workers must be >= 1\n"); return 2; }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout); return 0;
        } else {
            fprintf(stderr, "http-serve: unknown arg: %s\n\n", argv[i]);
            usage(stderr); return 2;
        }
    }
    if (!listen_spec || !root) { usage(stderr); return 2; }

    char     host[16];
    uint16_t port;
    if (parse_listen(listen_spec, host, sizeof host, &port) != FFE_OK) {
        fprintf(stderr, "http-serve: bad --listen spec: %s\n", listen_spec);
        return 2;
    }

    ffhttp_set_handler(ffhttp_static_handler, (void *)root);

    const Protocol *http = ffproto_find("http");
    if (!http) { FF_LOGE("http protocol not registered"); return 1; }

    int rc = ffnet_server_run(http, host, port, workers);
    if (rc != FFE_OK) {
        FF_LOGE("server_run: %s", ffutil_strerror_v(rc));
        return 1;
    }
    return 0;
}
