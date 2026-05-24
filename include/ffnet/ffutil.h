// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef FFNET_FFUTIL_H
#define FFNET_FFUTIL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Error codes =====
 * Convention: 0 == success, negative == error, positive == success/byte count.
 * Numeric values are part of the ABI; do not renumber. */
#define FFE_OK         0
#define FFE_IO        -1   /* underlying I/O syscall failed (errno may be set) */
#define FFE_PROTOCOL  -2   /* protocol-level violation                          */
#define FFE_BADKEY    -3   /* malformed/missing WS handshake key                */
#define FFE_NOMEM     -4   /* allocation failed                                  */
#define FFE_CLOSED    -5   /* peer closed the connection cleanly                */
#define FFE_INVAL     -6   /* invalid argument                                   */

/* I/O-class codes (range 7..13). The `ffutil_strerror_v` helper uses this
 * range to decide whether the current errno is worth printing alongside. */
#define FFE_AGAIN    -7    /* EAGAIN / EWOULDBLOCK — try again                  */
#define FFE_REFUSED  -8    /* ECONNREFUSED                                       */
#define FFE_RESET    -9    /* ECONNRESET — peer reset the connection            */
#define FFE_PIPE    -10    /* EPIPE — broken pipe on write                       */
#define FFE_TIMEOUT -11    /* ETIMEDOUT                                          */
#define FFE_PERM    -12    /* EACCES / EPERM                                     */
#define FFE_INTR    -13    /* EINTR — signal interrupted the syscall             */

const char *ffutil_strerror(int code);

/* Translate a negative errno (-EAGAIN, -ECONNREFUSED, etc.) into the matching
 * FFE_* code. Returns FFE_IO for unrecognized values. Sets `errno =
 * -negative_errno` as a side effect so callers can follow up with
 * `strerror(errno)` for a human description. */
int ffutil_map_errno(int negative_errno);

/* Format an FFE_* code for a log line. For I/O-class codes (FFE_IO plus the
 * range FFE_AGAIN..FFE_INTR), appends " (<strerror>)" using the current
 * errno. For non-I/O codes (FFE_OK, FFE_PROTOCOL, FFE_BADKEY, FFE_NOMEM,
 * FFE_CLOSED, FFE_INVAL), returns just the FFE_* name to avoid printing
 * stale errno noise. Returns a pointer to a thread-unsafe static buffer;
 * revisit when slice 1.7 introduces threading. */
const char *ffutil_strerror_v(int rc);

/* ===== Logging =====
 * Output goes to stderr. Active level is read once from FFNET_LOG_LEVEL
 * ("error"|"warn"|"info"|"debug"); default is "warn". */
enum {
    FF_LOG_ERROR = 1,
    FF_LOG_WARN  = 2,
    FF_LOG_INFO  = 3,
    FF_LOG_DEBUG = 4
};

int  ff_log_enabled(int level);
void ff_log_emit(int level, const char *file, int line, const char *fmt, ...);

#define FF_LOGE(...) do { if (ff_log_enabled(FF_LOG_ERROR)) ff_log_emit(FF_LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__); } while (0)
#define FF_LOGW(...) do { if (ff_log_enabled(FF_LOG_WARN))  ff_log_emit(FF_LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__); } while (0)
#define FF_LOGI(...) do { if (ff_log_enabled(FF_LOG_INFO))  ff_log_emit(FF_LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__); } while (0)
#define FF_LOGD(...) do { if (ff_log_enabled(FF_LOG_DEBUG)) ff_log_emit(FF_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__); } while (0)

/* ===== FrameBuf =====
 * Growable byte buffer. `data` may move on grow; do not retain pointers
 * across calls that may grow it. `len` is current size, `cap` is allocated. */
typedef struct FrameBuf {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} FrameBuf;

int  framebuf_init(FrameBuf *b, size_t initial_cap);
int  framebuf_reserve(FrameBuf *b, size_t need);
int  framebuf_append(FrameBuf *b, const void *src, size_t n);
void framebuf_reset(FrameBuf *b);   /* zero len; keep capacity                 */
void framebuf_free(FrameBuf *b);

/* ===== Crypto / encoding =====
 * Implemented in libffutil (sha1.asm, base64.asm). */

/* SHA-1 (RFC 3174) one-shot. Writes a 20-byte digest to `out`. */
void ff_sha1(const uint8_t *data, uint64_t len, uint8_t out[20]);

/* Base64 encode (RFC 4648, standard alphabet, '=' padding).
 * Writes encoded chars to `dst`, plus a trailing NUL when `dst_cap` permits.
 * `dst_cap` must be >= ((src_len + 2) / 3) * 4  (and +1 for the NUL).
 * Returns chars written (excluding the NUL), or FFE_INVAL / FFE_NOMEM. */
int ff_base64_encode(const uint8_t *src, size_t src_len,
                     char *dst, size_t dst_cap);

/* Bytes required for `ff_base64_encode` output INCLUDING the trailing NUL. */
static inline size_t ff_base64_encoded_size(size_t src_len) {
    return ((src_len + 2) / 3) * 4 + 1;
}

#ifdef __cplusplus
}
#endif
#endif /* FFNET_FFUTIL_H */
