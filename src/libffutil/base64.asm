; SPDX-License-Identifier: LGPL-2.1-or-later
; base64.asm — RFC 4648 Base64 encoder (standard alphabet) for ffnet
;
; int ff_base64_encode(const uint8_t *src, size_t src_len,
;                       char *dst, size_t dst_cap)
;     rdi = src
;     rsi = src_len
;     rdx = dst
;     rcx = dst_cap
;     return rax = bytes written (excluding trailing NUL) on success;
;            FFE_INVAL or FFE_NOMEM on failure.
;
; A trailing NUL is written when dst_cap > encoded_len.
; Encoded length (no NUL) = ((src_len + 2) / 3) * 4.

bits 64
default rel

global ff_base64_encode

%define FFE_NOMEM -4
%define FFE_INVAL -6

section .rodata
align 16
ff_base64_alphabet:
    db "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

section .text

ff_base64_encode:
    push    r12
    push    r15

    ; --- validate dst -------------------------------------------------------
    test    rdx, rdx
    jnz     .have_dst
    mov     rax, FFE_INVAL
    jmp     .epilogue
.have_dst:
    ; r12 = original dst (for return-length calculation)
    mov     r12, rdx

    ; --- required encoded length (excluding NUL) = ((rsi + 2) / 3) * 4 ------
    lea     rax, [rsi + 2]
    xor     rdx, rdx
    mov     r8,  3
    div     r8                      ; rax = (rsi+2)/3
    shl     rax, 2                  ; rax = required output bytes
    cmp     rcx, rax
    jb      .nomem
    mov     rdx, r12                ; restore current-dst pointer

    lea     r15, [rel ff_base64_alphabet]

    ; --- main loop: 3 src bytes → 4 dst chars -------------------------------
.group3:
    cmp     rsi, 3
    jb      .tail

    movzx   r8d,  byte [rdi]        ; b0
    movzx   r9d,  byte [rdi + 1]    ; b1
    movzx   r10d, byte [rdi + 2]    ; b2

    ; out[0] = alpha[b0 >> 2]
    mov     eax, r8d
    shr     eax, 2
    mov     al, [r15 + rax]
    mov     [rdx], al

    ; out[1] = alpha[((b0 & 3) << 4) | (b1 >> 4)]
    mov     eax, r8d
    and     eax, 3
    shl     eax, 4
    mov     r11d, r9d
    shr     r11d, 4
    or      eax, r11d
    mov     al, [r15 + rax]
    mov     [rdx + 1], al

    ; out[2] = alpha[((b1 & 0x0F) << 2) | (b2 >> 6)]
    mov     eax, r9d
    and     eax, 0x0F
    shl     eax, 2
    mov     r11d, r10d
    shr     r11d, 6
    or      eax, r11d
    mov     al, [r15 + rax]
    mov     [rdx + 2], al

    ; out[3] = alpha[b2 & 0x3F]
    mov     eax, r10d
    and     eax, 0x3F
    mov     al, [r15 + rax]
    mov     [rdx + 3], al

    add     rdi, 3
    add     rdx, 4
    sub     rsi, 3
    sub     rcx, 4
    jmp     .group3

.tail:
    test    rsi, rsi
    jz      .done
    cmp     rsi, 1
    je      .tail1

    ; --- two-byte tail: 3 chars + one '=' ---------------------------------
    movzx   r8d, byte [rdi]
    movzx   r9d, byte [rdi + 1]

    mov     eax, r8d
    shr     eax, 2
    mov     al, [r15 + rax]
    mov     [rdx], al

    mov     eax, r8d
    and     eax, 3
    shl     eax, 4
    mov     r11d, r9d
    shr     r11d, 4
    or      eax, r11d
    mov     al, [r15 + rax]
    mov     [rdx + 1], al

    mov     eax, r9d
    and     eax, 0x0F
    shl     eax, 2
    mov     al, [r15 + rax]
    mov     [rdx + 2], al

    mov     byte [rdx + 3], '='
    add     rdx, 4
    sub     rcx, 4
    jmp     .done

.tail1:
    ; --- one-byte tail: 2 chars + two '=' ---------------------------------
    movzx   r8d, byte [rdi]

    mov     eax, r8d
    shr     eax, 2
    mov     al, [r15 + rax]
    mov     [rdx], al

    mov     eax, r8d
    and     eax, 3
    shl     eax, 4
    mov     al, [r15 + rax]
    mov     [rdx + 1], al

    mov     byte [rdx + 2], '='
    mov     byte [rdx + 3], '='
    add     rdx, 4
    sub     rcx, 4

.done:
    ; Write trailing NUL if room remains.
    test    rcx, rcx
    jz      .compute_ret
    mov     byte [rdx], 0
.compute_ret:
    mov     rax, rdx
    sub     rax, r12                ; rax = bytes written (excluding NUL)
    jmp     .epilogue

.nomem:
    mov     rax, FFE_NOMEM

.epilogue:
    pop     r15
    pop     r12
    ret

; Mark stack as non-executable (Linux/binutils convention).
section .note.GNU-stack noalloc noexec nowrite progbits
