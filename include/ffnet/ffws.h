// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef FFNET_FFWS_H
#define FFNET_FFWS_H

#include "ffutil.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WebSocket opcodes (RFC 6455 §5.2). */
#define FFWS_OP_CONT   0x0
#define FFWS_OP_TEXT   0x1
#define FFWS_OP_BINARY 0x2
#define FFWS_OP_CLOSE  0x8
#define FFWS_OP_PING   0x9
#define FFWS_OP_PONG   0xA

/* Metadata for a decoded inbound frame. Payload bytes (already unmasked)
 * live in the caller's FrameBuf. */
typedef struct FfwsFrame {
    uint8_t  fin;
    uint8_t  opcode;
    uint8_t  masked;        /* RFC 6455: always 1 for client->server */
    uint64_t payload_len;
    uint8_t  mask[4];       /* zero when masked == 0 */
} FfwsFrame;

/* Note: the previous slice exposed blocking ffws_send_* helpers (text,
 * binary, close, pong). They were removed when the WS module migrated
 * to the event-driven Protocol ABI v3 (slice 1.7) because their
 * blocking semantics don't fit the new model. Callers wanting to send
 * WS frames should use the Protocol ABI via ffnet_server_run, or open
 * an issue if a blocking-helper variant is needed. */

#ifdef __cplusplus
}
#endif
#endif /* FFNET_FFWS_H */
