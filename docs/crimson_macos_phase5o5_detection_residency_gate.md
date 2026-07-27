# Crimson Phase 5O.5 Detection Residency Strategy Gate

Date: 2026-07-26

Contract version: 1

Status: complete; isolated and full-archive gates passed

## Result

The byte-budgeted residency strategy passed both frozen checkpoints on
2026-07-26. This authorizes a separately reviewed production policy; it does
not enable residency automatically and does not promote a Palette physical
storage profile.

The 20-process isolated comparison passed every correctness, latency, I/O,
memory, cancellation, and playback gate. Evidence is in:

```text
docs/diagnostics/canonical_detection_residency_strategy_2026-07-26/
```

The ten-process hybrid full-archive interference comparison also passed. Its
five paged and five resident processes used balanced order with uncontrolled
OS, SMB, and server caches. Evidence is in:

```text
docs/diagnostics/canonical_detection_full_archive_residency_2026-07-26/
```

Median required-product readiness was 71.00 seconds paged and 69.35 seconds
resident. No maintained product regressed by 10%, median resident peak RSS was
81.3 MiB lower rather than higher, and both modes had zero post-warmup playback
deadline misses. Resident construction took 12.35 seconds median while the
other repositories initialized.

Median current-frame request-to-publication latency during construction was
514.1 ms paged and 561.7 ms resident: a 9.25% and 47.6 ms regression, within
the pre-execution 10% and 250 ms noise limits. Resident samples nevertheless
included a 2.99-second maximum. Production review must therefore keep paging
available throughout construction and must not treat an active TensorStore
read as preemptible.

The fixture run is a canonical `detect_runs` surface declaring `stage: detect`.
These results validate canonical detection storage and access, not the separate
production selection rule that should prefer explicitly selected refined or
corrected detections when available.

## Decision

This contract supersedes the publication authorization in commit `aa346a1`.
Palette must pause the 25-store detection-layout matrix. Crimson first compares
bounded 70-frame paging with a first-page-then-background-resident UI strategy
using the already published full-duration regular and 128 KiB hybrid fixtures.

Palette storage remains pageable. Residency is a Crimson consumer optimization
selected from decoded byte size, not camera-frame count. All eight canonical
columns remain independently pageable for inspection, export, refinement, and
training consumers.

## Resident Surface

The resident hot set contains exactly:

- `instances/bbox_norm_coords` as `float32 (N,4)`;
- `instances/scores` as `float32 (N,)`; and
- `instances/class_ids` as `int32 (N,)`.

The already retained `int64 frame_row_offsets` index is shared with both access
strategies and is not copied into the resident snapshot. For the current
fixture, UI columns occupy 28,490,088 decoded bytes and offsets occupy 9,504,008
bytes, for 37,994,096 bytes total.

The resident transition is:

1. Open exact handles and read offsets once.
2. Resolve and publish the first 70-frame page through ordinary paging.
3. If the exact decoded UI bytes fit the configured resident budget, allocate
   the final three vectors.
4. Fill them through bounded speculative ranges on a separate scheduler source.
5. Validate every decoded value and final array extent.
6. Atomically publish one immutable snapshot.
7. Serve later repository ranges from the snapshot without TensorStore reads.

Paging remains available until step 6 succeeds. A partial snapshot is never
visible. Reload, close, failure, or explicit cancellation discards the builder
and publishes no stale resident state. Current-frame demand has workers and
priority independent of the speculative source and can overtake pending chunks.

## Existing Fixtures

Use the immutable pair at:

```text
/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/
canonical_detection_storage/full_analysis/sleepyfish_cam2010095_v1/
```

Leaves are `regular.zarr` and `hybrid.zarr`. The explicit detection run is:

```text
crimson_storage_fixture_sleepyfish_cam2010095_v1
```

No Palette archive, profile, writer, registry, selector, or fixture changes are
part of this gate.

## Frozen Process Matrix

There are 20 fresh detection-isolated processes:

```text
2 layouts x 2 consumer strategies x 5 repetitions
```

Condition IDs are `hybrid_paged`, `hybrid_resident`, `regular_paged`, and
`regular_resident`. Process order is:

| Repetition | First | Second | Third | Fourth |
| ---: | --- | --- | --- | --- |
| 0 | hybrid_paged | hybrid_resident | regular_paged | regular_resident |
| 1 | hybrid_resident | regular_resident | hybrid_paged | regular_paged |
| 2 | regular_paged | hybrid_paged | regular_resident | hybrid_resident |
| 3 | regular_resident | regular_paged | hybrid_resident | hybrid_paged |
| 4 | hybrid_paged | regular_paged | regular_resident | hybrid_resident |

Every process uses the production 64 MiB TensorStore cache, one retained offset
read, zero time-based speculative read-ahead, one 70-frame demand-lead page,
and the deterministic selections from
`docs/reference/crimson_canonical_detection_layout_matrix_workload_v1.json`.
OS, SMB, and server caches are uncontrolled and must not be called cold.

## Detection-Isolated Phases

Each process:

1. opens the exact run and records repository/offset initialization;
2. resolves the first page before starting any resident work;
3. in resident mode, schedules bounded chunks and records time, physical I/O,
   queueing, temporary memory, cancellation, and atomic publication;
4. traverses the repetition's 3,500-frame region in exactly 50 aligned
   70-frame pages at a simulated 700 FPS deadline;
5. executes the 120 UI random frames;
6. verifies one offset read and exact decoded digest; and
7. cancels, closes, and records bounded shutdown.

Paged mode runs the identical frame selections without starting residency.
Resident mode reports random and traversal phases only after the snapshot is
ready. A separate during-preload demand probe proves that current-frame demand
does not wait behind the complete bulk load.

## Full-Archive Interference Check

If the isolated resident strategy passes, run ten additional fresh hybrid
full-archive processes: five paged and five resident. The resident builder
starts after the first detection page while masks, keypoints, shapes, eye
geometry, crop geometry, and maintained timelines initialize normally.

This phase reports required-products Ready time, each product's initialization
time, first detection presentation, scheduler queue delay, playback deadlines,
resident completion, file bytes, and peak RSS. Scheduler starvation is a
Crimson failure, not a Palette physical-layout failure.

## Frozen Gates

Correctness gates are absolute:

- exact paged/resident values and traversal digest;
- exactly one offset read;
- no fallback dtype or legacy-path probes;
- zero stale resident or page publications;
- no repository, scheduler, or decode failures; and
- one complete atomic resident publication in admitted resident processes.

Performance and resource gates are:

| Evidence | Limit |
| --- | ---: |
| first-page repository-service p95 | at most 150 ms |
| resident first-page median regression from paired paged mode | at most 10% and 25 ms |
| complete resident preload cross-process p95 | at most 30 s |
| resident preload file bytes | at most 64 MiB |
| post-residency random-frame presentation p95 | at most 5 ms |
| post-residency TensorStore payload reads | exactly 0 |
| post-warmup deadline misses | at most 1% |
| incremental resident RSS cross-process p95 | at most 64 MiB |
| detection-process peak RSS | at most 768 MiB |
| cancellation and close p95 | at most 250 ms |
| stale resident publications | exactly 0 |

For the full-archive check, resident mode may regress median required-products
Ready time by at most 10% and 5 seconds, may add at most 64 MiB to median peak
RSS, and may not regress any maintained product's median initialization time by
more than 10%. Current-frame queue wait and playback deadline misses may not
regress.

## Selection

Residency passes only if every correctness, isolated, and full-archive gate
passes on the hybrid fixture. Passing authorizes a separately reviewed
byte-budgeted production policy and reduces the future layout matrix to the
128 KiB hybrid, one 8,192-row-aligned candidate, and the genuine 1 MiB
unsharded control.

If residency fails, Crimson retains paging and resumes the unchanged five-
candidate matrix contract. No production policy changes automatically from
this benchmark.
