# Keypoint V2 Full-Duration Mounted Evidence

Date: 2026-07-30

Status: PASS

Classification: full-duration refined-keypoint consumer and performance gate;
not a physical-profile comparison

## Scope

Five fresh processes exercised Palette's selector-ineligible Sleepyfish v8
fixture over 1,188,000 camera frames and 1,169,010 keypoint rows. The refined
presentation candidate opened four exact products:

- raw keypoint observations v2;
- raw-bound keypoint quality v1;
- refined keypoint observations v2; and
- the body-frame v1 snapshot derived from those refined observations.

The package contains only a refined-derived body frame. Raw-only presentation
was therefore not run with an invalid body-frame lineage.

## Provenance

- Palette fixture commit: `8fd810fd8f5ba5c7bc9dcc000ee9b4b90b4af342`
- Palette handoff SHA-256:
  `86e638ed977f71a69bdd4fd334d7778583226901e7acfc22f790607e8b6fc374`
- Palette handoff payload digest:
  `1089386cce3e94ab368203c3e9e70ebb0fee956860405a37f33a7d35dd4c5ec0`
- Crimson benchmark implementation:
  `72ddcd1109078bf97c7e0063bfe9aa16abb63d79`
- Crimson orchestration commit:
  `e055ce87815a4bceefd695d463fd0cd13eac8d56`
- Workload canonical SHA-256:
  `c419afa719b4d2b5c43e3fe1a0a89fe6e095c1f27024ebc14e1dcc0f63372ce2`

The handoff file SHA and its canonical payload digest were independently
recomputed before the run. Every subprocess reported the expected clean
Crimson implementation revision.

## Results

| Metric | Median | Process-first maximum |
| --- | ---: | ---: |
| First-presentation readiness | 594.37 ms | 6,398.32 ms |
| Exact repository open | 589.33 ms | 6,017.85 ms |
| First requested frame | 36.04 ms | 379.59 ms |
| First random-frame p95 | 2.62 ms | 169.94 ms |
| Warm random-frame p95 | 1.90 ms | 15.42 ms |
| Forward 70-frame page p95 | 5.06 ms | 6.04 ms |
| Reverse 70-frame page p95 | 4.47 ms | 5.25 ms |
| Current-frame queue maximum | 0.105 ms | 1.92 ms |
| Process file bytes | 607.90 MiB | 655.94 MiB |
| Peak RSS | 253.50 MiB | 303.75 MiB |
| Close | 0.084 ms | 0.088 ms |

Every process had:

- zero post-warmup traversal deadline misses;
- zero stale visible frames;
- zero repository read failures;
- zero ordinary-playback quality payload reads;
- one raw, refined, and body-frame offset-vector read; and
- no quality offset-vector read.

The three retained offset vectors consumed 28,512,024 bytes. The four archive
contexts declared a combined 256 MiB TensorStore cache limit; this is a limit,
not eager resident allocation.

## Interpretation

The first process paid the highest mounted-filesystem metadata and range-read
latency. Later fresh processes benefited from uncontrolled macOS, SMB, VPN,
and server caches. These results are therefore process-first distributions,
not controlled cold-cache measurements.

Unlike the earlier 23,287-frame fixture, this workload did not fit wholly in
cache: median process transfer was about 608 MiB and the repeated random pass
continued to cause bounded reads and evictions. Despite that pressure, current
frame queueing stayed negligible, traversal remained far inside the 100 ms
page deadline, and memory remained below the frozen 768 MiB gate.

This accepts the current full-duration refined-keypoint consumer strategy. It
does not compare two physical keypoint layouts and therefore does not select or
promote a Palette storage profile.

## Evidence

- `aggregate.json` SHA-256:
  `a00ca0240714219648dd01b8e1e9a40a34d55132daeab7f2f09fdcc328969ace`
- `summary.svg` SHA-256:
  `1881ecda93bdf9550c2872125c8a6abe57ba971ce08b3ed40f036c3ccde3d2b6`
- Per-process structured JSON is retained beside the aggregate.
