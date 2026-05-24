; SPDX-License-Identifier: LGPL-2.1-or-later
; http_parse.asm — HTTP/1.1 parser scanning hot paths.
;
;   int64_t _ff_asm_http_find_eoh(const uint8_t *buf, uint64_t len)
;       Scans `buf[0..len)` for the byte sequence "\r\n\r\n" (CR LF CR LF).
;       Returns the offset of the first byte AFTER the match (i.e. start
;       of the body), or -1 if not found.
;       rdi = buf, rsi = len ; returns rax.
;
;   int64_t _ff_asm_http_find_eol(const uint8_t *buf, uint64_t len)
;       Same idea for "\r\n" (CR LF). Returns offset after the match or -1.
;
;   int _ff_asm_http_ascii_lower_eq(const uint8_t *a, const uint8_t *b,
;                                    uint64_t n)
;       Case-insensitive ASCII compare. ASCII letters A-Z and a-z match
;       irrespective of case (via the `or 0x20` trick — works for letters
;       only; other bytes compare strictly). Returns 1 if all n bytes
;       match, else 0. Pure computation, no syscalls.
;
; System V AMD64 ABI. No SIMD; plain byte loops. SIMD vectorization is
; a future perf slice.

bits 64
default rel

global _ff_asm_http_find_eoh
global _ff_asm_http_find_eol
global _ff_asm_http_ascii_lower_eq

section .text

; -----------------------------------------------------------------------------
; _ff_asm_http_find_eoh
;     rdi = buf, rsi = len -> rax (offset of byte after "\r\n\r\n", or -1)
_ff_asm_http_find_eoh:
    cmp     rsi, 4
    jb      .notfound

    xor     rcx, rcx                ; i = 0
.scan:
    ; Need to read [rdi+i .. rdi+i+3]; while i + 4 <= len.
    mov     rax, rcx
    add     rax, 4
    cmp     rax, rsi
    ja      .notfound

    cmp     byte [rdi + rcx],     0x0D
    jne     .next
    cmp     byte [rdi + rcx + 1], 0x0A
    jne     .next
    cmp     byte [rdi + rcx + 2], 0x0D
    jne     .next
    cmp     byte [rdi + rcx + 3], 0x0A
    jne     .next

    ; Match at rcx; return rcx + 4.
    lea     rax, [rcx + 4]
    ret

.next:
    inc     rcx
    jmp     .scan

.notfound:
    mov     rax, -1
    ret

; -----------------------------------------------------------------------------
; _ff_asm_http_find_eol
;     rdi = buf, rsi = len -> rax (offset of byte after "\r\n", or -1)
_ff_asm_http_find_eol:
    cmp     rsi, 2
    jb      .notfound

    xor     rcx, rcx
.scan:
    mov     rax, rcx
    add     rax, 2
    cmp     rax, rsi
    ja      .notfound

    cmp     byte [rdi + rcx],     0x0D
    jne     .next
    cmp     byte [rdi + rcx + 1], 0x0A
    jne     .next

    lea     rax, [rcx + 2]
    ret

.next:
    inc     rcx
    jmp     .scan

.notfound:
    mov     rax, -1
    ret

; -----------------------------------------------------------------------------
; _ff_asm_http_ascii_lower_eq
;     rdi = a, rsi = b, rdx = n -> eax (1 if case-insensitively equal, 0 else)
;
; For each byte: if both are ASCII letters (A-Z or a-z) they compare equal
; regardless of case. Other bytes compare strictly.
_ff_asm_http_ascii_lower_eq:
    test    rdx, rdx
    jnz     .loop_init
    mov     eax, 1                  ; n == 0: trivially equal
    ret

.loop_init:
    xor     rcx, rcx                ; i = 0
.loop:
    movzx   eax, byte [rdi + rcx]   ; ca
    movzx   r8d, byte [rsi + rcx]   ; cb

    ; Check ca is in A-Z (0x41..0x5A) or a-z (0x61..0x7A). For letters,
    ; (c | 0x20) lowercases. For non-letters we must keep them as-is.
    ;
    ; Compute lower(ca):
    ;   tmp = ca | 0x20
    ;   if 'a' <= tmp <= 'z'  →  use tmp, else use ca
    mov     r9d, eax
    or      r9d, 0x20
    cmp     r9d, 'a'
    jb      .keep_a
    cmp     r9d, 'z'
    ja      .keep_a
    mov     eax, r9d                ; ca is a letter; use lowercase
.keep_a:

    mov     r9d, r8d
    or      r9d, 0x20
    cmp     r9d, 'a'
    jb      .keep_b
    cmp     r9d, 'z'
    ja      .keep_b
    mov     r8d, r9d
.keep_b:

    cmp     eax, r8d
    jne     .mismatch

    inc     rcx
    cmp     rcx, rdx
    jb      .loop

    mov     eax, 1
    ret

.mismatch:
    xor     eax, eax
    ret

; Mark stack as non-executable (Linux/binutils convention).
section .note.GNU-stack noalloc noexec nowrite progbits
