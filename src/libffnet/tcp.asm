; SPDX-License-Identifier: LGPL-2.1-or-later
; tcp.asm — TCP socket primitives for ffnet, Linux/x86_64 raw syscalls.
;
; Internal asm-side routines (System V AMD64 ABI). Each returns 0/fd on
; success, or -errno on failure. The C wrappers in tcp.c map -errno → FFE_*
; and expose the public ffnet_tcp_* ABI.
;
;   int _ff_asm_socket_create(void)
;       -> fd >= 0, or -errno
;
;   int _ff_asm_bind_listen(int fd, uint32_t ip_be, uint16_t port_be, int backlog)
;       sets SO_REUSEADDR, binds to ip_be:port_be, listens with backlog.
;       -> 0, or -errno
;
;   int _ff_asm_accept(int fd)
;       -> client fd >= 0, or -errno
;
;   int _ff_asm_local_port(int fd)
;       -> local port (host order, 0..65535), or -errno
;
;   int _ff_asm_close(int fd)
;       -> 0, or -errno
;
; Linux/x86_64 syscall ABI:
;     syscall nr in rax;
;     args in rdi, rsi, rdx, r10, r8, r9  (note: r10, not rcx);
;     return in rax (negative == -errno);
;     syscall clobbers rcx and r11.

bits 64
default rel

global _ff_asm_socket_create
global _ff_asm_bind_listen
global _ff_asm_accept
global _ff_asm_local_port
global _ff_asm_close

%define SYS_close       3
%define SYS_socket      41
%define SYS_accept      43
%define SYS_bind        49
%define SYS_listen      50
%define SYS_getsockname 51
%define SYS_setsockopt  54

%define AF_INET         2
%define SOCK_STREAM     1
%define SOL_SOCKET      1
%define SO_REUSEADDR    2

section .text

; -----------------------------------------------------------------------------
; int _ff_asm_socket_create(void)
;     sys_socket(AF_INET, SOCK_STREAM, 0)
_ff_asm_socket_create:
    mov     edi, AF_INET
    mov     esi, SOCK_STREAM
    xor     edx, edx
    mov     eax, SYS_socket
    syscall
    ret

; -----------------------------------------------------------------------------
; int _ff_asm_bind_listen(int fd, uint32_t ip_be, uint16_t port_be, int backlog)
;     rdi=fd, esi=ip_be, dx=port_be, ecx=backlog
;
; 1) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, 4)
; 2) build struct sockaddr_in { AF_INET, port_be, ip_be, zero[8] } on stack
; 3) sys_bind(fd, &sa, 16)
; 4) sys_listen(fd, backlog)
_ff_asm_bind_listen:
    push    r12
    push    r13                     ; 2 pushes; combined with ret addr → rsp%16 == 0
    sub     rsp, 24                 ; 16 sockaddr + 4 optval + 4 pad

    mov     r12d, edi               ; r12 = fd
    mov     r13d, ecx               ; r13 = backlog

    ; sockaddr_in at [rsp + 0 .. + 15]
    mov     word  [rsp +  0], AF_INET
    mov     word  [rsp +  2], dx    ; port (already big-endian)
    mov     dword [rsp +  4], esi   ; ip   (already big-endian)
    mov     qword [rsp +  8], 0     ; sin_zero[8]

    ; optval = 1 at [rsp + 16]
    mov     dword [rsp + 16], 1

    ; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, 4)
    mov     edi, r12d
    mov     esi, SOL_SOCKET
    mov     edx, SO_REUSEADDR
    lea     r10, [rsp + 16]
    mov     r8d, 4
    mov     eax, SYS_setsockopt
    syscall
    test    rax, rax
    js      .ret

    ; bind(fd, &sa, 16)
    mov     edi, r12d
    lea     rsi, [rsp]
    mov     edx, 16
    mov     eax, SYS_bind
    syscall
    test    rax, rax
    js      .ret

    ; listen(fd, backlog)
    mov     edi, r12d
    mov     esi, r13d
    mov     eax, SYS_listen
    syscall
    test    rax, rax
    js      .ret

    xor     eax, eax                ; success
.ret:
    add     rsp, 24
    pop     r13
    pop     r12
    ret

; -----------------------------------------------------------------------------
; int _ff_asm_accept(int fd)
;     sys_accept(fd, NULL, NULL)
_ff_asm_accept:
    ; rdi = fd already
    xor     esi, esi                ; struct sockaddr* = NULL
    xor     edx, edx                ; socklen_t*       = NULL
    mov     eax, SYS_accept
    syscall
    ret

; -----------------------------------------------------------------------------
; int _ff_asm_local_port(int fd)
;     getsockname(fd, &sa, &salen); return sa.sin_port byte-swapped to host.
_ff_asm_local_port:
    sub     rsp, 24                 ; 16 sockaddr + 4 socklen + 4 pad
    mov     dword [rsp + 16], 16    ; *salen = sizeof(sockaddr_in)

    ; getsockname(fd, &sa, &salen)
    ; rdi = fd (already)
    lea     rsi, [rsp]
    lea     rdx, [rsp + 16]
    mov     eax, SYS_getsockname
    syscall
    test    rax, rax
    js      .ret                    ; rax already -errno

    ; sa.sin_port is BE u16 at [rsp + 2]
    movzx   eax, word [rsp + 2]
    rol     ax, 8                   ; BE16 → host
    movzx   eax, ax
.ret:
    add     rsp, 24
    ret

; -----------------------------------------------------------------------------
; int _ff_asm_close(int fd)
;     sys_close(fd)
_ff_asm_close:
    ; rdi = fd already
    mov     eax, SYS_close
    syscall
    ret

; Mark stack as non-executable (Linux/binutils convention).
section .note.GNU-stack noalloc noexec nowrite progbits
