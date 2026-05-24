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

/* Send a complete text frame (FIN=1, server-side: unmasked). */
int ffws_send_text(int fd, const void *data, size_t n);

/* Send a complete binary frame. */
int ffws_send_binary(int fd, const void *data, size_t n);

/* Send a close frame; pass status == 0 to omit the body. */
int ffws_send_close(int fd, uint16_t status);

/* Send a pong frame echoing the given payload (RFC 6455 §5.5.3). */
int ffws_send_pong(int fd, const void *data, size_t n);

#ifdef __cplusplus
}
#endif
#endif /* FFNET_FFWS_H */
