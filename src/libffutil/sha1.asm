; SPDX-License-Identifier: LGPL-2.1-or-later
; sha1.asm — SHA-1 (RFC 3174) for ffnet
;
; void ff_sha1(const uint8_t *data, uint64_t len, uint8_t out[20])
;     rdi = data
;     rsi = len   (bytes)
;     rdx = out   (20-byte digest output)
;
; System V AMD64 ABI. Callee-saved regs (rbx, r12-r15) preserved.
; Pure computation: no syscalls, no libc calls.
;
; Stack frame (480 bytes, 16-aligned):
;     [rsp +   0 .. + 19]  H[0..4]            (5 dwords, working state)
;     [rsp +  20 .. + 23]  tail_count         (1 dword, 1 or 2)
;     [rsp +  24 .. + 31]  padding for align
;     [rsp +  32 .. +351]  W[0..79]           (320 bytes, message schedule)
;     [rsp + 352 .. +479]  tail scratch       (128 bytes; worst-case 2 blocks)

bits 64
default rel

global ff_sha1

section .rodata
align 16
ff_sha1_h_init:
    dd 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0

section .text

ff_sha1:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 480

    mov     rbx, rdi                ; rbx = data pointer (current block base)
    mov     r12, rsi                ; r12 = remaining bytes
    mov     r13, rdx                ; r13 = output digest pointer
    mov     r14, rsi
    shl     r14, 3                  ; r14 = total bit length

    ; init H[0..4] from ff_sha1_h_init
    lea     rsi, [rel ff_sha1_h_init]
    mov     rdi, rsp
    mov     ecx, 5
    rep movsd

ff_sha1_loop_full:
    cmp     r12, 64
    jb      ff_sha1_build_tail
    call    ff_sha1_compress
    add     rbx, 64
    sub     r12, 64
    jmp     ff_sha1_loop_full

ff_sha1_build_tail:
    ; Copy r12 (0..63) remaining bytes into tail scratch.
    lea     rdi, [rsp + 352]
    mov     rsi, rbx
    mov     rcx, r12
    rep movsb
    ; Append 0x80 sentinel.
    mov     byte [rdi], 0x80
    inc     rdi
    ; Decide one-block vs two-block padding based on bytes used (r12 + 1).
    mov     rax, r12
    inc     rax
    cmp     rax, 56
    jbe     .one_block
    mov     dword [rsp + 20], 2
    lea     r15, [rsp + 352 + 128 - 8]   ; length field at end of 2nd block
    jmp     .zero_fill
.one_block:
    mov     dword [rsp + 20], 1
    lea     r15, [rsp + 352 + 64 - 8]    ; length field at end of 1st block
.zero_fill:
    ; zero [rdi .. r15)
    mov     rcx, r15
    sub     rcx, rdi
    xor     al, al
    rep stosb
    ; Big-endian 64-bit bit length.
    mov     rax, r14
    bswap   rax
    mov     [r15], rax

    ; Compress tail blocks.
    lea     rbx, [rsp + 352]
ff_sha1_compress_tail:
    mov     ecx, [rsp + 20]
    test    ecx, ecx
    jz      ff_sha1_finalize
    call    ff_sha1_compress
    add     rbx, 64
    dec     dword [rsp + 20]
    jmp     ff_sha1_compress_tail

ff_sha1_finalize:
    ; Write H[0..4] big-endian to [r13].
    xor     ecx, ecx
.wloop:
    mov     eax, [rsp + rcx*4]
    bswap   eax
    mov     [r13 + rcx*4], eax
    inc     ecx
    cmp     ecx, 5
    jb      .wloop

    add     rsp, 480
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; ---------------------------------------------------------------------------
; ff_sha1_compress: compress one 64-byte block at [rbx] into H at [rsp+0..19].
; Clobbers caller-saved regs (rax, rcx, rdx, rsi, rdi, r8-r11) and r15.
; Preserves rbx, r12, r13, r14 (the caller's loop state).
; ---------------------------------------------------------------------------
ff_sha1_compress:
    ; Load 16 big-endian dwords into W[0..15].
    xor     ecx, ecx
.load:
    mov     eax, [rbx + rcx*4]
    bswap   eax
    mov     [rsp + 32 + rcx*4], eax
    inc     ecx
    cmp     ecx, 16
    jb      .load

    ; Expand W[16..79] = ROL(W[t-3] ^ W[t-8] ^ W[t-14] ^ W[t-16], 1).
    mov     ecx, 16
.expand:
    mov     eax, [rsp + 32 + rcx*4 - 12]    ; W[t-3]
    xor     eax, [rsp + 32 + rcx*4 - 32]    ; W[t-8]
    xor     eax, [rsp + 32 + rcx*4 - 56]    ; W[t-14]
    xor     eax, [rsp + 32 + rcx*4 - 64]    ; W[t-16]
    rol     eax, 1
    mov     [rsp + 32 + rcx*4], eax
    inc     ecx
    cmp     ecx, 80
    jb      .expand

    ; Working vars: a=r8d, b=r9d, c=r10d, d=r11d, e=esi.
    mov     r8d,  [rsp +  0]
    mov     r9d,  [rsp +  4]
    mov     r10d, [rsp +  8]
    mov     r11d, [rsp + 12]
    mov     esi,  [rsp + 16]

    xor     ecx, ecx
.round:
    cmp     ecx, 20
    jb      .f0
    cmp     ecx, 40
    jb      .f1
    cmp     ecx, 60
    jb      .f2
    ; --- F3 (60..79):  b XOR c XOR d
    mov     eax, r9d
    xor     eax, r10d
    xor     eax, r11d
    mov     edi, 0xCA62C1D6
    jmp     .apply
.f0:
    ; F0 (0..19): (b & c) | (~b & d)  ==  d XOR (b AND (c XOR d))
    mov     eax, r10d
    xor     eax, r11d
    and     eax, r9d
    xor     eax, r11d
    mov     edi, 0x5A827999
    jmp     .apply
.f1:
    ; F1 (20..39): b XOR c XOR d
    mov     eax, r9d
    xor     eax, r10d
    xor     eax, r11d
    mov     edi, 0x6ED9EBA1
    jmp     .apply
.f2:
    ; F2 (40..59): (b & c) | (b & d) | (c & d)
    mov     eax, r9d
    or      eax, r10d
    and     eax, r11d           ; eax = d & (b | c)
    mov     r15d, r9d
    and     r15d, r10d          ; r15d = b & c
    or      eax, r15d
    mov     edi, 0x8F1BBCDC
.apply:
    ; eax holds F(b,c,d); edi holds K_t.
    add     eax, esi                  ; + e
    add     eax, edi                  ; + K_t
    add     eax, [rsp + 32 + rcx*4]   ; + W[t]
    mov     edi, r8d
    rol     edi, 5
    add     eax, edi                  ; + ROL5(a)  → eax = new a

    mov     esi,  r11d                ; e <- d
    mov     r11d, r10d                ; d <- c
    rol     r9d,  30                  ; b := ROL(b, 30)
    mov     r10d, r9d                 ; c <- ROL30(b)
    mov     r9d,  r8d                 ; b <- a (old)
    mov     r8d,  eax                 ; a <- new

    inc     ecx
    cmp     ecx, 80
    jb      .round

    add     [rsp +  0], r8d
    add     [rsp +  4], r9d
    add     [rsp +  8], r10d
    add     [rsp + 12], r11d
    add     [rsp + 16], esi
    ret
