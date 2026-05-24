# Developer Instructions: Lightweight Instructional Assembly WebSocket Library

## 1. Project Overview & Vision
The goal of this project is to build a minimal, lightweight, and highly performant WebSocket library written entirely in Assembly (x86_64, targeting Linux). This library is designed primarily as an **instructional example** for advanced system programming and Assembly language mastery. 

To serve as an effective teaching tool, the codebase must balance optimal, low-level execution with extreme structural clarity. It must avoid "clever" unreadable hacks in favor of clean, modular routines, explicit register management, and rigorous documentation.

### Core Objectives:
* **Minimalist Design:** Depend strictly on native Linux system calls (`sys_socket`, `sys_accept`, `sys_read`, `sys_write`, etc.). No external C library dependencies (`glibc` is prohibited).
* **Performance:** Zero-copy or minimal-copy buffering, direct register usage for state-machine transitions, and optimized bit manipulation for WebSocket framing.
* **Educational Clarity:** Every routine must map cleanly to a single architectural or protocol concept (e.g., TCP handshaking, HTTP parsing, WebSocket frame unmasking).

---

## 2. Target Architecture & Environment
* **Architecture:** x86_64 Intel/AMD
* **Operating System:** Linux (Kernel 5.4+)
* **Assembler:** NASM (Netwide Assembler), ELF64 output format
* **Calling Convention:** System V AMD64 ABI
    * **Arguments:** `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`
    * **Return value:** `rax`
    * **Preserved registers (callee-saved):** `rbx`, `rsp`, `rbp`, `r12`, `r13`, `r14`, `r15`
    * **Scratch registers (caller-saved):** `rax`, `rcx`, `rdx`, `rsi`, `rdi`, `r8`, `r9`, `r10`, `r11`

---

## 3. Library Architecture & Components

The library must be divided into distinct, logically decoupled modules to facilitate modular learning.

### A. Network & Socket Layer (`src/network.asm`)
Responsible for establishing the raw TCP connection using native Linux syscall numbers (`rax`).
* `socket_create`: Initializes a non-blocking or blocking IPv4 TCP socket (`sys_socket`).
* `socket_bind_listen`: Binds to a port and initializes the listening queue (`sys_bind`, `sys_listen`).
* `socket_accept`: Blocks/waits for an incoming connection (`sys_accept`), returning the client file descriptor.

### B. HTTP Handshake Parser (`src/handshake.asm`)
When a client connects, they issue a standard HTTP GET upgrade request. This layer processes that raw text.
* `parse_http_request`: Scans the incoming buffer for the `Sec-WebSocket-Key:` header.
* `generate_accept_key`: Implements the WebSocket handshaking algorithm:
    1. Concatenate the extracted key string with the magic GUID: `"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"`.
    2. Compute the SHA-1 hash of the combined string.
    3. Base64-encode the resulting 20-byte SHA-1 digest.
* `send_http_response`: Constructs the standard HTTP `101 Switching Protocols` response with the encoded key and transmits it via `sys_write`.

> *Note for Implementation:* The SHA-1 and Base64 algorithms must be written in clean, unrolled/looped assembly macros or subroutines within a helper file (`src/crypto.asm`), maintaining visibility into data movement.

### C. WebSocket Protocol Layer (`src/ws_frame.asm`)
Once upgraded, data is transmitted in binary WebSocket frames. This module handles framing compliance.
* `ws_read_frame`: Parses the incoming byte stream according to RFC 6455.
    * Extract `FIN` bit, `Opcode` (Text: `0x1`, Binary: `0x2`, Close: `0x8`, Ping: `0x9`, Pong: `0xA`).
    * Parse Payload Length (handles 7-bit, 16-bit, and 64-bit length variations).
    * Extract the 4-byte Masking Key (mandatory for client-to-server frames).
* `ws_unmask_payload`: Performs the XOR unmasking transformation on the payload bytes.
* `ws_write_frame`: Encodes raw data into a server-to-client frame (servers *must not* mask frames sent to clients) and sends via `sys_write`.

---

## 4. Coding & Documentation Standards

Because this project is a teaching tool, compliance with the following styling rules is mandatory.

### 1. Mandatory Commenting Blueprint
Every subroutine header must include a structured block detailing its inputs, outputs, corrupted registers, and architectural behavior.