# Keypoint V2 Fresh-Process Integration Evidence

Date: 2026-07-30

Status: PASS

Classification: mounted-store interoperability and reader-behavior evidence;
not long-duration physical-profile promotion evidence

## Scope

The experiment ran five fresh processes for each of Palette's 23,287-frame
raw-v2 and refined-v2 integration handoffs. Candidate order rotated between
repetitions. All ten trials passed their absolute workload gates.

Raw and refined are separate semantic products and were not treated as
logically identical physical candidates. The comparison gate is therefore not
applicable in this checkpoint.

## Provenance

- Runner repository commit: `1c8427bbe8748d69d73dafebcc18c95b05f67285`
- Benchmark implementation commit: `d0aa35b553ba0be46f84961c44ccfc6493f1531c`
- Benchmark binary SHA-256: `f43ecb98f4586ed367ef6cac872850842f499dfe1d54908005fe0886e2a1cd6a`
- Experiment file SHA-256: `56b3cdf29d64aa0e21c9c43b88dfcc431b110ca3405f7575b6b0d8875eaa5c8f`
- Workload canonical SHA-256: `c419afa719b4d2b5c43e3fe1a0a89fe6e095c1f27024ebc14e1dcc0f63372ce2`
- Raw Palette handoff SHA-256: `cd33ac60e2f72f614a0ea5f2583d08229b9dee22d2ddb2692a56a284f4f2d8c2`
- Refined Palette handoff SHA-256: `d1c0e27303b715c95c645f077406906f691be2f9d86a74307425bb55465606b1`

The benchmark binary reported a clean worktree in every trial. The runner also
recorded a clean orchestration worktree before the evidence directory was
created.

## Median Results

| Metric | Raw v2 | Refined v2 |
| --- | ---: | ---: |
| First-presentation readiness | 339.42 ms | 918.83 ms |
| Exact repository open | 286.45 ms | 781.11 ms |
| First requested frame | 52.56 ms | 199.26 ms |
| Warm random-frame p95 | 0.156 ms | 0.187 ms |
| Forward 70-frame page p95 | 4.53 ms | 4.43 ms |
| Reverse 70-frame page p95 | 3.48 ms | 3.87 ms |
| Current-frame queue maximum | 0.101 ms | 0.119 ms |
| Process file bytes | 3.38 MiB | 3.81 MiB |
| Peak RSS | 21.02 MiB | 22.98 MiB |

Both modes had zero post-warmup deadline misses, zero stale visible frames,
zero repository read failures, and zero ordinary-playback quality payload
reads. Raw mode read the raw and body-frame offset vectors exactly once.
Refined mode read the raw, refined, and body-frame offset vectors exactly once.

## Interpretation

The first processes required roughly four seconds to open repositories; later
fresh processes were much faster. Processes were isolated, but macOS, mounted
filesystem, VPN, and server caches were not evicted or controlled. The coldest
timing is therefore recorded as process-first with external caches
uncontrolled, not as a reproducible cold-storage latency.

The fixtures become cache-resident and are too short to measure long-recording
cache pressure, sustained transfer, or physical-layout promotion. They do
establish exact-schema interoperability, retained-offset behavior, directional
prefetch, scheduler responsiveness, cancellation correctness, and quality-data
laziness across repeated fresh processes.

## Linux Portability Check

An isolated detached worktree on `ws1` at Crimson commit
`39b61e614efa3a032ef4a1b86991ede3cf92db05` configured with CUDA 12.4,
TensorRT 10.0.1.6, OpenCV 4.10.0, and CUDA architectures `80;86`. It built the
benchmark and overlay repository test. The following tests passed `3/3`:

- `keypoint_v2_long_duration_benchmark_self_test`;
- `keypoint_v2_experiment_runner_self_test`; and
- `keypoint_overlay_repository_tests`.

The check validates portable compilation, orchestration, and reverse-prefetch
behavior. It is not a cross-host performance comparison. The shared Linux
working tree and all datasets remained unchanged.

## Evidence

- `aggregate.json` SHA-256:
  `5b5c6bcdf243ec6330dea1d2059fa85952c163813c7c8c135e307e18fafa11f8`
- `summary.svg` SHA-256:
  `48a22b4f78b04715884482d22442990b8b778fc34f95fa1c570bfef1ca6b4912`
- Per-process structured JSON is retained beside the aggregate. The local run
  also produced empty stderr logs and one-line success receipts on stdout; log
  files remain excluded by the repository's diagnostics ignore policy.
