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
    fprintf(out, "  ffnet ws-echo --listen <addr>:<port>\n\n");
    fprintf(out,
        "Each text/binary frame received from a client is echoed back to that\n"
        "client with the same opcode. Control frames (ping, close) are handled\n"
        "inline per RFC 6455. Single-threaded, blocking I/O, one client at a time\n"
        "in slice 1.\n\n");
    fprintf(out, "arguments:\n");
    fprintf(out,
        "  --listen <addr>:<port>   IPv4 dotted-quad and TCP port. Use port 0 to\n"
        "                           request an ephemeral port; the actual bound\n"
        "                           port is printed on stdout when ready.\n\n");
    fprintf(out, "example:\n");
    fprintf(out, "  ffnet ws-echo --listen 127.0.0.1:8080\n");
}

/* Split "host:port" into a host string and a uint16_t port. */
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

/* Run one connection through the full Protocol lifecycle. */
static int serve_one(const Protocol *proto, int client_fd)
{
    void *ctx = NULL;
    int rc = proto->server_handshake(client_fd, &ctx);
    if (rc != FFE_OK) {
        FF_LOGW("handshake: %s", ffutil_strerror_v(rc));
        proto->close(client_fd, ctx);
        return rc;
    }

    FrameBuf buf;
    framebuf_init(&buf, 0);

    for (;;) {
        framebuf_reset(&buf);
        rc = proto->read_frame(client_fd, ctx, &buf);
        if (rc == FFE_CLOSED) { rc = FFE_OK; break; }
        if (rc != FFE_OK) {
            FF_LOGW("read_frame: %s", ffutil_strerror_v(rc));
            break;
        }
        rc = proto->write_frame(client_fd, ctx, &buf);
        if (rc != FFE_OK) {
            FF_LOGW("write_frame: %s", ffutil_strerror_v(rc));
            break;
        }
    }

    framebuf_free(&buf);
    proto->close(client_fd, ctx);
    return rc;
}

int cmd_ws_echo(int argc, char **argv)
{
    const char *listen_spec = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            listen_spec = argv[++i];
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

    int listen_fd = ffnet_tcp_create();
    if (listen_fd < 0) {
        FF_LOGE("socket: %s", ffutil_strerror_v(listen_fd));
        return 1;
    }

    int rc = ffnet_tcp_bind_listen(listen_fd, host, port, 16);
    if (rc != FFE_OK) {
        FF_LOGE("bind_listen %s:%u: %s", host, (unsigned)port, ffutil_strerror_v(rc));
        ffnet_tcp_close(listen_fd);
        return 1;
    }

    int bound_port = ffnet_tcp_local_port(listen_fd);
    if (bound_port < 0) {
        FF_LOGE("local_port: %s", ffutil_strerror_v(bound_port));
        ffnet_tcp_close(listen_fd);
        return 1;
    }

    /* Stdout, unbuffered, so integration tests can read the bound port
     * immediately after spawn (e.g. when --listen used port 0). */
    printf("listening on %s:%d\n", host, bound_port);
    fflush(stdout);

    for (;;) {
        int client_fd = ffnet_tcp_accept(listen_fd);
        if (client_fd < 0) {
            FF_LOGW("accept: %s", ffutil_strerror_v(client_fd));
            continue;
        }
        FF_LOGI("client connected");
        serve_one(ws, client_fd);
        ffnet_tcp_close(client_fd);
        FF_LOGI("client disconnected");
    }
}
