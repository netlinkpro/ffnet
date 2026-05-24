// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ffnet/ffnet.h"
#include "ffnet/ffproto.h"
#include "ffnet/ffutil.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out)
{
    fprintf(out, "ffnet ws-echo — WebSocket echo server\n\n");
    fprintf(out, "usage:\n");
    fprintf(out, "  ffnet ws-echo --listen <addr>:<port> [--workers N]\n\n");
    fprintf(out,
        "Echoes each text/binary frame received from a client back to that\n"
        "client with the same opcode. Control frames (ping, close) are\n"
        "handled inline per RFC 6455. Multi-worker mode uses SO_REUSEPORT\n"
        "to distribute incoming connections across N pthread workers.\n\n");
    fprintf(out, "arguments:\n");
    fprintf(out,
        "  --listen <addr>:<port>   IPv4 dotted-quad and TCP port. Use port 0\n"
        "                           to request an ephemeral port; the actual\n"
        "                           bound port is printed on stdout when ready.\n"
        "  --workers N              Number of pthread workers (default 1).\n\n");
    fprintf(out, "examples:\n");
    fprintf(out, "  ffnet ws-echo --listen 127.0.0.1:8080\n");
    fprintf(out, "  ffnet ws-echo --listen 0.0.0.0:8080 --workers 4\n");
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

int cmd_ws_echo(int argc, char **argv)
{
    const char *listen_spec = NULL;
    int workers = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            listen_spec = argv[++i];
        } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            workers = atoi(argv[++i]);
            if (workers < 1) {
                fprintf(stderr, "ws-echo: --workers must be >= 1\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "ws-echo: unknown arg: %s\n\n", argv[i]);
            usage(stderr);
            return 2;
        }
    }
    if (!listen_spec) { usage(stderr); return 2; }

    char     host[16];
    uint16_t port;
    if (parse_listen(listen_spec, host, sizeof host, &port) != FFE_OK) {
        fprintf(stderr, "ws-echo: bad --listen spec: %s\n", listen_spec);
        return 2;
    }

    const Protocol *ws = ffproto_find("websocket");
    if (!ws) {
        FF_LOGE("websocket protocol not registered");
        return 1;
    }

    int rc = ffnet_server_run(ws, host, port, workers);
    if (rc != FFE_OK) {
        FF_LOGE("server_run: %s", ffutil_strerror_v(rc));
        return 1;
    }
    return 0;
}
