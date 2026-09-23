# Refined-Detection Physical-Profile Canary

Date: 2026-07-27

Status: consumer gate passed; access-aware promotion recommended to Palette

## Verdict

Crimson accepts Palette's access-aware refined-detection physical profile for
a separate versioned Palette promotion. All frozen correctness, readiness,
current-frame, cancellation, 700 FPS deadline, memory, transfer, and shutdown
gates passed. Crimson did not mutate either archive and does not set
`profile_promoted=true`.

The evidence is bound to clean Crimson implementation commit
`9cf04acee9682a6f4f5fae005c0af6077ec5cc4b` and Palette implementation commit
`a94abaea1206ae232b65185d1c98f250499af45b`.

The accepted candidate declaration is
`published_http_v1__benchmark_sharded__chunk_131072__eager_chunk_1048576__shard_8388608`:
128 KiB windowed/indexed inner chunks, 1 MiB eager offset chunks, and 8 MiB
outer shards. Palette should assign its production versioned identity without
retuning those values. The canary reduced payload objects from 220 to 42
(`5.24x` fewer).

## Results

Values are medians across five fresh processes per layout unless marked p95.

| Metric | Regular | Access-aware | Interpretation |
| --- | ---: | ---: | --- |
| detection readiness | 462.0 ms | 300.2 ms | 35.0% lower |
| current-frame p95 | 144.2 ms | 48.0 ms | 66.7% lower |
| retained-offset read | 107.0 ms | 103.4 ms | effectively unchanged |
| whole-process file bytes | 16.95 MiB | 5.33 MiB | 68.5% lower |
| 7,000-frame traversal bytes | 4.65 MiB | 0.61 MiB | 86.8% lower |
| traversal file reads | 14 | 21 | more, substantially smaller ranges |
| seek-settlement cross-process p95 | 235.5 ms | 70.6 ms | both pass 250 ms |
| peak RSS | 89.13 MiB | 55.03 MiB | 34.09 MiB lower |
| post-warmup deadline misses | 0 / 400 | 0 / 400 | both pass |

All paired forward/reverse traversal digests matched. Every process used one
retained offset read, 11 exact instance-side handles, zero source-audit
handles, a 64 MiB TensorStore cache, and zero stale publications or failed
work. Access-aware transferred `0.13195x` as many traversal bytes and
`0.31461x` as many whole-process bytes as regular.

The access-aware layout uses more physical reads because TensorStore fetches
smaller independently addressable inner chunks from indexed shards. The much
lower byte count, lower observed latency, and zero missed deadlines establish
that this is beneficial on the mounted Mac path rather than harmful request
fanout.

## Environment

- macOS 26.3.1, build 25D2128, arm64
- mounted path: `//delahantyj@prfs.hhmi.org/johnsonlab`
- mount type: `smbfs`
- TensorStore cache: 64 MiB
- process order: alternating regular/access-aware across five matched pairs
- cache classification: process-first; macOS, SMB, and server caches
  uncontrolled
- no time-based read-ahead and no UI-column residency

TensorStore `file_bytes` measures file-kvstore ranges returned to the process.
It is not a measurement of complete SMB wire traffic.

## Evidence

- `aggregate.json` contains the frozen limits, process order, environment,
  structured summaries, every gate, and SHA-256 for all ten trial files.
- `00_...json` through `09_...json` are the raw per-process results.
- `refined_detection_physical_profile_comparison.png` plots the five samples
  per layout.

Evidence hashes:

```text
benchmark executable  cc026c9b4a5e7b1b33740beffc94a64c5a3ac432b46470c9c4431c8911c737c8
aggregate.json        0be6f191b0d684914cdd48bc938267cd8bb2fb6e066e158ba65cf2339d466d32
comparison plot       2d4186c2208bd131b652e59ba03487aa98a9c3dafe80345445e0761c95164676
canary manifest       8d9215aa29bf4b0787e50114e9fb429f959194ee3a7f1bdea6fd1d04ae1424b6
canary payload        2c00649c378c7a33f5621c4cd91ad787db46dcd0a512d6391ece92b383cd5609
```

## Boundary

This is a detection-isolated physical-profile gate over the complete
full-duration refined snapshot. It validates the production backend-neutral
repository, buffer, scheduler, and TensorStore path, but it is not a new
whole-application simultaneous-stream benchmark. The earlier full-application
gates remain the evidence for mask, keypoint, timeline, video, and detection
interference.
