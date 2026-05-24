#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Run Autobahn|TestSuite against ./build/ffnet ws-echo and print a
# per-section pass/fail summary. Used by `make autobahn`.
#
# Exit status:
#   0  — script ran end-to-end (independent of Autobahn pass/fail)
#   1  — script error (missing binary, docker fail, no report produced, etc.)

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"

bin="$repo/build/ffnet"
if [[ ! -x "$bin" ]]; then
    echo "error: $bin not found; run 'make' first" >&2
    exit 1
fi

reports="$here/reports"
mkdir -p "$reports"

# --- start ws-echo on port 9001 ---
echo "==> starting ffnet ws-echo on 0.0.0.0:9001"
"$bin" ws-echo --listen 0.0.0.0:9001 >"$here/server.log" 2>&1 &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || true' EXIT

# Wait up to 5s for the "listening on" line.
for _ in {1..50}; do
    if grep -q "listening on" "$here/server.log" 2>/dev/null; then
        break
    fi
    sleep 0.1
done
if ! grep -q "listening on" "$here/server.log"; then
    echo "error: ws-echo did not bind; log:" >&2
    cat "$here/server.log" >&2
    exit 1
fi

# --- record the image digest BEFORE running so the baseline doc matches ---
image_digest=$(docker inspect --format '{{index .RepoDigests 0}}' \
    crossbario/autobahn-testsuite:latest 2>/dev/null || echo "unknown")
echo "==> autobahn image: $image_digest"

# --- run Autobahn ---
echo "==> running Autobahn fuzzingclient (this takes ~5-10 min)"
docker run --rm \
    --add-host=host.docker.internal:host-gateway \
    -v "$here:/config" \
    -v "$reports:/reports/servers" \
    crossbario/autobahn-testsuite:latest \
    wstest -m fuzzingclient -s /config/fuzzingclient.json

# --- parse the index ---
# Autobahn writes a single top-level index.json keyed by agent name:
#   { "ffnet-ws-echo": { "1.1.1": { "behavior": "OK", ... }, ... } }
index="$reports/index.json"
if [[ ! -f "$index" ]]; then
    echo "error: $index not found after run" >&2
    exit 1
fi
agent="ffnet-ws-echo"

echo
echo "==> per-section summary"
jq -r --arg agent "$agent" '
    .[$agent]
    | to_entries
    | map(.key as $case | .value.behavior as $b
            | {case: $case, section: ($case | split(".") | .[0]), behavior: $b})
    | group_by(.section)
    | map({
        section: .[0].section,
        total:   length,
        ok:      (map(select(.behavior == "OK"))            | length),
        non_strict: (map(select(.behavior == "NON-STRICT")) | length),
        informational: (map(select(.behavior == "INFORMATIONAL")) | length),
        failed:  (map(select(.behavior == "FAILED"))        | length)
      })
    | (["section","total","ok","non-strict","info","failed"] | @tsv),
      (.[] | [.section, .total, .ok, .non_strict, .informational, .failed] | @tsv)
' "$index" | column -t

echo
echo "==> FAILED case IDs"
jq -r --arg agent "$agent" \
    '.[$agent] | to_entries | map(select(.value.behavior == "FAILED") | .key) | .[]' \
    "$index" | sort -V | tr '\n' ' '
echo
echo
echo "==> NON-STRICT case IDs (informational; not counted as failure)"
jq -r --arg agent "$agent" \
    '.[$agent] | to_entries | map(select(.value.behavior == "NON-STRICT") | .key) | .[]' \
    "$index" | sort -V | tr '\n' ' '
echo

echo
echo "==> open: $reports/index.html"
