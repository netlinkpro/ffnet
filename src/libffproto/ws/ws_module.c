// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ffnet/ffws.h"
#include "ffnet/ffproto.h"
#include "ffnet/ffutil.h"
#include "ffnet/ffnet.h"
#include "ffnet/byteorder.h"
#include "utf8.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>                    /* read, write */

/* ----- asm-side primitives (defined in ws_handshake.asm, ws_frame.asm) ---- */
extern int  _ff_asm_ws_find_key(const uint8_t *buf, uint64_t buflen,
                                const uint8_t **out_key, uint64_t *out_keylen);
extern int  _ff_asm_ws_accept_key(const uint8_t *key, uint64_t keylen,
                                  char out[29]);
extern void _ff_asm_ws_unmask(uint8_t *data, uint64_t len,
                              const uint8_t mask[4]);

#define WS_HANDSHAKE_CAP   8192    /* refuse a request larger than this */
#define WS_RD_CHUNK        4096    /* read this much per pass */

typedef enum WsState {
    WS_STATE_HANDSHAKE,
    WS_STATE_OPEN,
    WS_STATE_CLOSING,
} WsState;

typedef struct WsConn {
    WsState   state;
    FrameBuf  rd_buf;        /* inbound bytes not yet parsed */
    FrameBuf  wr_buf;        /* outbound bytes not yet written */
    size_t    wr_pos;        /* number of bytes already drained from wr_buf */
    FrameBuf  msg_buf;       /* accumulates current data message across fragments */
    uint8_t   fragment_opcode;   /* 0 / FFWS_OP_TEXT / FFWS_OP_BINARY */
    uint32_t  utf8_state;
    uint8_t   last_opcode;
} WsConn;

/* ---------------------------------------------------------------------------
 * Outbound queue helpers — all queue bytes into wr_buf, never touch fd.
 * --------------------------------------------------------------------------*/

static int queue_handshake_response(WsConn *c, const char *accept_key,
                                    size_t accept_len)
{
    static const char resp_a[] =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: ";
    static const char resp_b[] = "\r\n\r\n";

    int rc = framebuf_append(&c->wr_buf, resp_a, sizeof(resp_a) - 1);
    if (rc != FFE_OK) return rc;
    rc = framebuf_append(&c->wr_buf, accept_key, accept_len);
    if (rc != FFE_OK) return rc;
    return framebuf_append(&c->wr_buf, resp_b, sizeof(resp_b) - 1);
}

static int queue_frame(WsConn *c, uint8_t opcode, const void *data, size_t n)
{
    uint8_t hdr[10];
    size_t  hlen;

    hdr[0] = 0x80 | (opcode & 0x0F);              /* FIN=1, no RSV */

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

    int rc = framebuf_append(&c->wr_buf, hdr, hlen);
    if (rc != FFE_OK) return rc;
    if (n > 0) return framebuf_append(&c->wr_buf, data, n);
    return FFE_OK;
}

static int queue_close(WsConn *c, uint16_t code)
{
    uint8_t body[2];
    ff_be16_store(body, code);
    return queue_frame(c, FFWS_OP_CLOSE, body, 2);
}

static int validate_text_chunk(WsConn *c, const uint8_t *data, size_t n)
{
    if (!c) return FFE_OK;
    for (size_t i = 0; i < n; i++) {
        c->utf8_state = ff_utf8_decode(c->utf8_state, data[i]);
        if (c->utf8_state == FF_UTF8_REJECT) return FFE_PROTOCOL;
    }
    return FFE_OK;
}

/* ---------------------------------------------------------------------------
 * Frame parser — operates on rd_buf in place. Returns FFE_OK with consumed
 * count, FFE_AGAIN (need more bytes), or FFE_PROTOCOL.
 * --------------------------------------------------------------------------*/

typedef struct ParseResult {
    int       rc;
    FfwsFrame meta;
    size_t    payload_off;   /* offset within buf of payload start */
    size_t    total_len;     /* total bytes consumed (header + ext_len + mask + payload) */
} ParseResult;

static ParseResult try_parse_frame(const FrameBuf *buf)
{
    ParseResult r;
    memset(&r, 0, sizeof r);

    if (buf->len < 2) { r.rc = FFE_AGAIN; return r; }
    const uint8_t *p = buf->data;

    if (p[0] & 0x70) { r.rc = FFE_PROTOCOL; return r; }   /* RSV bits */

    r.meta.fin    = (p[0] & 0x80) ? 1 : 0;
    r.meta.opcode = p[0] & 0x0F;
    r.meta.masked = (p[1] & 0x80) ? 1 : 0;
    uint64_t plen = p[1] & 0x7F;
    size_t off = 2;

    if (plen == 126) {
        if (buf->len < off + 2) { r.rc = FFE_AGAIN; return r; }
        plen = ff_be16_load(p + off);
        off += 2;
    } else if (plen == 127) {
        if (buf->len < off + 8) { r.rc = FFE_AGAIN; return r; }
        plen = ff_be64_load(p + off);
        off += 8;
    }

    /* Control frames must have FIN=1 and payload ≤ 125. */
    if ((r.meta.opcode & 0x8) != 0) {
        if (!r.meta.fin) { r.rc = FFE_PROTOCOL; return r; }
        if (plen > 125) { r.rc = FFE_PROTOCOL; return r; }
    }
    r.meta.payload_len = plen;

    if (r.meta.masked) {
        if (buf->len < off + 4) { r.rc = FFE_AGAIN; return r; }
        memcpy(r.meta.mask, p + off, 4);
        off += 4;
    } else {
        memset(r.meta.mask, 0, 4);
        if (plen > 0) { r.rc = FFE_PROTOCOL; return r; }   /* RFC: client MUST mask */
    }

    if (buf->len < off + plen) { r.rc = FFE_AGAIN; return r; }

    r.payload_off = off;
    r.total_len   = off + (size_t)plen;
    r.rc = FFE_OK;
    return r;
}

/* ---------------------------------------------------------------------------
 * dispatch_frame — process a complete parsed frame (payload already unmasked
 * in caller). Echoes data messages, handles control frames, manages
 * fragmentation + UTF-8 state. Queues Close into wr_buf on protocol error.
 * Returns FFE_OK to continue, or FFE_PROTOCOL on error (state transitions to
 * CLOSING in caller).
 * --------------------------------------------------------------------------*/

static int dispatch_frame(WsConn *c, const FfwsFrame *meta, const uint8_t *payload)
{
    int rc;

    switch (meta->opcode) {
    case FFWS_OP_CLOSE: {
        /* RFC 6455 §5.5.1, §7.4: validate peer's close payload before echoing. */
        uint16_t code = 1000;

        if (meta->payload_len == 1) {
            /* 1-byte payload is malformed (must be 0 or >=2). */
            queue_close(c, 1002);
            c->state = WS_STATE_CLOSING;
            return FFE_PROTOCOL;
        }
        if (meta->payload_len >= 2) {
            code = ff_be16_load(payload);
            /* Valid status codes: 1000, 1001, 1002, 1003, 1007–1011, 3000–4999.
             * Reserved-not-to-use (1004, 1005, 1006, 1015), below-range (<1000),
             * gaps (1012–1014, 1016+ before 3000), and extension-reserved
             * (2000–2999) all → protocol error. */
            int valid =
                code == 1000 || code == 1001 || code == 1002 || code == 1003 ||
                (code >= 1007 && code <= 1011) ||
                (code >= 3000 && code <= 4999);
            if (!valid) {
                queue_close(c, 1002);
                c->state = WS_STATE_CLOSING;
                return FFE_PROTOCOL;
            }
            /* Reason text (bytes 2..) must be valid UTF-8. */
            if (meta->payload_len > 2) {
                uint32_t st = FF_UTF8_ACCEPT;
                for (size_t i = 2; i < (size_t)meta->payload_len; i++) {
                    st = ff_utf8_decode(st, payload[i]);
                    if (st == FF_UTF8_REJECT) break;
                }
                if (st != FF_UTF8_ACCEPT) {
                    queue_close(c, 1007);
                    c->state = WS_STATE_CLOSING;
                    return FFE_PROTOCOL;
                }
            }
        }
        /* Echo peer's code (or 1000 if no payload). */
        if (queue_close(c, code) != FFE_OK) return FFE_NOMEM;
        c->state = WS_STATE_CLOSING;
        return FFE_OK;
    }

    case FFWS_OP_PING:
        return queue_frame(c, FFWS_OP_PONG, payload, (size_t)meta->payload_len);

    case FFWS_OP_PONG:
        return FFE_OK;   /* unsolicited pong: ignore */

    case FFWS_OP_TEXT:
    case FFWS_OP_BINARY:
        if (c->fragment_opcode != 0) {
            queue_close(c, 1002);
            return FFE_PROTOCOL;
        }
        if (meta->opcode == FFWS_OP_TEXT) {
            c->utf8_state = FF_UTF8_ACCEPT;
            if (validate_text_chunk(c, payload, (size_t)meta->payload_len) != FFE_OK) {
                queue_close(c, 1007);
                return FFE_PROTOCOL;
            }
        }
        rc = framebuf_append(&c->msg_buf, payload, (size_t)meta->payload_len);
        if (rc != FFE_OK) return rc;
        if (meta->fin) {
            if (meta->opcode == FFWS_OP_TEXT && c->utf8_state != FF_UTF8_ACCEPT) {
                queue_close(c, 1007);
                return FFE_PROTOCOL;
            }
            rc = queue_frame(c, meta->opcode, c->msg_buf.data, c->msg_buf.len);
            framebuf_reset(&c->msg_buf);
            c->last_opcode = meta->opcode;
            return rc;
        }
        c->fragment_opcode = meta->opcode;
        return FFE_OK;

    case FFWS_OP_CONT:
        if (c->fragment_opcode == 0) {
            queue_close(c, 1002);
            return FFE_PROTOCOL;
        }
        if (c->fragment_opcode == FFWS_OP_TEXT) {
            if (validate_text_chunk(c, payload, (size_t)meta->payload_len) != FFE_OK) {
                queue_close(c, 1007);
                return FFE_PROTOCOL;
            }
        }
        rc = framebuf_append(&c->msg_buf, payload, (size_t)meta->payload_len);
        if (rc != FFE_OK) return rc;
        if (meta->fin) {
            if (c->fragment_opcode == FFWS_OP_TEXT && c->utf8_state != FF_UTF8_ACCEPT) {
                queue_close(c, 1007);
                return FFE_PROTOCOL;
            }
            rc = queue_frame(c, c->fragment_opcode, c->msg_buf.data, c->msg_buf.len);
            framebuf_reset(&c->msg_buf);
            c->last_opcode = c->fragment_opcode;
            c->fragment_opcode = 0;
            return rc;
        }
        return FFE_OK;

    default:
        /* Reserved opcodes 0x3..0x7 and 0xB..0xF. */
        queue_close(c, 1002);
        return FFE_PROTOCOL;
    }
}

/* ---------------------------------------------------------------------------
 * Handshake processor — scans rd_buf for end-of-headers, runs find_key +
 * accept_key, queues 101 response, transitions to OPEN. Returns FFE_OK
 * (transitioned or waiting for more bytes) or negative on protocol error.
 * --------------------------------------------------------------------------*/

static int process_handshake(WsConn *c)
{
    /* Look for \r\n\r\n. */
    if (c->rd_buf.len < 4) return FFE_OK;          /* keep reading */

    size_t end = 0;
    int found = 0;
    for (size_t i = 0; i + 4 <= c->rd_buf.len; i++) {
        if (c->rd_buf.data[i]     == '\r' && c->rd_buf.data[i + 1] == '\n' &&
            c->rd_buf.data[i + 2] == '\r' && c->rd_buf.data[i + 3] == '\n') {
            end = i + 4;
            found = 1;
            break;
        }
    }
    if (!found) {
        if (c->rd_buf.len > WS_HANDSHAKE_CAP) return FFE_PROTOCOL;
        return FFE_OK;
    }

    const uint8_t *key;
    uint64_t keylen;
    if (_ff_asm_ws_find_key(c->rd_buf.data, end, &key, &keylen) != 0) {
        return FFE_BADKEY;
    }

    char accept[29];
    if (_ff_asm_ws_accept_key(key, keylen, accept) != 0) return FFE_BADKEY;

    int rc = queue_handshake_response(c, accept, 28);
    if (rc != FFE_OK) return rc;

    /* Consume the handshake bytes from rd_buf. */
    memmove(c->rd_buf.data, c->rd_buf.data + end, c->rd_buf.len - end);
    c->rd_buf.len -= end;

    c->state = WS_STATE_OPEN;
    return FFE_OK;
}

/* ---------------------------------------------------------------------------
 * Frame processor — runs the parser repeatedly on rd_buf until it returns
 * AGAIN or PROTOCOL. Per-frame work goes through dispatch_frame.
 * --------------------------------------------------------------------------*/

static int process_frames(WsConn *c)
{
    for (;;) {
        ParseResult r = try_parse_frame(&c->rd_buf);
        if (r.rc == FFE_AGAIN) return FFE_OK;
        if (r.rc != FFE_OK) {
            queue_close(c, 1002);
            c->state = WS_STATE_CLOSING;
            return FFE_OK;
        }

        if (r.meta.masked && r.meta.payload_len > 0) {
            _ff_asm_ws_unmask(c->rd_buf.data + r.payload_off,
                              r.meta.payload_len, r.meta.mask);
        }

        int rc = dispatch_frame(c, &r.meta,
                                c->rd_buf.data + r.payload_off);

        /* Consume the frame from rd_buf. */
        memmove(c->rd_buf.data, c->rd_buf.data + r.total_len,
                c->rd_buf.len - r.total_len);
        c->rd_buf.len -= r.total_len;

        if (rc != FFE_OK) {
            c->state = WS_STATE_CLOSING;
            return FFE_OK;
        }
        if (c->state != WS_STATE_OPEN) return FFE_OK;
    }
}

/* ---------------------------------------------------------------------------
 * I/O drain helpers.
 * --------------------------------------------------------------------------*/

/* Read as much as possible into rd_buf. Returns FFE_OK (got some, EAGAIN
 * reached), FFE_CLOSED (peer EOF), or negative on error. */
static int drain_read(WsConn *c, int fd)
{
    for (;;) {
        int rc = framebuf_reserve(&c->rd_buf, WS_RD_CHUNK);
        if (rc != FFE_OK) return rc;
        ssize_t n = read(fd, c->rd_buf.data + c->rd_buf.len,
                         c->rd_buf.cap - c->rd_buf.len);
        if (n == 0) return FFE_CLOSED;
        if (n  < 0) {
            if (errno == EINTR)        continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return FFE_OK;
            return ffutil_map_errno(-errno);
        }
        c->rd_buf.len += (size_t)n;
    }
}

/* Write as much as possible from wr_buf. Returns FFE_OK or negative. */
static int drain_write(WsConn *c, int fd)
{
    while (c->wr_pos < c->wr_buf.len) {
        ssize_t n = write(fd, c->wr_buf.data + c->wr_pos,
                          c->wr_buf.len - c->wr_pos);
        if (n < 0) {
            if (errno == EINTR)        continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return FFE_OK;
            return ffutil_map_errno(-errno);
        }
        c->wr_pos += (size_t)n;
    }
    /* All drained: compact. */
    if (c->wr_pos > 0) {
        c->wr_buf.len = 0;
        c->wr_pos = 0;
    }
    return FFE_OK;
}

/* ---------------------------------------------------------------------------
 * Protocol entry points (ABI v3).
 * --------------------------------------------------------------------------*/

static int ws_open(int fd, void **ctx_out)
{
    (void)fd;
    WsConn *c = (WsConn *)calloc(1, sizeof(WsConn));
    if (!c) return FFE_NOMEM;
    c->state = WS_STATE_HANDSHAKE;
    c->last_opcode = FFWS_OP_TEXT;
    /* utf8_state == FF_UTF8_ACCEPT (== 0) from calloc.
     * fragment_opcode == 0 (no fragment in progress) from calloc. */
    *ctx_out = c;
    return FFE_OK;
}

static int ws_on_event(int fd, void *ctx, uint32_t events)
{
    WsConn *c = (WsConn *)ctx;
    int rc;

    if (events & FF_EV_READ) {
        rc = drain_read(c, fd);
        if (rc == FFE_CLOSED) {
            /* Peer EOF. If we still have bytes to write, drain them then close;
             * otherwise close immediately. */
            if (c->wr_buf.len > c->wr_pos) {
                c->state = WS_STATE_CLOSING;
            } else {
                return 0;
            }
        } else if (rc != FFE_OK) {
            return rc;
        }

        if (c->state == WS_STATE_HANDSHAKE) {
            rc = process_handshake(c);
            if (rc != FFE_OK) {
                /* Protocol error during handshake: just close (peer is HTTP,
                 * not a WS client yet — no point sending Close frame). */
                return 0;
            }
        }
        if (c->state == WS_STATE_OPEN) {
            rc = process_frames(c);
            if (rc != FFE_OK) return rc;
        }
    }

    if (c->wr_buf.len > c->wr_pos) {
        rc = drain_write(c, fd);
        if (rc != FFE_OK) return rc;
    }

    /* If closing and drained, we're done. */
    if (c->state == WS_STATE_CLOSING && c->wr_buf.len == c->wr_pos) {
        return 0;
    }

    /* Compute next event mask. */
    uint32_t want = 0;
    if (c->state != WS_STATE_CLOSING) want |= FF_EV_READ;
    if (c->wr_buf.len > c->wr_pos)    want |= FF_EV_WRITE;
    if (want == 0) return 0;          /* nothing to wait for: close */
    return (int)want;
}

static int ws_close(int fd, void *ctx)
{
    (void)fd;
    WsConn *c = (WsConn *)ctx;
    if (!c) return FFE_OK;
    framebuf_free(&c->rd_buf);
    framebuf_free(&c->wr_buf);
    framebuf_free(&c->msg_buf);
    free(c);
    return FFE_OK;
}

const Protocol ffproto_websocket = {
    .name     = "websocket",
    .open     = ws_open,
    .on_event = ws_on_event,
    .close    = ws_close,
};
