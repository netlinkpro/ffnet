// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ffnet/ffws.h"
#include "ffnet/ffproto.h"
#include "ffnet/ffutil.h"
#include "ffnet/byteorder.h"
#include "utf8.h"     /* sibling file in src/libffproto/ws/ */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>                    /* read, write */

/* Per-connection state, allocated in server_handshake, freed in close.
 * Holds the opcode of the most-recent inbound data frame so write_frame
 * can echo with a matching type (text → text, binary → binary). */
typedef struct WsConn {
    uint8_t  last_opcode;       /* slice 1: echo opcode matching             */
    uint8_t  fragment_opcode;   /* 0 / FFWS_OP_TEXT / FFWS_OP_BINARY         */
    uint32_t utf8_state;        /* DFA state; reset at start of each text msg */
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
        if (r <  0) {
            if (errno == EINTR) continue;       /* retry on signal */
            return ffutil_map_errno(-errno);
        }
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
        if (w <  0) {
            if (errno == EINTR) continue;
            return ffutil_map_errno(-errno);
        }
        if (w == 0) return FFE_IO;              /* shouldn't happen for blocking sockets */
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

    /* RSV1..3 must be zero unless an extension is negotiated. We don't
     * negotiate any, so any RSV bit set is a protocol error. */
    if (hdr[0] & 0x70) return FFE_PROTOCOL;

    meta->fin    = (hdr[0] & 0x80) ? 1 : 0;
    meta->opcode = hdr[0] & 0x0F;
    meta->masked = (hdr[1] & 0x80) ? 1 : 0;
    uint64_t plen = hdr[1] & 0x7F;

    /* Control frames (opcode >= 0x8) must have FIN=1 and payload <= 125. */
    if ((meta->opcode & 0x8) != 0) {
        if (!meta->fin) return FFE_PROTOCOL;
        if (plen > 125) return FFE_PROTOCOL;
    }

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

/* Send a Close frame with the given status code. Best-effort; on write
 * failure return the rc so the caller can still propagate the original
 * protocol error. The caller is expected to close the fd shortly after. */
static int ws_close_with(int fd, uint16_t code)
{
    uint8_t body[2];
    ff_be16_store(body, code);
    return write_frame_raw(fd, FFWS_OP_CLOSE, body, 2);
}

/* Validate freshly-appended TEXT bytes against the WsConn's incremental
 * UTF-8 state. Returns FFE_OK or FFE_PROTOCOL on reject. */
static int validate_text_chunk(WsConn *c, const uint8_t *data, size_t n)
{
    if (!c) return FFE_OK;
    for (size_t i = 0; i < n; i++) {
        c->utf8_state = ff_utf8_decode(c->utf8_state, data[i]);
        if (c->utf8_state == FF_UTF8_REJECT) return FFE_PROTOCOL;
    }
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
    /* fragment_opcode = 0 (no fragment in progress) and utf8_state = 0
     * (== FF_UTF8_ACCEPT) come for free from calloc. */
    *ctx_out = c;
    return FFE_OK;
}

static int ws_read_frame(int fd, void *ctx, FrameBuf *out)
{
    WsConn *c = (WsConn *)ctx;

    /* Loop until a complete data message has been assembled (FIN=1).
     * Control frames are handled inline and do not advance the message-
     * complete decision. Fragmented messages accumulate in `out` across
     * iterations; UTF-8 is validated incrementally as bytes arrive. */
    for (;;) {
        FfwsFrame meta;
        size_t mark = out->len;

        int rc = read_one_frame(fd, &meta, out);
        if (rc != FFE_OK) {
            if (rc == FFE_PROTOCOL) ws_close_with(fd, 1002);
            return rc;
        }

        /* --- control frames (interleave freely; don't touch fragment state) */
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
        }

        /* --- data frames --- */
        switch (meta.opcode) {
        case FFWS_OP_TEXT:
        case FFWS_OP_BINARY:
            /* A new data message begins; must not already be mid-fragment. */
            if (c && c->fragment_opcode != 0) {
                ws_close_with(fd, 1002);
                return FFE_PROTOCOL;
            }
            /* For TEXT, reset the validator and check the new bytes. */
            if (meta.opcode == FFWS_OP_TEXT) {
                if (c) c->utf8_state = FF_UTF8_ACCEPT;
                if (validate_text_chunk(c, out->data + mark,
                                        out->len - mark) != FFE_OK) {
                    ws_close_with(fd, 1007);
                    return FFE_PROTOCOL;
                }
            }
            if (meta.fin) {
                if (meta.opcode == FFWS_OP_TEXT &&
                    c && c->utf8_state != FF_UTF8_ACCEPT) {
                    ws_close_with(fd, 1007);
                    return FFE_PROTOCOL;
                }
                if (c) c->last_opcode = meta.opcode;
                return FFE_OK;
            }
            /* Fragment start; remember opcode and read the next frame. */
            if (c) c->fragment_opcode = meta.opcode;
            continue;

        case FFWS_OP_CONT:
            if (!c || c->fragment_opcode == 0) {
                ws_close_with(fd, 1002);
                return FFE_PROTOCOL;
            }
            if (c->fragment_opcode == FFWS_OP_TEXT) {
                if (validate_text_chunk(c, out->data + mark,
                                        out->len - mark) != FFE_OK) {
                    ws_close_with(fd, 1007);
                    return FFE_PROTOCOL;
                }
            }
            if (meta.fin) {
                if (c->fragment_opcode == FFWS_OP_TEXT &&
                    c->utf8_state != FF_UTF8_ACCEPT) {
                    ws_close_with(fd, 1007);
                    return FFE_PROTOCOL;
                }
                c->last_opcode = c->fragment_opcode;
                c->fragment_opcode = 0;
                return FFE_OK;
            }
            continue;

        default:
            /* Reserved opcodes 0x3..0x7 and 0xB..0xF. */
            ws_close_with(fd, 1002);
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
