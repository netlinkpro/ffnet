// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef FFNET_FFPROTO_H
#define FFNET_FFPROTO_H

#include "ffutil.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol module ABI.
 *
 * Designed against a single protocol (WebSocket). The fd-based, message-at-a-time
 * shape does not model multiplexed streams; expect this struct to evolve when
 * HTTP/2 or QUIC lands. Treat any field change as a major version bump.
 *
 * Connection lifecycle for the caller:
 *     void *ctx;
 *     server_handshake(fd, &ctx);            // may allocate per-conn state
 *     while (...) {
 *         read_frame (fd, ctx, &in);          // ctx threads through
 *         write_frame(fd, ctx, &out);
 *     }
 *     close(fd, ctx);                          // frees state; caller closes fd
 *
 * Stateless protocols set *ctx_out = NULL in server_handshake and ignore ctx
 * everywhere else; close() must accept NULL ctx as a no-op. */
typedef struct Protocol {
    const char *name;                                /* e.g. "websocket" */

    /* Server-side handshake. Reads request bytes from `fd` and replies.
     * On success, may allocate per-connection state and return it via
     * `*ctx_out`; stateless protocols set `*ctx_out = NULL`. Returns FFE_OK
     * on successful upgrade, negative on failure. */
    int (*server_handshake)(int fd, void **ctx_out);

    /* Read one application-level frame from `fd`, appended to `out`.
     * `ctx` is the value returned by server_handshake for this connection.
     * Returns FFE_OK on success, FFE_CLOSED on orderly peer close, or
     * negative on error. */
    int (*read_frame)(int fd, void *ctx, FrameBuf *out);

    /* Write one application-level frame from `in` to `fd`. */
    int (*write_frame)(int fd, void *ctx, const FrameBuf *in);

    /* Release per-connection state. Does NOT close `fd`. Safe to call with
     * NULL ctx. Returns FFE_OK or negative. */
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
