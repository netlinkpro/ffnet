#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Integration test for `ffnet ws-echo`.

Spawns the binary, parses the bound port from stdout, opens a WebSocket
client via the `websockets` library, exercises text echo and binary echo,
then shuts the server down.

Usage:  python3 tests/test_ws_echo.py <path-to-ffnet>
"""

import asyncio
import re
import subprocess
import sys

try:
    import websockets
except ImportError:
    print("test_ws_echo.py: requires `pip install websockets`", file=sys.stderr)
    sys.exit(2)


async def run(ffnet_path: str) -> int:
    proc = subprocess.Popen(
        [ffnet_path, "ws-echo", "--listen", "127.0.0.1:0"],
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
        print(f"ok: bound on port {port}")

        async with websockets.connect(f"ws://127.0.0.1:{port}") as ws:
            await ws.send("hello ffnet")
            got = await ws.recv()
            if got != "hello ffnet":
                print(f"FAIL: text echo got {got!r}", file=sys.stderr)
                return 1
            print("ok: text echo")

            await ws.send(b"\x00\x01\x02\xff")
            got = await ws.recv()
            if got != b"\x00\x01\x02\xff":
                print(f"FAIL: binary echo got {got!r}", file=sys.stderr)
                return 1
            print("ok: binary echo")

        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_ws_echo.py <path-to-ffnet>", file=sys.stderr)
        return 2
    return asyncio.run(run(sys.argv[1]))


if __name__ == "__main__":
    sys.exit(main())
