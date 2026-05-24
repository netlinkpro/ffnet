# Autobahn|TestSuite — post-1.6b results

Captured: 2026-05-23
ffnet HEAD at test: `1e255c1` (slice 1.6b spec) + the implementation in the
same commit as this document.
Autobahn image: `crossbario/autobahn-testsuite@sha256:519915fb568b04c9383f70a1c405ae3ff44ab9e35835b085239c258b6fac3074`
Host: shaiman (Ubuntu 24.04, Docker 28.2.1)
Command: `make autobahn`

## Summary

**290 / 301 strict pass (96.3%)**, vs baseline 181 / 301 (60.1%) — a gain
of **+109 cases**. **Zero FAILED.** Eight NON-STRICT remain (RFC-compliant
but Autobahn prefers a different stylistic choice; see below). The 95% target
in the slice 1.6b spec is exceeded.

Lenient pass rate (counting NON-STRICT as pass): **298 / 301 (99.0%)**.

The hard §9 cases I'd flagged as needing slice 1.7's event loop **all
passed** — turns out the FrameBuf-grow + blocking-read model handles 16 MB
single messages and similarly-sized fragmented messages just fine when
Autobahn drives one connection at a time. The earlier worry was misplaced;
no event-loop work is needed for compliance, only for serving multiple
clients concurrently (still slice 1.7's job, but for a different reason).

## Per-section delta

| Section | Topic                       | Total | Before OK | After OK | Δ  | Failed |
|---------|-----------------------------|------:|----------:|---------:|---:|-------:|
| 1       | Text / binary messages      |    16 |        16 |       16 |  0 |      0 |
| 2       | Pings / pongs               |    11 |        10 |       11 | +1 |      0 |
| 3       | Reserved bits               |     7 |         1 |        6 | +5 |      0 |
| 4       | Reserved opcodes            |    10 |         6 |        6 |  0 |      0 |
| 5       | Fragmentation               |    20 |        10 |       19 | +9 |      0 |
| 6       | UTF-8 handling              |   145 |        66 |      143 | +77|      0 |
| 7       | Close handling              |    37 |        34 |       34 |  0 |      0 |
| 9       | Limits / performance        |    54 |        38 |       54 | +16|      0 |
| 10      | Misc (auto-fragmentation)   |     1 |         0 |        1 | +1 |      0 |
| **Total** |                           | **301** | **181** |  **290** | **+109** | **0** |

## NON-STRICT cases (remaining)

These are RFC-compliant — Autobahn flags them as "behavior is technically
allowed but a different one is preferred." None are failures; documented for
completeness.

```
3.2                  reserved-bit close-code preference (we close with 1002;
                     Autobahn prefers an immediate close in some forms)
4.1.3 4.1.4 4.2.3 4.2.4   reserved-opcode close timing; same flavor
5.15                 fragmentation edge: server may choose to drop the
                     mid-message control-frame buffer in a specific way
6.4.3 6.4.4          UTF-8 cases where the validator could in principle
                     reject one byte earlier than the strict standard
                     requires
```

Two of these (3.2 and 5.15) moved into NON-STRICT from FAILED in this
slice — i.e. they used to be wrong, now they're "less than optimal."
The other six were NON-STRICT in the baseline too and have not regressed.

These eight could be polished to OK in a future micro-slice if needed; not
worth the bytes right now.

## Section 7 INFORMATIONAL (unchanged)

```
7.13.1 7.13.2  9.7.x performance throughput cases (no pass/fail criterion)
... plus one other 7.x informational
```

## Verdict

The fragmentation + streaming UTF-8 validator + reserved-bits check + clean
Close-on-error combo eliminated **all 110 failures** from the baseline. The
two big surprises:

1. The §9 large-message cases that I'd predicted would need slice 1.7
   passed without any I/O changes. The simple blocking read + FrameBuf
   `realloc` pipeline handles 16 MB payloads fine.
2. Reserved-opcode polish (sending Close(1002)) was unnecessary for OK
   classification — Autobahn was already counting our bare reject as
   NON-STRICT, not FAILED. The Close just makes the behavior cleaner;
   it didn't move the score.

ws-echo at this commit is **RFC 6455-compliant** for all non-extension
test cases that Autobahn checks. Permessage-deflate (sections 12, 13) is
the only large category not covered, and it's a deliberate extension
omission for the entire 1.x slice family.

## Reproduction

```sh
ssh -i ~/.ssh/id_cwpi fooldev@shaiman 'cd ffnet && make autobahn'
```

Stable modulo Autobahn image updates (digest pinned above).
