; SPDX-License-Identifier: LGPL-2.1-or-later
; ws_handshake.asm — WebSocket opening-handshake primitives for ffnet.
;
;   int _ff_asm_ws_find_key(const uint8_t *buf, uint64_t buflen,
;                            const uint8_t **out_key, uint64_t *out_keylen)
;       Scans `buf` for "Sec-WebSocket-Key:" (case-sensitive), skips one
;       optional space, reads the value up to CR or LF. Stores a pointer
;       into the original buffer and the value length.
;       -> 0 on found, FFE_BADKEY on missing/empty.
;
;   int _ff_asm_ws_accept_key(const uint8_t *key, uint64_t keylen,
;                              char out[29])
;       Computes base64(SHA1(key || magic_guid)). Writes 28 chars + NUL.
;       -> 0 on success, FFE_BADKEY if keylen > 92.
;
;   int _ff_asm_ws_send_response(int fd, const char *accept_key,
;                                 uint64_t accept_len)
;       Constructs the HTTP/1.1 101 Switching Protocols response and
;       writes it via sys_write.
;       -> 0 on success, -errno on failure.

bits 64
default rel

global _ff_asm_ws_find_key
global _ff_asm_ws_accept_key
global _ff_asm_ws_send_response

extern ff_sha1
extern ff_base64_encode

%define SYS_write    1
%define FFE_BADKEY  -3

section .rodata
align 16
ws_key_header:
    db "Sec-WebSocket-Key:"
ws_key_header_len equ $ - ws_key_header                 ; 18

ws_magic_guid:
    db "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
ws_magic_guid_len equ $ - ws_magic_guid                 ; 36

; Response template, split around the place where the accept key is spliced in.
ws_resp_a:
    db "HTTP/1.1 101 Switching Protocols", 13, 10
    db "Upgrade: websocket", 13, 10
    db "Connection: Upgrade", 13, 10
    db "Sec-WebSocket-Accept: "
ws_resp_a_len equ $ - ws_resp_a                         ; 97

ws_resp_b:
    db 13, 10, 13, 10
ws_resp_b_len equ $ - ws_resp_b                         ; 4

section .text

; -----------------------------------------------------------------------------
; _ff_asm_ws_find_key
;     rdi = buf
;     rsi = buflen
;     rdx = out_key   (uint8_t **)
;     rcx = out_keylen(uint64_t *)
_ff_asm_ws_find_key:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15

    cmp     rsi, ws_key_header_len
    jb      .nomatch

    mov     rbx, rdi                ; rbx = current scan position
    mov     r12, rsi                ; r12 = remaining bytes
    mov     r13, rdx                ; r13 = &out_key
    mov     r14, rcx                ; r14 = &out_keylen

.scan_loop:
    cmp     r12, ws_key_header_len
    jb      .nomatch

    ; repe cmpsb advances rsi, rdi and decrements rcx.
    mov     rdi, rbx
    lea     rsi, [rel ws_key_header]
    mov     rcx, ws_key_header_len
    repe cmpsb
    je      .found_header

    inc     rbx
    dec     r12
    jmp     .scan_loop

.found_header:
    add     rbx, ws_key_header_len
    sub     r12, ws_key_header_len

    ; Skip ONE optional space (HTTP allows OWS but most clients send a single space).
    test    r12, r12
    jz      .nomatch
    cmp     byte [rbx], ' '
    jne     .read_value
    inc     rbx
    dec     r12

.read_value:
    mov     r15, rbx                ; key start
    xor     rcx, rcx                ; key length

.read_loop:
    cmp     rcx, r12
    jae     .have_value
    movzx   eax, byte [rbx + rcx]
    cmp     eax, 13
    je      .have_value
    cmp     eax, 10
    je      .have_value
    inc     rcx
    jmp     .read_loop

.have_value:
    test    rcx, rcx
    jz      .nomatch                ; empty value is not a valid key
    mov     [r13], r15              ; *out_key    = key start
    mov     [r14], rcx              ; *out_keylen = length
    xor     eax, eax
    jmp     .ret

.nomatch:
    mov     eax, FFE_BADKEY
.ret:
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; _ff_asm_ws_accept_key
;     rdi = key
;     rsi = keylen
;     rdx = out (29 bytes)
_ff_asm_ws_accept_key:
    push    rbx
    push    r12
    push    r13
    push    r14
    sub     rsp, 152                ; 128 concat + 20 digest + 4 pad
                                    ; → rsp aligned to 16 for call sites

    cmp     rsi, 92
    ja      .badkey

    mov     rbx, rdi                ; key ptr
    mov     r12, rsi                ; keylen
    mov     r13, rdx                ; out ptr

    ; Copy key to [rsp + 0]
    mov     rdi, rsp
    mov     rsi, rbx
    mov     rcx, r12
    rep movsb

    ; Append magic GUID at [rsp + keylen]
    lea     rsi, [rel ws_magic_guid]
    mov     rcx, ws_magic_guid_len
    rep movsb

    ; Total concat length = keylen + 36
    lea     r14, [r12 + ws_magic_guid_len]

    ; ff_sha1(concat, total_len, digest)
    mov     rdi, rsp
    mov     rsi, r14
    lea     rdx, [rsp + 128]
    call    ff_sha1 wrt ..plt

    ; ff_base64_encode(digest, 20, out, 29)
    lea     rdi, [rsp + 128]
    mov     esi, 20
    mov     rdx, r13
    mov     ecx, 29
    call    ff_base64_encode wrt ..plt

    xor     eax, eax
    jmp     .ret

.badkey:
    mov     eax, FFE_BADKEY
.ret:
    add     rsp, 152
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; _ff_asm_ws_send_response
;     rdi = fd
;     rsi = accept_key
;     rdx = accept_len
;
; Writes:  ws_resp_a + accept_key + ws_resp_b   (single sys_write)
_ff_asm_ws_send_response:
    push    rbx
    push    r12
    push    r13
    push    r14
    sub     rsp, 200                ; response staging buffer

    mov     r12d, edi               ; fd
    mov     r13, rsi                ; accept_key ptr
    mov     r14, rdx                ; accept_len

    ; Copy ws_resp_a to [rsp + 0]
    mov     rdi, rsp
    lea     rsi, [rel ws_resp_a]
    mov     rcx, ws_resp_a_len
    rep movsb

    ; Append accept_key
    mov     rsi, r13
    mov     rcx, r14
    rep movsb

    ; Append ws_resp_b
    lea     rsi, [rel ws_resp_b]
    mov     rcx, ws_resp_b_len
    rep movsb

    ; total = ws_resp_a_len + accept_len + ws_resp_b_len
    mov     rbx, ws_resp_a_len
    add     rbx, r14
    add     rbx, ws_resp_b_len

    ; sys_write(fd, rsp, rbx)
    mov     edi, r12d
    mov     rsi, rsp
    mov     rdx, rbx
    mov     eax, SYS_write
    syscall
    test    rax, rax
    js      .ret                    ; rax = -errno

    xor     eax, eax                ; success → 0
.ret:
    add     rsp, 200
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; Mark stack as non-executable (Linux/binutils convention).
section .note.GNU-stack noalloc noexec nowrite progbits
