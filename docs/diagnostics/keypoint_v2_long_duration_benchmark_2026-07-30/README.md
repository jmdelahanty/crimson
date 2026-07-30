# Keypoint V2 Long-Duration Harness Integration Checkpoint

Date: 2026-07-30

Status: PASS for raw and refined integration fixtures; not full-duration
physical-profile evidence

## Identity

- Crimson implementation commit:
  `d0aa35b553ba0be46f84961c44ccfc6493f1531c`
- Worktree recorded by both processes: clean
- Frozen workload file SHA-256:
  `26da2cadf14bae3df42414586234eddcc47486bcfdfb9d3592a96d7d5063ad99`
- Frozen workload canonical-JSON SHA-256:
  `c419afa719b4d2b5c43e3fe1a0a89fe6e095c1f27024ebc14e1dcc0f63372ce2`
- Raw result SHA-256:
  `e7d51b6fd73cdf71e5ed11ca3cd710a1158060d2696ac41a10f0f9414c952ece`
- Refined result SHA-256:
  `2bf0855cf40cc0af1bc349d723995a3aab94b7a025e062bf48f3844c2a865bbd`

The raw fixture is Palette's immutable
`20260128_cropv2_keypoint_v2_20260729_v4` handoff. The refined fixture is
`20260128_cropv2_keypoint_refined_v2_20260729_v2`. Their handoff SHA-256 values
are recorded in `docs/crimson_keypoint_v2_consumer.md`.

## Result

| Metric | Raw | Refined |
| --- | ---: | ---: |
| Archive-context open | 157.0 ms | 31.0 ms |
| Exact repository open | 3,646.7 ms | 3,620.6 ms |
| First-presentation readiness | 4,157.1 ms | 4,085.5 ms |
| First page service | 352.8 ms | 433.6 ms |
| First page physical bytes | 2,707,879 | 2,708,039 |
| Warm random p95 | 0.209 ms | 0.250 ms |
| Forward 70-frame page p95 | 4.130 ms | 9.647 ms |
| Reverse 70-frame page p95 | 3.372 ms | 6.722 ms |
| Current-frame queue maximum | 0.130 ms | 0.450 ms |
| Retained offset bytes | 372,608 | 558,912 |
| Configured cache limit | 192 MiB | 256 MiB |
| Peak process RSS | 20.6 MiB | 22.7 MiB |
| Deadline miss ratio | 0 | 0 |
| Stale visible frames | 0 | 0 |
| Ordinary quality payload reads | 0 | 0 |
| Close | 0.038 ms | 0.062 ms |

All frozen gates passed. Exact open used no metadata or dtype fallback. Raw,
selected, and body-frame offset indexes followed the one-read policy; the
quality offset and quality payload arrays remained unopened during ordinary
presentation.

The raw package uses three unique archives and the refined package uses four.
Each `ArchiveContext` configures a 64 MiB TensorStore cache pool, hence the
reported 192/256 MiB aggregate limits. These are maximum configured capacities,
not eager allocations; measured process RSS remained about 21--23 MiB.

## Interpretation

The fixtures contain 23,287 frames and 22,926 rows. The first deterministic
random pass brings their small payloads into TensorStore's caches, so both
forward and reverse traversals subsequently report zero physical file bytes.
That is expected and validates cache reuse, but it cannot characterize
full-recording eviction, transfer, or storage layout.

The harness is ready for Palette's full-duration raw/refined handoff. Those
stores must be run as five balanced fresh-process repetitions. A source-matched
GUI smoke follows only after the headless full-duration gate. No selector,
writer, archive, or production storage profile changed at this checkpoint.
