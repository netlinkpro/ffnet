// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Raw TCP echo subcommand — demonstrates the libffnet event loop without
// any protocol-module complexity. Useful as a reference for protocol
// authors and as a benchmark target for the multi-worker model.

#include "ffnet/ffnet.h"
#include "ffnet/ffproto.h"
#include "ffnet/ffutil.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *out)
{
    fprintf(out, "ffnet tcp-echo — raw TCP echo server (event loop demo)\n\n");
    fprintf(out, "usage:\n");
    fprintf(out, "  ffnet tcp-echo --listen <addr>:<port> [--workers N]\n\n");
    fprintf(out,
        "Echoes raw bytes back to each client (no protocol framing). Intended\n"
        "as a minimal exercise of the libffnet event loop and the SO_REUSEPORT\n"
        "+ pthread worker model.\n\n");
    fprintf(out, "arguments:\n");
    fprintf(out,
        "  --listen <addr>:<port>   IPv4 dotted-quad and TCP port. Port 0 for\n"
        "                           ephemeral; the bound port is printed when\n"
        "                           the server is ready.\n"
        "  --workers N              pthread workers (default 1).\n\n");
    fprintf(out, "example:\n");
    fprintf(out, "  ffnet tcp-echo --listen 127.0.0.1:9100 --workers 4\n");
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

/* --- inline Protocol for raw echo. No per-conn state needed. --- */

static int tcp_echo_open(int fd, void **ctx_out)
{
    (void)fd;
    *ctx_out = NULL;
    return FFE_OK;
}

static int tcp_echo_on_event(int fd, void *ctx, uint32_t events)
{
    (void)ctx;
    if (!(events & FF_EV_READ)) return FF_EV_READ;

    /* Read what's available and best-effort echo it. Real protocols
     * buffer; this demo writes inline because the data path is trivial. */
    uint8_t buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n == 0) return 0;                           /* peer EOF */
        if (n  < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return FF_EV_READ;
            return ffutil_map_errno(-errno);
        }
        ssize_t sent = 0;
        while (sent < n) {
            ssize_t w = write(fd, buf + sent, (size_t)(n - sent));
            if (w < 0) {
                if (errno == EINTR) continue;
                /* For an event-loop-correct echo we'd buffer here.
                 * The demo treats write failure as terminal. */
                return ffutil_map_errno(-errno);
            }
            sent += w;
        }
    }
}

static int tcp_echo_close(int fd, void *ctx)
{
    (void)fd; (void)ctx;
    return FFE_OK;
}

static const Protocol g_tcp_echo = {
    .name     = "tcp-echo",
    .open     = tcp_echo_open,
    .on_event = tcp_echo_on_event,
    .close    = tcp_echo_close,
};

int cmd_tcp_echo(int argc, char **argv)
{
    const char *listen_spec = NULL;
    int workers = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            listen_spec = argv[++i];
        } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            workers = atoi(argv[++i]);
            if (workers < 1) {
                fprintf(stderr, "tcp-echo: --workers must be >= 1\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "tcp-echo: unknown arg: %s\n\n", argv[i]);
            usage(stderr);
            return 2;
        }
    }
    if (!listen_spec) { usage(stderr); return 2; }

    char     host[16];
    uint16_t port;
    if (parse_listen(listen_spec, host, sizeof host, &port) != FFE_OK) {
        fprintf(stderr, "tcp-echo: bad --listen spec: %s\n", listen_spec);
        return 2;
    }

    int rc = ffnet_server_run(&g_tcp_echo, host, port, workers);
    if (rc != FFE_OK) {
        FF_LOGE("server_run: %s", ffutil_strerror_v(rc));
        return 1;
    }
    return 0;
}
