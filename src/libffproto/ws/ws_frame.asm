; SPDX-License-Identifier: LGPL-2.1-or-later
; ws_frame.asm — WebSocket frame hot path: payload unmask.
;
; void _ff_asm_ws_unmask(uint8_t *data, uint64_t len, const uint8_t mask[4])
;     rdi = data
;     rsi = len
;     rdx = mask  (4 bytes)
;
; In-place XOR of data[i] ^= mask[i % 4].
;
; Byte-at-a-time for slice 1 clarity. A 64-bit replicated-mask path
; (load mask as a u32 ×2 into a u64 register and XOR 8 bytes per step,
;  plus a residue loop) is the natural next optimization.

bits 64
default rel

global _ff_asm_ws_unmask

section .text

_ff_asm_ws_unmask:
    test    rsi, rsi
    jz      .done
    xor     rcx, rcx
.loop:
    mov     r8, rcx
    and     r8, 3
    mov     al, [rdx + r8]
    xor     [rdi + rcx], al
    inc     rcx
    cmp     rcx, rsi
    jb      .loop
.done:
    ret
