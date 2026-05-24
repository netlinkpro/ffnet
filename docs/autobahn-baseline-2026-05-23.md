# Autobahn|TestSuite baseline — ws-echo

Captured: 2026-05-23
ffnet commit: `42bc225` (pre-Phase-B; HEAD when baseline ran)
Autobahn image: `crossbario/autobahn-testsuite@sha256:519915fb568b04c9383f70a1c405ae3ff44ab9e35835b085239c258b6fac3074`
Host: shaiman (Ubuntu 24.04, Docker 28.2.1)
Command: `make autobahn`

## Summary

**181 / 301 strict pass (60.1%)** across all non-extension cases. 7 NON-STRICT
(soft warnings — RFC-compliant but suboptimal), 3 INFORMATIONAL (perf cases
with no pass/fail criterion), 110 FAILED.

Lenient pass rate (counting NON-STRICT as pass): **188 / 301 (62.5%)**.

Excluded: sections 12.x and 13.x (`permessage-deflate`; not implemented and
not planned for the 1.x slice family).

## Per-section breakdown

| Section | Topic                       | Total | OK  | Non-strict | Info | Failed |
|---------|-----------------------------|------:|----:|-----------:|-----:|-------:|
| 1       | Text / binary messages      |    16 |  16 |          0 |    0 |      0 |
| 2       | Pings / pongs               |    11 |  10 |          0 |    0 |      1 |
| 3       | Reserved bits               |     7 |   1 |          0 |    0 |      6 |
| 4       | Reserved opcodes            |    10 |   6 |          4 |    0 |      0 |
| 5       | Fragmentation               |    20 |  10 |          1 |    0 |      9 |
| 6       | UTF-8 handling              |   145 |  66 |          2 |    0 |     77 |
| 7       | Close handling              |    37 |  34 |          0 |    3 |      0 |
| 9       | Limits / performance        |    54 |  38 |          0 |    0 |     16 |
| 10      | Misc (auto-fragmentation)   |     1 |   0 |          0 |    0 |      1 |

## Failed case IDs

```
2.5
3.1 3.2 3.3 3.4 3.5 3.6
5.1 5.3 5.4 5.5 5.6 5.7 5.8 5.19 5.20
6.1.2 6.1.3
6.2.2 6.2.3 6.2.4
6.3.1
6.4.1 6.4.2
6.6.1 6.6.3 6.6.4 6.6.6 6.6.8 6.6.10
6.8.1 6.8.2
6.10.1 6.10.2 6.10.3
6.11.5
6.12.1 6.12.2 6.12.3 6.12.4 6.12.5 6.12.6 6.12.7 6.12.8
6.13.1 6.13.2 6.13.3 6.13.4 6.13.5
6.14.1 6.14.2 6.14.3 6.14.4 6.14.5 6.14.6 6.14.7 6.14.8 6.14.9 6.14.10
6.15.1
6.16.1 6.16.2 6.16.3
6.17.1 6.17.2 6.17.3 6.17.4 6.17.5
6.18.1 6.18.2 6.18.3 6.18.4 6.18.5
6.19.1 6.19.2 6.19.3 6.19.4 6.19.5
6.20.1 6.20.2 6.20.3 6.20.4 6.20.5 6.20.6 6.20.7
6.21.1 6.21.2 6.21.3 6.21.4 6.21.5 6.21.6 6.21.7 6.21.8
9.3.1 9.3.2 9.3.3 9.3.4 9.3.5 9.3.6 9.3.7 9.3.8
9.4.1 9.4.2 9.4.3 9.4.4 9.4.5 9.4.6 9.4.7 9.4.8
10.1.1
```

## Non-strict case IDs (RFC-compliant but flagged)

```
4.1.3 4.1.4 4.2.3 4.2.4   — reserved-opcode handling (we close, but Autobahn
                            prefers a specific close code; cosmetic)
5.15                       — fragmentation edge case (we reject; non-strict)
6.4.3 6.4.4                — UTF-8 (specific edge cases; non-strict)
```

## Interpretation

The 110 FAILED cases cluster cleanly by RFC 6455 section. Phase B work breaks
down roughly as follows, prioritized by failures-per-effort:

1. **Section 3 (reserved bits, 6 cases)** — trivial: reject any frame with
   `RSV1|RSV2|RSV3 != 0`. ~30 min.

2. **Section 2.5 + control-frame edge cases (~2 cases)** — reject control
   frames with payload >125 bytes or fragmented control frames. ~30 min.

3. **Section 5 (fragmentation, 9 cases) + 10.1.1** — implement
   continuation-frame handling: accumulate text/binary payload across
   FIN=0 frames, allow CONT opcodes only after a TEXT/BINARY start.
   ~half day in `ws_module.c`. Also fixes 10.1.1 (auto-fragmented echo).

4. **Section 6 (UTF-8 handling, 77 cases — the big one)** — add an
   incremental UTF-8 validator and apply it to TEXT frame payloads. Must
   be incremental because fragmented text spans frames and we should
   reject as soon as an invalid sequence is observed. ~1 day. This is
   the bulk of the slice and where most of the improvement comes from.

5. **Section 9 (limits, 16 cases — all 9.3.x and 9.4.x)** — these are
   "send a large fragmented message" cases. Likely a combination of:
   our max-frame-size limit or its absence, the fact we don't support
   fragmentation, and slow buffer growth on 1MB+ payloads. Fixing
   fragmentation (point 3) probably gets a large chunk of these for
   free; the rest may need slice 1.7's event loop to handle without
   timeouts. Defer the genuinely hard cases to 1.7.

**Estimated reachable target for Phase B alone (without slice 1.7):**
~95% strict pass rate (~285/301), with the remaining ~15 deferred to
1.7. Effort: ~2 focused days.

## Reproduction

```sh
ssh -i ~/.ssh/id_cwpi fooldev@shaiman 'cd ffnet && make autobahn'
```

The summary is regenerated from the same JSON on each run; results are
stable modulo Autobahn image updates (pinned digest above).
