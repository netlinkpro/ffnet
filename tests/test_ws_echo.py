#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Integration test for `ffnet ws-echo` — pure Python 3 stdlib.

Spawns the binary, parses the bound port from stdout, opens a WebSocket
connection via a minimal hand-rolled client (RFC 6455), exercises the
handshake (verifying Sec-WebSocket-Accept), text echo, and binary echo.

Usage:  python3 tests/test_ws_echo.py <path-to-ffnet>
"""

import base64
import hashlib
import os
import re
import socket
import struct
import subprocess
import sys


GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


# --- WebSocket helpers ----------------------------------------------------

def expected_accept(key: bytes) -> str:
    return base64.b64encode(hashlib.sha1(key + GUID).digest()).decode()


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise RuntimeError(f"unexpected eof after {len(buf)} bytes")
        buf.extend(chunk)
    return bytes(buf)


def handshake(sock: socket.socket, host: str, port: int, key: bytes) -> str:
    req = (
        f"GET / HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key.decode()}\r\n"
        f"Sec-WebSocket-Version: 13\r\n"
        f"\r\n"
    )
    sock.sendall(req.encode())

    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("eof during handshake")
        buf += chunk
        if len(buf) > 8192:
            raise RuntimeError("handshake response too large")
    return buf.decode("ascii", errors="replace")


def send_frame(sock: socket.socket, opcode: int, payload: bytes) -> None:
    n = len(payload)
    hdr = bytearray()
    hdr.append(0x80 | (opcode & 0x0F))                    # FIN=1
    mask = os.urandom(4)
    if n < 126:
        hdr.append(0x80 | n)
    elif n <= 0xFFFF:
        hdr.append(0x80 | 126)
        hdr.extend(struct.pack(">H", n))
    else:
        hdr.append(0x80 | 127)
        hdr.extend(struct.pack(">Q", n))
    hdr.extend(mask)
    masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
    sock.sendall(bytes(hdr) + masked)


def recv_frame(sock: socket.socket):
    h0, h1 = recv_exact(sock, 2)
    fin = bool(h0 & 0x80)
    opcode = h0 & 0x0F
    masked = bool(h1 & 0x80)
    plen = h1 & 0x7F
    if plen == 126:
        (plen,) = struct.unpack(">H", recv_exact(sock, 2))
    elif plen == 127:
        (plen,) = struct.unpack(">Q", recv_exact(sock, 8))
    mask = recv_exact(sock, 4) if masked else b""
    payload = recv_exact(sock, plen)
    if masked:
        payload = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
    return fin, opcode, payload


# --- Test driver ----------------------------------------------------------

def one_client(host: str, port: int, label: str) -> str:
    """Run a single client through handshake + text + binary echo.
    Returns 'ok' or an error string."""
    try:
        sock = socket.create_connection((host, port), timeout=10)
    except Exception as e:
        return f"connect: {e}"

    try:
        key = base64.b64encode(os.urandom(16))
        headers = handshake(sock, host, port, key)

        status_line = headers.split("\r\n", 1)[0]
        if "101" not in status_line:
            return f"bad status line: {status_line!r}"

        want = expected_accept(key)
        m = re.search(r"Sec-WebSocket-Accept:\s*(\S+)", headers, re.IGNORECASE)
        if not m or m.group(1) != want:
            return f"bad accept: want {want}, got {m and m.group(1)}"

        msg = f"hello ffnet from {label}".encode()
        send_frame(sock, 0x1, msg)
        fin, op, payload = recv_frame(sock)
        if not (fin and op == 0x1 and payload == msg):
            return f"text echo: fin={fin} op={op:#x} payload={payload!r}"

        bmsg = b"\x00\x01\x02\xff\xaa\x55" + label.encode()
        send_frame(sock, 0x2, bmsg)
        fin, op, payload = recv_frame(sock)
        if not (fin and op == 0x2 and payload == bmsg):
            return f"binary echo: fin={fin} op={op:#x} payload={payload!r}"

        return "ok"
    except Exception as e:
        return f"exception: {e}"
    finally:
        sock.close()


def run(ffnet_path: str) -> int:
    """Test in two phases: single-client baseline, then parallel multi-worker."""
    import threading

    rc = 0

    # --- Phase 1: single client, --workers 1 ---
    proc = subprocess.Popen(
        [ffnet_path, "ws-echo", "--listen", "127.0.0.1:0"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    try:
        line = proc.stdout.readline()
        m = re.search(r"listening on 127\.0\.0\.1:(\d+)", line)
        if not m:
            print(f"FAIL: expected listen line, got: {line!r}", file=sys.stderr)
            return 1
        port = int(m.group(1))
        print(f"ok: bound on port {port} (single-worker)")

        result = one_client("127.0.0.1", port, "solo")
        if result != "ok":
            print(f"FAIL: single client: {result}", file=sys.stderr)
            rc = 1
        else:
            print("ok: single-client handshake + text + binary echo")
    finally:
        proc.terminate()
        try: proc.wait(timeout=2)
        except subprocess.TimeoutExpired: proc.kill()

    if rc != 0:
        return rc

    # --- Phase 2: multi-worker, 3 parallel clients ---
    proc = subprocess.Popen(
        [ffnet_path, "ws-echo", "--listen", "127.0.0.1:0", "--workers", "2"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    try:
        line = proc.stdout.readline()
        m = re.search(r"listening on 127\.0\.0\.1:(\d+)", line)
        if not m:
            print(f"FAIL: expected listen line (mw), got: {line!r}", file=sys.stderr)
            return 1
        port = int(m.group(1))
        print(f"ok: bound on port {port} (workers=2)")

        results = [None] * 3
        threads = []
        for i in range(3):
            def runner(idx):
                results[idx] = one_client("127.0.0.1", port, f"c{idx}")
            t = threading.Thread(target=runner, args=(i,))
            threads.append(t)
            t.start()
        for t in threads:
            t.join(timeout=15)

        bad = [(i, r) for i, r in enumerate(results) if r != "ok"]
        if bad:
            for i, r in bad:
                print(f"FAIL: parallel client {i}: {r}", file=sys.stderr)
            rc = 1
        else:
            print("ok: 3 parallel clients across 2 workers")
    finally:
        proc.terminate()
        try: proc.wait(timeout=2)
        except subprocess.TimeoutExpired: proc.kill()

    return rc


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_ws_echo.py <path-to-ffnet>", file=sys.stderr)
        return 2
    return run(sys.argv[1])


if __name__ == "__main__":
    sys.exit(main())
