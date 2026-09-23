# Crimson Phase 5O Endurance Acceptance

Date: 2026-07-29

Status: implementation and first mounted full-duration checkpoint complete.

## Purpose

This checkpoint tests whether Crimson's current bounded data-access policies
remain bounded under repeated use. It deliberately does not change the schema,
coordinates, selection, or storage representation of keypoints, masks, subject
shape, eye geometry, or timelines while those contracts are being stabilized.

The workload is headless. It reuses the production-shaped full-archive loader
and the same repositories used by the existing Stage 1 gate. Metal, CUDA,
OpenGL, ImGui, and video decoding are outside this measurement. The portable
workload planner and plateau classifier live in `src/endurance_workload.*` and
compile on every supported backend.

## Workload

Endurance mode is opt-in on
`canonical_detection_full_archive_stage1_benchmark`. Each cycle:

1. resolves deterministic random current frames concurrently across canonical
   detections, keypoints, subject masks, subject shape, eye geometry, crop
   geometry, motion, eye angles, and tail kinematics;
2. issues a rapid canonical-detection seek burst and rejects stale publication;
3. traverses a page-aligned 3,500-frame span at the simulated 700 FPS page
   deadline;
4. alternates forward and reverse direction;
5. moves traversal spans across the complete recording domain; and
6. waits for scheduled work to settle before recording physical I/O, scheduler,
   cache, RSS, and repository-owned retained-memory evidence.

The frame schedule uses a fixed SplitMix64 sequence instead of an implementation-
defined standard-library random engine. A given configuration therefore emits
the same plan on macOS, Linux, and Windows.

The runner accepts:

```text
--endurance-cycles N
--endurance-traversal-frames N
--endurance-probes-per-cycle N
--endurance-seeks-per-cycle N
--endurance-seed N
```

At least ten cycles are required. A six-cycle sensitivity trial showed that
four post-warmup points made the fitted RSS slope depend too heavily on one
late allocator/TensorStore fluctuation. Setting the traversal span to the
complete camera-frame count enables a literal full-recording traversal in each
cycle; shorter spans provide a faster whole-domain checkpoint.

## Memory Verdict

Cycle-end process RSS and the sum of repository-reported retained lower bounds
are evaluated separately. The first two cycles are warmup. The remaining
samples must contain at least four observations and a positive time interval.

The initial and final values are two-sample medians. A least-squares slope is
reported in bytes per minute. The first diagnostic gate is:

| Metric | Process RSS | Reported retained bytes |
| --- | ---: | ---: |
| Maximum final-window growth | 256 MiB | 64 MiB |
| Maximum peak growth | 512 MiB | 128 MiB |
| Maximum fitted slope | 64 MiB/min | 32 MiB/min |

These are coarse leak-detection limits, not final cache budgets. A pass means
the observed working set plateaued within those limits. It does not prove every
RSS byte has an owner. Repository-reported memory remains a lower bound because
TensorStore internals, allocators, libraries, and thread stacks are incomplete.

The workload also requires:

- zero stale seek publications;
- zero new scheduler failures or callback exceptions;
- no repeated canonical `frame_row_offsets` read;
- deterministic settlement of every simultaneous probe; and
- a bounded application cache accounting snapshot for detections and masks.

## Current Boundary

The macOS application currently combines shared-scheduler buffers with older
per-product workers. This checkpoint calls stable repository interfaces
concurrently and records the shared detection scheduler without forcing
evolving products through a new adapter. As those adapters converge, the same
workload and evidence schema can move outward to the production buffers.

Archive reload is measured as a separate fresh-process repetition so process,
TensorStore, and repository state cannot leak across trials. Close while work is
pending remains covered by deterministic buffer/scheduler tests and the native
residency close smoke; it is not inferred from a settled endurance cycle.

## Invocation

For the full Sleepyfish access-aware archive, a ten-cycle checkpoint is:

```bash
build/macos-arm64-release/canonical_detection_full_archive_stage1_benchmark \
  ARCHIVE.zarr \
  DETECTION_RUN \
  VIDEO.mp4 \
  hybrid \
  0 \
  resident \
  endurance.json \
  --endurance-cycles 10 \
  --endurance-traversal-frames 3500 \
  --endurance-probes-per-cycle 2 \
  --endurance-seeks-per-cycle 8 \
  --endurance-seed 20260729
```

Plot the structured evidence with:

```bash
python tools/plot_analysis_endurance.py endurance.json endurance.png
```

The first mounted result is a diagnostic acceptance checkpoint. A later release
gate should use multiple fresh processes and include native Windows execution.
