#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Integration test for `ffnet http-serve` — pure stdlib.

Spawns the binary with a tmp --root containing a known file, then drives
it through GET / HEAD / 404 / 405 / keep-alive (multiple requests on one
connection) / Range (single range) / If-None-Match (304) / If-Modified-Since (304).

Usage:  python3 tests/test_http_serve.py <path-to-ffnet>
"""

import http.client
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time


CONTENT = b"hello ffnet http\n" * 16     # 272 bytes, repeating


def wait_listen(proc: subprocess.Popen) -> int:
    line = proc.stdout.readline()
    m = re.search(r"listening on 127\.0\.0\.1:(\d+)", line)
    if not m:
        raise RuntimeError(f"no listen line: {line!r}")
    return int(m.group(1))


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_http_serve.py <path-to-ffnet>", file=sys.stderr)
        return 2
    ffnet = sys.argv[1]

    tmp = tempfile.mkdtemp(prefix="ffnet-http-test-")
    try:
        target = os.path.join(tmp, "index.html")
        with open(target, "wb") as f:
            f.write(CONTENT)

        proc = subprocess.Popen(
            [ffnet, "http-serve", "--listen", "127.0.0.1:0", "--root", tmp],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            port = wait_listen(proc)
            print(f"ok: bound on port {port}")

            fails = 0

            # --- GET /index.html ---
            c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            c.request("GET", "/index.html")
            r = c.getresponse()
            body = r.read()
            if r.status != 200 or body != CONTENT:
                print(f"FAIL: GET 200: status={r.status} len={len(body)}", file=sys.stderr)
                fails += 1
            else:
                print(f"ok: GET /index.html -> 200, {len(body)} bytes")
            etag = r.getheader("ETag")
            last_mod = r.getheader("Last-Modified")
            c.close()

            # --- HEAD /index.html (no body) ---
            c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            c.request("HEAD", "/index.html")
            r = c.getresponse()
            body = r.read()
            if r.status != 200 or body != b"":
                print(f"FAIL: HEAD: status={r.status} body_len={len(body)}", file=sys.stderr)
                fails += 1
            else:
                cl = r.getheader("Content-Length")
                if cl != str(len(CONTENT)):
                    print(f"FAIL: HEAD Content-Length: {cl!r}", file=sys.stderr)
                    fails += 1
                else:
                    print("ok: HEAD /index.html -> 200, Content-Length present, no body")
            c.close()

            # --- 404 ---
            c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            c.request("GET", "/nope.html")
            r = c.getresponse(); r.read()
            if r.status != 404:
                print(f"FAIL: 404: status={r.status}", file=sys.stderr); fails += 1
            else:
                print("ok: GET missing -> 404")
            c.close()

            # --- 405 ---
            c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            c.request("DELETE", "/index.html")
            r = c.getresponse(); r.read()
            if r.status != 405:
                print(f"FAIL: 405: status={r.status}", file=sys.stderr); fails += 1
            else:
                print("ok: DELETE -> 405")
            c.close()

            # --- Range bytes=0-9 ---
            c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            c.request("GET", "/index.html", headers={"Range": "bytes=0-9"})
            r = c.getresponse(); body = r.read()
            if r.status != 206 or body != CONTENT[:10]:
                print(f"FAIL: Range 0-9: status={r.status} body={body!r}", file=sys.stderr)
                fails += 1
            else:
                cr = r.getheader("Content-Range")
                if cr != f"bytes 0-9/{len(CONTENT)}":
                    print(f"FAIL: Content-Range: {cr!r}", file=sys.stderr); fails += 1
                else:
                    print("ok: Range bytes=0-9 -> 206")
            c.close()

            # --- If-None-Match (with ETag from earlier) -> 304 ---
            if etag:
                c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
                c.request("GET", "/index.html", headers={"If-None-Match": etag})
                r = c.getresponse(); r.read()
                if r.status != 304:
                    print(f"FAIL: If-None-Match: status={r.status}", file=sys.stderr); fails += 1
                else:
                    print("ok: If-None-Match -> 304")
                c.close()

            # --- If-Modified-Since (use Last-Modified from earlier) -> 304 ---
            if last_mod:
                c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
                c.request("GET", "/index.html", headers={"If-Modified-Since": last_mod})
                r = c.getresponse(); r.read()
                if r.status != 304:
                    print(f"FAIL: If-Modified-Since: status={r.status}", file=sys.stderr); fails += 1
                else:
                    print("ok: If-Modified-Since -> 304")
                c.close()

            # --- Keep-alive: two requests on one TCP connection (raw) ---
            s = socket.create_connection(("127.0.0.1", port), timeout=5)
            req1 = (
                "GET /index.html HTTP/1.1\r\n"
                "Host: x\r\nConnection: keep-alive\r\n\r\n"
            )
            req2 = (
                "GET /index.html HTTP/1.1\r\n"
                "Host: x\r\nConnection: close\r\n\r\n"
            )
            s.sendall((req1 + req2).encode())
            data = b""
            while True:
                chunk = s.recv(8192)
                if not chunk: break
                data += chunk
            s.close()
            # Should see at least two "HTTP/1.1 200 OK" status lines.
            n_responses = data.count(b"HTTP/1.1 200 OK")
            if n_responses < 2:
                print(f"FAIL: keep-alive: saw {n_responses} responses", file=sys.stderr)
                fails += 1
            else:
                print(f"ok: keep-alive: {n_responses} responses on one connection")

            return 0 if fails == 0 else 1

        finally:
            proc.terminate()
            try: proc.wait(timeout=2)
            except subprocess.TimeoutExpired: proc.kill()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
