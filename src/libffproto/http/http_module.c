// SPDX-License-Identifier: LGPL-2.1-or-later
#include "http.h"
#include "ffnet/ffhttp.h"
#include "ffnet/ffproto.h"
#include "ffnet/ffnet.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * Global handler registration (single handler per process; slice 2).
 * --------------------------------------------------------------------------*/

static HttpHandler g_handler = NULL;
static void       *g_handler_user = NULL;

void ffhttp_set_handler(HttpHandler handler, void *user)
{
    g_handler = handler;
    g_handler_user = user;
}

/* ---------------------------------------------------------------------------
 * Per-connection state machine.
 * --------------------------------------------------------------------------*/

typedef enum {
    HTTP_STATE_WAIT_REQ,
    HTTP_STATE_READ_HEAD,
    HTTP_STATE_READ_BODY,
    HTTP_STATE_HANDLE,
    HTTP_STATE_WRITE_RESP,
    HTTP_STATE_CLOSING,
} HttpState;

typedef struct HttpConn {
    HttpState     state;
    FrameBuf      rd_buf;
    FrameBuf      wr_buf;
    size_t        wr_pos;

    /* Per-request scratch — reset at each WAIT_REQ → READ_HEAD transition. */
    HttpRequest  *req;
    HttpResponse *resp;
    int           keep_alive;
    size_t        body_remaining;
    int           chunked;
    int           chunk_state;
    size_t        chunk_remaining;

    /* Optional 100-Continue tracking. */
    int           sent_100;
} HttpConn;

#define HTTP_HEAD_CAP   16384   /* refuse a request line+headers larger than this */
#define HTTP_BODY_CAP   (1<<24) /* 16 MB request-body cap */
#define HTTP_RD_CHUNK   4096

/* ---------------------------------------------------------------------------
 * Buffer helpers.
 * --------------------------------------------------------------------------*/

static int drain_read(HttpConn *c, int fd)
{
    for (;;) {
        int rc = framebuf_reserve(&c->rd_buf, HTTP_RD_CHUNK);
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

static int drain_write(HttpConn *c, int fd)
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
    if (c->wr_pos > 0) {
        c->wr_buf.len = 0;
        c->wr_pos = 0;
    }
    return FFE_OK;
}

/* ---------------------------------------------------------------------------
 * Error response builder — writes a minimal "<code> <reason>" response
 * directly into wr_buf and transitions to CLOSING.
 * --------------------------------------------------------------------------*/

static int queue_simple_status(HttpConn *c, int code)
{
    HttpResponse *r = http_response_new();
    if (!r) return FFE_NOMEM;
    r->status = code;
    int rc = http_response_serialize(r, /*keep_alive=*/0, &c->wr_buf);
    http_response_free(r);
    if (rc != FFE_OK) return rc;
    c->state = HTTP_STATE_CLOSING;
    return FFE_OK;
}

/* ---------------------------------------------------------------------------
 * Process the accumulated header block: parse request, decide body model.
 * --------------------------------------------------------------------------*/

static int begin_request(HttpConn *c, size_t eoh)
{
    c->req = http_request_new();
    if (!c->req) return queue_simple_status(c, 500);

    int rc = http_parse_request(c->rd_buf.data, eoh, c->req);
    if (rc != FFE_OK) {
        return queue_simple_status(c, 400);
    }

    /* Validate version. */
    if (strcmp(c->req->version, "HTTP/1.1") != 0 &&
        strcmp(c->req->version, "HTTP/1.0") != 0) {
        return queue_simple_status(c, 505);
    }

    /* Validate path safety (decoded path). */
    if (!http_path_safe(c->req->path)) {
        return queue_simple_status(c, 400);
    }

    /* Determine body. */
    c->body_remaining = 0;
    c->chunked = 0;
    c->chunk_state = 0;
    c->chunk_remaining = 0;

    const char *te = ffhttp_req_header(c->req, "Transfer-Encoding");
    if (te && _ff_asm_http_ascii_lower_eq((const uint8_t *)te,
                                          (const uint8_t *)"chunked", 7)) {
        c->chunked = 1;
    } else {
        const char *cl = ffhttp_req_header(c->req, "Content-Length");
        if (cl) {
            char *end = NULL;
            unsigned long long n = strtoull(cl, &end, 10);
            if (end == cl || *end != '\0' || n > HTTP_BODY_CAP) {
                return queue_simple_status(c, 413);
            }
            c->body_remaining = (size_t)n;
        }
    }

    /* Connection: close header overrides default keep-alive. */
    c->keep_alive = (strcmp(c->req->version, "HTTP/1.1") == 0);
    const char *cn = ffhttp_req_header(c->req, "Connection");
    if (cn) {
        if (_ff_asm_http_ascii_lower_eq((const uint8_t *)cn,
                                        (const uint8_t *)"close", 5)) {
            c->keep_alive = 0;
        } else if (_ff_asm_http_ascii_lower_eq((const uint8_t *)cn,
                   (const uint8_t *)"keep-alive", 10)) {
            c->keep_alive = 1;
        }
    }

    /* Consume the header bytes from rd_buf. */
    memmove(c->rd_buf.data, c->rd_buf.data + eoh, c->rd_buf.len - eoh);
    c->rd_buf.len -= eoh;

    /* 100-Continue: if requested AND we have a body to read AND haven't
     * sent already, send "HTTP/1.1 100 Continue\r\n\r\n". */
    const char *expect = ffhttp_req_header(c->req, "Expect");
    if (!c->sent_100 && expect && (c->chunked || c->body_remaining > 0) &&
        _ff_asm_http_ascii_lower_eq((const uint8_t *)expect,
                                    (const uint8_t *)"100-continue", 12)) {
        int rc100 = framebuf_append(&c->wr_buf,
            "HTTP/1.1 100 Continue\r\n\r\n", 25);
        if (rc100 != FFE_OK) return rc100;
        c->sent_100 = 1;
    }

    /* Transition. */
    if (c->chunked || c->body_remaining > 0) {
        c->state = HTTP_STATE_READ_BODY;
    } else {
        c->state = HTTP_STATE_HANDLE;
    }
    return FFE_OK;
}

/* ---------------------------------------------------------------------------
 * Read body from rd_buf into req->body.
 * --------------------------------------------------------------------------*/

static int continue_read_body(HttpConn *c)
{
    if (c->chunked) {
        FrameBuf fb = {0};
        fb.data = c->req->body;
        fb.len  = c->req->body_len;
        /* framebuf API expects cap >= len; rough fit. We re-init below. */
        fb.cap  = c->req->body_len;

        size_t consumed = 0;
        int rc = http_chunk_decode(c->rd_buf.data, c->rd_buf.len, &consumed,
                                   &c->chunk_state, &c->chunk_remaining, &fb);
        c->req->body     = fb.data;
        c->req->body_len = fb.len;
        if (rc == FFE_AGAIN) {
            memmove(c->rd_buf.data, c->rd_buf.data + consumed,
                    c->rd_buf.len - consumed);
            c->rd_buf.len -= consumed;
            return FFE_OK;        /* need more bytes */
        }
        if (rc != FFE_OK) {
            return queue_simple_status(c, 400);
        }
        memmove(c->rd_buf.data, c->rd_buf.data + consumed,
                c->rd_buf.len - consumed);
        c->rd_buf.len -= consumed;
        c->state = HTTP_STATE_HANDLE;
        return FFE_OK;
    } else {
        /* Content-Length path. */
        size_t take = c->rd_buf.len < c->body_remaining
                    ? c->rd_buf.len : c->body_remaining;
        if (take > 0) {
            uint8_t *p = (uint8_t *)realloc(c->req->body, c->req->body_len + take);
            if (!p) return queue_simple_status(c, 500);
            memcpy(p + c->req->body_len, c->rd_buf.data, take);
            c->req->body = p;
            c->req->body_len += take;
            memmove(c->rd_buf.data, c->rd_buf.data + take,
                    c->rd_buf.len - take);
            c->rd_buf.len -= take;
            c->body_remaining -= take;
        }
        if (c->body_remaining == 0) {
            c->state = HTTP_STATE_HANDLE;
        }
        return FFE_OK;
    }
}

/* ---------------------------------------------------------------------------
 * Invoke handler, serialize response into wr_buf.
 * --------------------------------------------------------------------------*/

static int do_handle(HttpConn *c)
{
    c->resp = http_response_new();
    if (!c->resp) return queue_simple_status(c, 500);

    if (g_handler) {
        int hrc = g_handler(c->req, c->resp, g_handler_user);
        if (hrc < 0 && c->resp->status == 200) c->resp->status = 500;
    } else {
        c->resp->status = 501;
    }

    int rc = http_response_serialize(c->resp, c->keep_alive, &c->wr_buf);
    if (rc != FFE_OK) return rc;

    /* Done with per-request scratch. */
    http_request_free(c->req);
    http_response_free(c->resp);
    c->req = NULL;
    c->resp = NULL;
    c->sent_100 = 0;
    c->state = HTTP_STATE_WRITE_RESP;
    return FFE_OK;
}

/* ---------------------------------------------------------------------------
 * Drive the state machine after a read.
 * --------------------------------------------------------------------------*/

static int drive(HttpConn *c)
{
    for (;;) {
        switch (c->state) {
        case HTTP_STATE_WAIT_REQ:
        case HTTP_STATE_READ_HEAD: {
            int64_t eoh = _ff_asm_http_find_eoh(c->rd_buf.data, c->rd_buf.len);
            if (eoh < 0) {
                if (c->rd_buf.len > HTTP_HEAD_CAP) {
                    return queue_simple_status(c, 414);
                }
                return FFE_OK;
            }
            c->state = HTTP_STATE_READ_HEAD;
            int rc = begin_request(c, (size_t)eoh);
            if (rc != FFE_OK) return rc;
            break;
        }
        case HTTP_STATE_READ_BODY: {
            int rc = continue_read_body(c);
            if (rc != FFE_OK) return rc;
            if (c->state != HTTP_STATE_HANDLE) return FFE_OK;
            break;
        }
        case HTTP_STATE_HANDLE: {
            int rc = do_handle(c);
            if (rc != FFE_OK) return rc;
            break;
        }
        case HTTP_STATE_WRITE_RESP:
        case HTTP_STATE_CLOSING:
            return FFE_OK;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Protocol entry points (ABI v3).
 * --------------------------------------------------------------------------*/

static int http_open(int fd, void **ctx_out)
{
    (void)fd;
    HttpConn *c = (HttpConn *)calloc(1, sizeof(HttpConn));
    if (!c) return FFE_NOMEM;
    c->state = HTTP_STATE_WAIT_REQ;
    *ctx_out = c;
    return FFE_OK;
}

static int http_on_event(int fd, void *ctx, uint32_t events)
{
    HttpConn *c = (HttpConn *)ctx;
    int rc;

    if (events & FF_EV_READ) {
        rc = drain_read(c, fd);
        if (rc == FFE_CLOSED) {
            /* Peer EOF: if we have nothing to send back, close immediately. */
            if (c->wr_buf.len > c->wr_pos) {
                c->state = HTTP_STATE_CLOSING;
            } else {
                return 0;
            }
        } else if (rc != FFE_OK) {
            return rc;
        }

        if (c->state != HTTP_STATE_CLOSING && c->state != HTTP_STATE_WRITE_RESP) {
            rc = drive(c);
            if (rc != FFE_OK) return rc;
        }
    }

    if (c->wr_buf.len > c->wr_pos) {
        rc = drain_write(c, fd);
        if (rc != FFE_OK) return rc;
    }

    /* If WRITE_RESP finished draining and we're keep-alive, recycle. */
    if (c->state == HTTP_STATE_WRITE_RESP && c->wr_buf.len == c->wr_pos) {
        if (c->keep_alive) {
            c->state = HTTP_STATE_WAIT_REQ;
            /* Drive again in case the next request is already in rd_buf. */
            int drc = drive(c);
            if (drc != FFE_OK) return drc;
        } else {
            return 0;
        }
    }

    if (c->state == HTTP_STATE_CLOSING && c->wr_buf.len == c->wr_pos) {
        return 0;
    }

    uint32_t want = 0;
    if (c->state != HTTP_STATE_CLOSING) want |= FF_EV_READ;
    if (c->wr_buf.len > c->wr_pos)      want |= FF_EV_WRITE;
    if (want == 0) return 0;
    return (int)want;
}

static int http_close(int fd, void *ctx)
{
    (void)fd;
    HttpConn *c = (HttpConn *)ctx;
    if (!c) return FFE_OK;
    framebuf_free(&c->rd_buf);
    framebuf_free(&c->wr_buf);
    http_request_free(c->req);
    http_response_free(c->resp);
    free(c);
    return FFE_OK;
}

const Protocol ffproto_http = {
    .name     = "http",
    .open     = http_open,
    .on_event = http_on_event,
    .close    = http_close,
};
