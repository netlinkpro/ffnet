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

const char *ffutil_strerror(int code);

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
