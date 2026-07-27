# Crimson Phase 5O.4 Full-Duration Stage 1 Result

Date: 2026-07-26

Status: deterministic matrix complete; correctness accepted; frozen Stage 1
gate failed; Stage 2 not authorized; physical profile not promoted

## Decision

The persisted-offset logical detection contract is consumer-validated at full
duration. All ten fresh processes selected the explicit run, used consolidated
metadata and exact dtypes without fallback probes, read and retained offsets
exactly once, produced identical direction-specific detection digests, resolved
all maintained simultaneous products, cancelled stale seeks, and traversed at
the simulated 700 FPS deadline without a miss.

The access-aware hybrid physical profile must not be promoted unchanged from
this checkpoint. It reduced median detection traversal file bytes from 3.42 MiB
to 2.07 MiB (`0.606x`) and median total file bytes from 558.15 MiB to 550.75 MiB
(`0.987x`), but the frozen traversal gate required at most `0.25x`. The full
application also failed the first-overlay and absolute peak-RSS gates. Stage 2,
which would compare 16 MiB and 64 MiB caches, therefore does not run.

This is not evidence that persisted offsets or sharding were mistakes. It is a
failed promotion result for this exact physical profile and frozen workload.

## Fixture Identity

- Palette commit: `fcdc67e764a8ddbe318cab7be19f2c3ab7f5fdb5`
- Crimson contract commit: `dadd9d779f0737c9643f15e3831a7c514bf99665`
- Contract SHA-256:
  `aa64a94de7096b6a22e53d76357a619ca92bc5296b38f0549202fd67aee36a86`
- Detection run: `crimson_storage_fixture_sleepyfish_cam2010095_v1`
- Camera frames / detection rows: `1,188,000 / 1,187,087`
- Retained offset entries / bytes: `1,188,001 / 9,504,008`
- Cache: unchanged production `64 MiB`
- Presentation policy: 70-frame pages, one asynchronous demand lead, zero
  speculative time-based read-ahead
- Storage path: mounted Johnson Lab SMB/PRFS share on macOS
- Cache classification: process-first with OS, filesystem, SMB, and server
  caches uncontrolled; no sample is described as cold

All four published evidence hashes matched the handoff before execution. The
fixtures were read only. No selector, registry, training artifact, or Palette
profile was changed.

`git show dadd9d7:docs/crimson_macos_phase5o4_full_analysis_fixture_contract.md`
hashes to the pinned `aa64a9...` value. The current working document has later
2,048-frame integration addenda and therefore is not used as the immutable
contract-content identity; those addenda do not change the frozen full-duration
workload or gates.

## Execution

Five fresh processes per layout ran in balanced order:

```text
R0 regular, R0 hybrid
R1 hybrid,  R1 regular
R2 regular, R2 hybrid
R3 hybrid,  R3 regular
R4 regular, R4 hybrid
```

Every subprocess returned success, none timed out, and all stderr logs are
empty. The matrix took 981.8 seconds. Its final nonzero driver exit reflects
the frozen reducer verdict, not a failed trial.

## Reduced Measurements

Values are medians except where marked p95. With five samples, nearest-rank p95
is the maximum observation.

| Measurement | Regular | Hybrid | Gate/result |
| --- | ---: | ---: | --- |
| required-products Ready | 76.36 s | 77.23 s | pass; hybrid `1.011x`, +0.875 s |
| Ready p95 | 80.17 s | 84.00 s | pass; every process below 180 s |
| first overlay after archive, p95 | 54.66 s | 55.42 s | fail; at most 1 s |
| first-page repository read/decode | 116.1 ms | 444.4 ms | diagnostic median |
| first-page repository read/decode, p95 | 128.4 ms | 594.3 ms | diagnostic |
| inferred first-page queue wait | 51.22 s | 51.59 s | scheduler head-of-line delay |
| retained offset read | 224.1 ms | 225.8 ms | exactly once; not a separate gate |
| offset read p95 | 419.8 ms | 272.2 ms | process-first distribution |
| detection traversal file bytes | 3.42 MiB | 2.07 MiB | fail; hybrid `0.606x`, limit `0.25x` |
| total file bytes | 558.15 MiB | 550.75 MiB | pass; hybrid `0.987x`, limit `1.05x` |
| post-warmup traversal misses | 0 / 490 | 0 / 490 | pass |
| seek cancellation p95 | 82.0 ms | 226.6 ms | pass; at most 250 ms |
| post-cancel bytes/seek p95 | 0.333 MiB | 0.127 MiB | pass; at most 1 MiB |
| peak RSS median | 2,282.95 MiB | 1,732.91 MiB | relative gate passes |
| peak RSS maximum | 2,366.61 MiB | 2,319.30 MiB | fail; every process at most 2 GiB |
| shutdown p95 | 36.3 ms | 1.1 ms | pass; at most 2 s |

TensorStore file-kvstore bytes are file-driver range bytes, not observed SMB
wire traffic. Detection-only seek and traversal deltas were recorded after the
shared scheduler became idle and only canonical-detection requests were issued.
The counters remain process-global TensorStore metrics, which is stated in each
trial.

## Detection Verdict

The logical contract passes:

- `frame_row_offsets` provides constant-time frame-to-row lookup over the
  million-row run with a modest 9.5 MiB retained index;
- offsets were read exactly once in every adapter lifetime;
- all exact schema, codec, consolidated-metadata, and decoded-value checks
  passed;
- the three UI fields overlapped in flight;
- forward and reverse traversal had zero post-warmup misses at 700 FPS;
- rapid seeking produced no stale publication; and
- hybrid substantially reduced cancellation waste.

The unchanged hybrid physical profile does not pass promotion. It saved 39.4%
of median detection traversal bytes, not the required 75%, and its measured
first-page read/decode and seek-settle latency were higher. The absolute
traversal payloads are already small, so a follow-up should compare absolute
latency and byte budgets rather than silently rewriting this frozen ratio.

A storage follow-up may compare 64, 128, and 256 KiB windowed inner chunks plus
a row/time-aligned candidate. It must retain request-count, shard-index,
random-frame p95, cancellation-waste, 700 FPS deadline, and absolute-byte gates.
No production writer profile changes on this result.

## Application Verdict

Required-product Ready passed comfortably and hybrid added only 1.1% to median
Ready time. The first detection page itself was not the source of the roughly
52-second first-overlay delay. When the request was issued, all four shared
workers were occupied by non-preemptive initialization jobs. Subtracting the
repository read/decode maximum leaves about 51-52 seconds of inferred queue
wait in both layouts.

The next application change should prevent current-frame presentation work
from waiting behind long repository initialization. A dedicated demand lane,
separate initialization/runtime schedulers, or an explicit reserved worker are
candidate designs. Merely adding more workers should not be accepted without a
bounded concurrency and memory argument.

Peak RSS also requires attribution before another promotion run. The configured
TensorStore cache accounts for only 64 MiB. Full-duration trace preload,
repository indexes, transient decoded arrays, presentation caches, in-flight
reads, and allocator retention account for the rest. Record current and peak
RSS by phase and retained bytes by product before changing the frozen 2 GiB
limit.

## Evidence

Structured reducer and environment identity:

```text
docs/diagnostics/crimson_macos_phase5o4_full_duration_stage1_2026-07-26/aggregate.json
```

The same directory contains ten per-process JSON documents plus stdout and
stderr logs. The benchmark implementation is:

```text
tools/canonical_detection_full_archive_stage1_benchmark.cpp
tools/run_canonical_detection_full_archive_stage1.py
```

The earlier 2,048-frame regular/hybrid integration and Metal GUI smokes remain
the compatibility evidence. A new full-duration GUI acceptance smoke was not
run because the frozen headless pair was not accepted.
