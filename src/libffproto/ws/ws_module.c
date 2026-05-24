// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ffnet/ffws.h"
#include "ffnet/ffproto.h"
#include "ffnet/ffutil.h"
#include "ffnet/byteorder.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>                    /* read, write */

/* Per-connection state, allocated in server_handshake, freed in close.
 * Holds the opcode of the most-recent inbound data frame so write_frame
 * can echo with a matching type (text → text, binary → binary). */
typedef struct WsConn {
    uint8_t last_opcode;
} WsConn;

/* ----- asm-side primitives (defined in ws_handshake.asm, ws_frame.asm) ---- */
extern int  _ff_asm_ws_find_key(const uint8_t *buf, uint64_t buflen,
                                const uint8_t **out_key, uint64_t *out_keylen);
extern int  _ff_asm_ws_accept_key(const uint8_t *key, uint64_t keylen,
                                  char out[29]);
extern int  _ff_asm_ws_send_response(int fd, const char *accept_key,
                                     uint64_t accept_len);
extern void _ff_asm_ws_unmask(uint8_t *data, uint64_t len,
                              const uint8_t mask[4]);

#define WS_HANDSHAKE_BUF 4096

/* ---------------- syscall loops ----------------------------------------- */

static int read_n(int fd, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) return FFE_CLOSED;
        if (r <  0) return FFE_IO;
        got += (size_t)r;
    }
    return FFE_OK;
}

static int write_n(int fd, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, p + sent, n - sent);
        if (w <= 0) return FFE_IO;
        sent += (size_t)w;
    }
    return FFE_OK;
}

/* ---------------- one-frame reader -------------------------------------- */

static int read_one_frame(int fd, FfwsFrame *meta, FrameBuf *payload_out)
{
    uint8_t hdr[2];
    int rc = read_n(fd, hdr, 2);
    if (rc != FFE_OK) return rc;

    meta->fin    = (hdr[0] & 0x80) ? 1 : 0;
    meta->opcode = hdr[0] & 0x0F;
    meta->masked = (hdr[1] & 0x80) ? 1 : 0;
    uint64_t plen = hdr[1] & 0x7F;

    if (plen == 126) {
        uint8_t ext[2];
        rc = read_n(fd, ext, 2);
        if (rc != FFE_OK) return rc;
        plen = ff_be16_load(ext);
    } else if (plen == 127) {
        uint8_t ext[8];
        rc = read_n(fd, ext, 8);
        if (rc != FFE_OK) return rc;
        plen = ff_be64_load(ext);
    }
    meta->payload_len = plen;

    if (meta->masked) {
        rc = read_n(fd, meta->mask, 4);
        if (rc != FFE_OK) return rc;
    } else {
        memset(meta->mask, 0, 4);
        /* RFC 6455 §5.1: client MUST mask. Reject unmasked client frames. */
        if (plen > 0) return FFE_PROTOCOL;
    }

    if (plen == 0) return FFE_OK;

    rc = framebuf_reserve(payload_out, (size_t)plen);
    if (rc != FFE_OK) return rc;
    rc = read_n(fd, payload_out->data + payload_out->len, (size_t)plen);
    if (rc != FFE_OK) return rc;

    if (meta->masked) {
        _ff_asm_ws_unmask(payload_out->data + payload_out->len, plen, meta->mask);
    }
    payload_out->len += (size_t)plen;
    return FFE_OK;
}

/* ---------------- frame writer (any opcode) ----------------------------- */

static int write_frame_raw(int fd, uint8_t opcode, const void *data, size_t n)
{
    uint8_t hdr[10];
    size_t  hlen;

    hdr[0] = 0x80 | (opcode & 0x0F);              /* FIN=1, no RSV bits */

    if (n < 126) {
        hdr[1] = (uint8_t)n;
        hlen = 2;
    } else if (n <= 0xFFFFu) {
        hdr[1] = 126;
        ff_be16_store(&hdr[2], (uint16_t)n);
        hlen = 4;
    } else {
        hdr[1] = 127;
        ff_be64_store(&hdr[2], (uint64_t)n);
        hlen = 10;
    }

    int rc = write_n(fd, hdr, hlen);
    if (rc != FFE_OK) return rc;
    if (n > 0)        return write_n(fd, data, n);
    return FFE_OK;
}

/* ---------------- Protocol entry points --------------------------------- */

static int ws_server_handshake(int fd, void **ctx_out)
{
    *ctx_out = NULL;

    uint8_t buf[WS_HANDSHAKE_BUF];
    size_t  used = 0;

    while (used < sizeof(buf)) {
        ssize_t r = read(fd, buf + used, sizeof(buf) - used);
        if (r == 0) return FFE_CLOSED;
        if (r <  0) return FFE_IO;
        used += (size_t)r;

        if (used >= 4) {
            for (size_t i = 0; i + 4 <= used; i++) {
                if (buf[i]   == '\r' && buf[i + 1] == '\n' &&
                    buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                    goto have_request;
                }
            }
        }
    }
    return FFE_PROTOCOL;       /* request larger than handshake buffer */

have_request:;
    const uint8_t *key;
    uint64_t keylen;
    int rc = _ff_asm_ws_find_key(buf, used, &key, &keylen);
    if (rc != 0) return FFE_BADKEY;

    char accept[29];
    rc = _ff_asm_ws_accept_key(key, keylen, accept);
    if (rc != 0) return FFE_BADKEY;

    rc = _ff_asm_ws_send_response(fd, accept, 28);
    if (rc < 0) return FFE_IO;

    WsConn *c = (WsConn *)calloc(1, sizeof(WsConn));
    if (!c) return FFE_NOMEM;
    c->last_opcode = FFWS_OP_TEXT;     /* default if write precedes any read */
    *ctx_out = c;
    return FFE_OK;
}

static int ws_read_frame(int fd, void *ctx, FrameBuf *out)
{
    WsConn *c = (WsConn *)ctx;

    /* Loop until a data frame arrives. Control frames are handled inline. */
    for (;;) {
        FfwsFrame meta;
        size_t mark = out->len;

        int rc = read_one_frame(fd, &meta, out);
        if (rc != FFE_OK) return rc;

        switch (meta.opcode) {
        case FFWS_OP_CLOSE:
            return FFE_CLOSED;

        case FFWS_OP_PING: {
            int wrc = write_frame_raw(fd, FFWS_OP_PONG,
                                      out->data + mark, out->len - mark);
            out->len = mark;
            if (wrc != FFE_OK) return wrc;
            continue;
        }

        case FFWS_OP_PONG:
            out->len = mark;
            continue;

        case FFWS_OP_TEXT:
        case FFWS_OP_BINARY:
            if (!meta.fin) return FFE_PROTOCOL;   /* slice 1: no fragments */
            if (c) c->last_opcode = meta.opcode;
            return FFE_OK;

        default:
            return FFE_PROTOCOL;
        }
    }
}

static int ws_write_frame(int fd, void *ctx, const FrameBuf *in)
{
    WsConn *c = (WsConn *)ctx;
    uint8_t opcode = (c && c->last_opcode) ? c->last_opcode : FFWS_OP_TEXT;
    return write_frame_raw(fd, opcode, in->data, in->len);
}

static int ws_close(int fd, void *ctx)
{
    (void)fd;
    free(ctx);
    return FFE_OK;
}

const Protocol ffproto_websocket = {
    .name             = "websocket",
    .server_handshake = ws_server_handshake,
    .read_frame       = ws_read_frame,
    .write_frame      = ws_write_frame,
    .close            = ws_close,
};

/* ---------------- public ffws_send_* helpers ---------------------------- */

int ffws_send_text(int fd, const void *data, size_t n)
{
    return write_frame_raw(fd, FFWS_OP_TEXT, data, n);
}

int ffws_send_binary(int fd, const void *data, size_t n)
{
    return write_frame_raw(fd, FFWS_OP_BINARY, data, n);
}

int ffws_send_close(int fd, uint16_t status)
{
    if (status == 0) return write_frame_raw(fd, FFWS_OP_CLOSE, NULL, 0);
    uint8_t body[2];
    ff_be16_store(body, status);
    return write_frame_raw(fd, FFWS_OP_CLOSE, body, 2);
}

int ffws_send_pong(int fd, const void *data, size_t n)
{
    return write_frame_raw(fd, FFWS_OP_PONG, data, n);
}
