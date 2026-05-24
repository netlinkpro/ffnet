#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Integration test for `ffnet tcp-echo` with multi-worker concurrency.

Spawns the binary with --workers 4 on an ephemeral port, opens 8 parallel
TCP clients, each sends 8 KiB of pseudo-random bytes and verifies the
echo. Passes if all clients round-trip cleanly within a reasonable timeout.

Usage:  python3 tests/test_tcp_echo.py <path-to-ffnet>
"""

import os
import re
import socket
import subprocess
import sys
import threading


PAYLOAD_LEN = 8 * 1024
N_CLIENTS = 8
WORKERS = 4


def recv_exact(s: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise RuntimeError(f"eof after {len(buf)} bytes")
        buf.extend(chunk)
    return bytes(buf)


def one_client(host: str, port: int, idx: int, results: list, lock: threading.Lock) -> None:
    payload = os.urandom(PAYLOAD_LEN)
    try:
        with socket.create_connection((host, port), timeout=10) as s:
            s.sendall(payload)
            got = recv_exact(s, PAYLOAD_LEN)
            ok = (got == payload)
    except Exception as e:
        with lock:
            results[idx] = f"exception: {e}"
        return
    with lock:
        results[idx] = "ok" if ok else "mismatch"


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_tcp_echo.py <path-to-ffnet>", file=sys.stderr)
        return 2
    ffnet = sys.argv[1]

    proc = subprocess.Popen(
        [ffnet, "tcp-echo", "--listen", "127.0.0.1:0", "--workers", str(WORKERS)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        line = proc.stdout.readline()
        m = re.search(r"listening on 127\.0\.0\.1:(\d+)", line)
        if not m:
            print(f"FAIL: expected listen line, got: {line!r}", file=sys.stderr)
            return 1
        port = int(m.group(1))
        print(f"ok: bound on port {port} (workers={WORKERS})")

        results = ["?"] * N_CLIENTS
        lock = threading.Lock()
        threads = [
            threading.Thread(
                target=one_client, args=("127.0.0.1", port, i, results, lock)
            )
            for i in range(N_CLIENTS)
        ]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=15)

        fails = [i for i, r in enumerate(results) if r != "ok"]
        if fails:
            for i in fails:
                print(f"FAIL: client {i}: {results[i]}", file=sys.stderr)
            return 1
        print(f"ok: {N_CLIENTS} parallel clients round-tripped {PAYLOAD_LEN} bytes each")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
