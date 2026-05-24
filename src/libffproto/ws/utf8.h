// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef FFNET_INTERNAL_UTF8_H
#define FFNET_INTERNAL_UTF8_H

#include <stdint.h>

/* States returned by ff_utf8_decode. Only ACCEPT and REJECT are meaningful
 * to callers; other values indicate the decoder is mid-sequence. */
#define FF_UTF8_ACCEPT  0
#define FF_UTF8_REJECT 12

/* Feed one byte into the decoder. Caller starts a new message with
 *   state = FF_UTF8_ACCEPT
 * and feeds bytes one at a time. After each call:
 *   - state == FF_UTF8_REJECT → the byte sequence is not valid UTF-8;
 *     the caller should reject the message (no further bytes will recover).
 *   - state == FF_UTF8_ACCEPT → all bytes so far form valid complete UTF-8.
 *     This is the only state at which the message may end cleanly.
 *   - any other value → mid-sequence (a multi-byte codepoint is in progress
 *     and more bytes are needed).
 *
 * State carries across calls, so the decoder works incrementally across
 * fragmented WebSocket TEXT messages. */
uint32_t ff_utf8_decode(uint32_t state, uint8_t byte);

#endif /* FFNET_INTERNAL_UTF8_H */
