#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Integration test for `ffnet http-echo` — pure stdlib.

POSTs with Content-Length and with chunked Transfer-Encoding; asserts
the response body equals the request body.

Usage:  python3 tests/test_http_echo.py <path-to-ffnet>
"""

import os
import re
import socket
import subprocess
import sys


PAYLOAD = b"slice 2 http echo body test " * 50   # 1400 bytes


def recv_response(s: socket.socket) -> tuple[int, dict, bytes]:
    """Read until either the response body matches Content-Length, or the
    connection closes. Returns (status, headers, body). Uses a simple
    parser sufficient for our echo server's responses."""
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = s.recv(4096)
        if not chunk:
            return (0, {}, data)
        data += chunk
    head, body = data.split(b"\r\n\r\n", 1)
    lines = head.split(b"\r\n")
    m = re.match(rb"HTTP/1\.1 (\d+) ", lines[0])
    status = int(m.group(1)) if m else 0
    headers: dict[str, str] = {}
    for line in lines[1:]:
        k, _, v = line.partition(b": ")
        headers[k.decode().lower()] = v.decode()
    cl = int(headers.get("content-length", "0"))
    while len(body) < cl:
        chunk = s.recv(4096)
        if not chunk: break
        body += chunk
    return (status, headers, body[:cl])


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_http_echo.py <path-to-ffnet>", file=sys.stderr)
        return 2
    ffnet = sys.argv[1]

    proc = subprocess.Popen(
        [ffnet, "http-echo", "--listen", "127.0.0.1:0"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    try:
        line = proc.stdout.readline()
        m = re.search(r"listening on 127\.0\.0\.1:(\d+)", line)
        if not m:
            print(f"FAIL: no listen line: {line!r}", file=sys.stderr)
            return 1
        port = int(m.group(1))
        print(f"ok: bound on port {port}")

        fails = 0

        # --- POST with Content-Length ---
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        req = (
            f"POST / HTTP/1.1\r\nHost: x\r\n"
            f"Content-Length: {len(PAYLOAD)}\r\nConnection: close\r\n\r\n"
        ).encode() + PAYLOAD
        s.sendall(req)
        status, headers, body = recv_response(s)
        s.close()
        if status != 200 or body != PAYLOAD:
            print(f"FAIL: Content-Length echo: status={status} body_len={len(body)}",
                  file=sys.stderr)
            fails += 1
        else:
            print(f"ok: POST Content-Length: echoed {len(body)} bytes")

        # --- POST with chunked Transfer-Encoding ---
        # Split payload into 3 chunks of varying size.
        chunks = [PAYLOAD[:500], PAYLOAD[500:1100], PAYLOAD[1100:]]
        chunk_body = b""
        for ch in chunks:
            chunk_body += f"{len(ch):X}\r\n".encode() + ch + b"\r\n"
        chunk_body += b"0\r\n\r\n"
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        req = (
            "POST / HTTP/1.1\r\nHost: x\r\n"
            "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
        ).encode() + chunk_body
        s.sendall(req)
        status, headers, body = recv_response(s)
        s.close()
        if status != 200 or body != PAYLOAD:
            print(f"FAIL: chunked echo: status={status} body_len={len(body)} (want {len(PAYLOAD)})",
                  file=sys.stderr)
            fails += 1
        else:
            print(f"ok: POST chunked: echoed {len(body)} bytes")

        return 0 if fails == 0 else 1
    finally:
        proc.terminate()
        try: proc.wait(timeout=2)
        except subprocess.TimeoutExpired: proc.kill()


if __name__ == "__main__":
    sys.exit(main())
