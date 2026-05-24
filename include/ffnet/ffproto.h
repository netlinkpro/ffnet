// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef FFNET_FFPROTO_H
#define FFNET_FFPROTO_H

#include "ffutil.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol module ABI v3 (event-driven, slice 1.7+).
 *
 * Each connection is a state machine owned by the protocol. The framework
 * (libffnet/server.c) handles accept/close-fd and dispatches epoll events;
 * the protocol owns everything that flows over fd. Slice 1.7 retired the
 * earlier blocking-call ABI (server_handshake/read_frame/write_frame).
 *
 * Lifecycle:
 *   open      called once after accept. fd is already non-blocking.
 *   on_event  called whenever an event the protocol requested fires on fd.
 *             Protocol reads/writes from/to fd (handling EAGAIN itself)
 *             and returns the next event mask, 0 to close, or negative.
 *   close     called when on_event returned 0 or negative, before the
 *             framework closes fd. */
typedef struct Protocol {
    const char *name;                                /* e.g. "websocket" */

    /* Allocate per-connection state. Returns FFE_OK and writes ctx into
     * *ctx_out, or negative on failure (framework closes fd directly,
     * neither on_event nor close are called). */
    int (*open)(int fd, void **ctx_out);

    /* Returns:
     *   > 0  new event mask (FF_EV_READ | FF_EV_WRITE) to wait on next.
     *   = 0  close this connection cleanly.
     *   < 0  error; framework logs ffutil_strerror_v(rc) then closes. */
    int (*on_event)(int fd, void *ctx, uint32_t events);

    /* Free per-connection state. Framework closes fd after returning. */
    int (*close)(int fd, void *ctx);
} Protocol;

/* Compiled-in protocols. */
extern const Protocol ffproto_websocket;

/* Look up a protocol by name across compiled-in and runtime-registered tables.
 * Returns NULL if not found. */
const Protocol *ffproto_find(const char *name);

/* Register a protocol at runtime. `proto` must outlive the process (typically
 * a static const). Returns FFE_OK, FFE_NOMEM, or FFE_INVAL on name collision. */
int ffproto_register(const Protocol *proto);

#ifdef __cplusplus
}
#endif
#endif /* FFNET_FFPROTO_H */
