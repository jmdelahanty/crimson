# Crimson Phase 5O.4 Full-Analysis Fixture Contract

Date: 2026-07-25

Status: fixture and acceptance contract frozen; Crimson adapter prerequisite
complete; Palette fixtures and the full-archive runner not yet published

## Decision

The full-archive checkpoint has two stages:

1. Compare otherwise-identical regular and access-aware hybrid archives with
   Crimson's unchanged 64 MiB production `ArchiveContext` cache.
2. Only if the hybrid passes, compare 16 MiB and 64 MiB caches on the hybrid
   archive.

Stage 1 changes storage layout only. Stage 2 changes cache policy only. Both
stages retain zero speculative time-based read-ahead and the existing one-page
asynchronous demand lead. The 135.2 ms process-first median offset read remains
visible in startup and first-overlay measurements, but is not an independent
rejection gate.

## Crimson Prerequisite

The normal macOS application now exposes `--detection-run`, loads the exact
canonical run asynchronously through `AppleAnalysisRepositoryBundle`, and
renders its boxes through the shared backend-neutral overlay scene. The
implementation is split across:

- `src/zarr/tensorstore_canonical_detection_repository.cpp` for consolidated
  schema validation, exact typed opens, and one retained offsets read;
- `src/canonical_detection_buffer.cpp` for 70-frame demand pages, one-page
  asynchronous lead, generation cancellation, and bounded presentation
  caching;
- `src/zarr/canonical_detection_overlay_scene_adapter.cpp` for the persisted
  source-camera-normalized box contract; and
- `src/platform/macos/apple_analysis_repository_loader.cpp` and
  `src/platform/macos/crimson_macos_main.mm` for asynchronous application
  loading, required-product readiness, CLI selection, rendering, and
  telemetry.

The headless repository fixture verifies exact selection, all nine
consolidated declarations, four exact handle opens, zero fallback metadata or
dtype probes, one offsets read, empty and multi-detection frames, page
scheduling, and overlay coordinates. An explicit missing run publishes an
unavailable required product and blocks readiness rather than selecting a
fallback.

Palette may now build the paired archives. Their publication is not itself
application performance evidence: the deterministic five-process runner,
first-presentation readiness measurement, physical I/O capture, and reducer
remain the next Crimson checkpoint.

The required explicit selection surface is:

```text
--detection-run crimson_storage_fixture_sleepyfish_cam2010095_v1
```

It must select exactly:

```text
detect_runs/crimson_storage_fixture_sleepyfish_cam2010095_v1
```

No latest-run fallback, dtype probing, or legacy detection-loader fallback is
allowed when this option is present. An unknown, incomplete, or incompatible
run must fail the benchmark session before playback.

This is not a request for Palette to preserve every historical immutable run.
Each fixture needs only the selected benchmark run and the maintained
nondetection content required below.

## Canonical Detection Schema

The run contains one `instances` group with these exact arrays:

| Relative path | Exact type |
| --- | --- |
| `instances/frame_indices` | `int32 (N,)` |
| `instances/source_acquisition_frame_index` | `int64 (N,)` |
| `instances/instance_key` | `uint64 (N,)` |
| `instances/bbox_norm_coords` | `float32 (N,4)` |
| `instances/bbox_img_xyxy` | `float32 (N,4)` |
| `instances/centers_img_xy` | `float32 (N,2)` |
| `instances/scores` | `float32 (N,)` |
| `instances/class_ids` | `int32 (N,)` |
| `instances/frame_row_offsets` | `int64 (F+1,)` |

`frame_row_offsets` is read completely once per selected adapter lifetime,
validated, retained, and never reread during seeking or playback. The timed UI
path reads only `bbox_norm_coords`, `scores`, and `class_ids`; the other fields
remain part of schema and value validation.

The regular fixture uses 1 MiB unsharded chunks. The hybrid fixture uses
approximately 128 KiB inner chunks for windowed instance columns, 1 MiB inner
chunks for the eager offsets array, and 8 MiB outer indexed shards. The hybrid
codec chain is Zarr v3 `sharding_indexed`, little-endian bytes plus Zstandard
level 0 for inner payloads, and little-endian bytes plus CRC32C for the
end-located shard index.

## Fixture Identity

Both archives derive from the same maintained recording:

```text
/Volumes/johnsonlab/jeremy/recordings/
sleepyfish_2026_05_05_17_45_30_cam2010095
```

They retain the same raw-video association:

```text
cams/Cam2010095_sleepyfish_2026_05_05_17_45_30_cam2010095.mp4
```

The associated source is camera `2010095`, 4512 by 4512 pixels, 1,188,000
frames, and 30 FPS. Both archives must resolve to that one existing video; the
fixture must not copy or transcode it. No stimulus video is required.

The fixtures live outside production recording, registry, and selector state,
for example under:

```text
/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/
canonical_detection_storage/full_analysis/sleepyfish_cam2010095_v1/
```

Recommended leaf names are `regular.zarr` and `hybrid.zarr`. Both contain the
same canonical detection run name. Their benchmark-local selector attributes
may point to that run, but the Crimson test still supplies `--detection-run`.

Palette must publish a logical-content manifest proving that all nondetection
groups and decoded detection values are identical. Only detection chunk,
shard, codec, and the necessarily changed consolidated metadata bytes may
differ. Failed fixtures remain outside production selection and are never
reused as successful evidence.

## Consolidated Discovery

Inline Zarr v3 consolidated metadata is required in both fixture roots. It is
part of this checkpoint for canonical detection discovery and exact typed
opens:

- the direct and consolidated descriptions of the run and all nine arrays must
  agree;
- the timed canonical adapter must use the consolidated descriptions and
  perform zero fallback metadata or dtype opens; and
- normalized logical consolidated inventories must match between fixtures.

Rewriting every legacy nondetection repository to use consolidated discovery
is not part of this storage-layout comparison. Their existing open behavior is
measured identically in both archives and remains separately tracked technical
debt.

## Simultaneous Products

The full readiness transaction enables these products together:

| Product | Requirement |
| --- | --- |
| raw video | association validated; decoder prepared for GUI smoke |
| canonical detections | selected explicit run; first overlay required |
| refined keypoints | required |
| refined subject masks | required |
| subject shape | required |
| eye geometry | required |
| motion timeline | required |
| eye-angle timeline | required |
| tail-kinematics timeline | required |
| crop geometry | required; geometry-only ROI presentation |

Swim-bout and stimulus-context timelines are disabled because the maintained
Sleepyfish selection is unavailable or incompatible. Chaser polar, stimulus,
and acquisition crop remain optional unavailable probes only if the current
loader cannot omit them; they must behave identically in both fixtures and may
not delay the required-products milestone.

The application invocation is:

```bash
CRIMSON_STARTUP_TRACE=1 \
build/macos-arm64-release/Crimson.app/Contents/MacOS/Crimson \
  --zarr "$ARCHIVE" \
  --video "/Volumes/johnsonlab/jeremy/recordings/sleepyfish_2026_05_05_17_45_30_cam2010095/cams/Cam2010095_sleepyfish_2026_05_05_17_45_30_cam2010095.mp4" \
  --detection-run crimson_storage_fixture_sleepyfish_cam2010095_v1 \
  --crop-source geometry \
  --no-swim-bout-timeline \
  --no-stimulus-context-timeline \
  --show-analysis-timeline \
  --show-eye-angle-timeline \
  --start-paused 0
```

The loading modal remains responsive until all required products are ready.
Closing the modal does not start playback; the session begins paused.

## Frozen Workload

Each condition runs in five fresh processes. Stage 1 uses matched regular and
hybrid fixtures in balanced process-first order. OS, SMB, and server caches are
uncontrolled and must not be described as cold.

Each process performs:

1. Open the root consolidated metadata and resolve the source video.
2. Select the explicit canonical detection run, create the shared 64 MiB
   archive context, and initialize all required products concurrently.
3. Read and retain offsets exactly once, prepare frame 0 and every required
   first presentation, publish Ready, and remain paused.
4. Settle at eight deterministic random frames:
   `271085, 85499, 397712, 1003450, 939795, 903492, 351953, 1141796`.
5. Submit a rapid seek burst to
   `560111, 1066017, 905397, 100063, 996466, 909512, 639969, 378251`; only the
   final generation may publish.
6. Traverse 3,500 frames forward and reverse in 70-frame pages at a simulated
   700 FPS analysis deadline. The renderer uses cache-only presentation, the
   first page is prepared before the clock, and zero speculative read-ahead
   retains the one-page demand lead.
7. Cancel outstanding work, close the session, and verify bounded shutdown.

The first stage is a headless full-repository/application transaction so video
decode and window interaction do not obscure storage evidence. Each accepted
pair also receives one real GUI correctness smoke at the recording's native
30 FPS to verify the paused loading transition, detection overlay, geometry
crop inset, masks, keypoints, and timelines. GUI timing is diagnostic, not the
storage-layout reducer. The event-loop interval gate below applies to these GUI
smokes; the remaining numeric gates apply to the five-process headless matrix.

## Frozen Stage 1 Gates

Correctness gates are absolute:

- all schema, codec, CRC, consolidated/direct metadata, and decoded-value
  checks pass;
- the exact requested run is selected with no fallback probes;
- all required products become ready with identical source and frame identity;
- offsets are read exactly once per adapter lifetime;
- stale publications, failed reads, and application-thread analysis waits are
  exactly zero; and
- at least two of the three UI detection fields overlap in flight.

Performance and resource gates are:

| Evidence | Frozen limit |
| --- | ---: |
| every process reaches required-products Ready | at most 180 s |
| hybrid median Ready time regression | at most 10% and at most 5 s |
| main event-loop interval during loading | at most 250 ms |
| first detection overlay after archive context is ready | p95 at most 1 s |
| hybrid first-overlay p95 regression | at most 250 ms |
| post-warmup detection deadline miss rate | at most 1% |
| seek cancellation cross-run p95 | at most 250 ms |
| post-cancel detection transfer cross-run p95 | at most 1 MiB per seek |
| total process peak RSS | at most 2 GiB |
| hybrid median peak-RSS regression | at most 128 MiB |
| hybrid total TensorStore file bytes | at most 1.05 times regular |
| hybrid detection-only traversal bytes | at most 0.25 times regular |
| shutdown settlement | at most 2 s |

Ready time starts before root metadata open and ends only when all required
products have installed self-consistent first presentations. First-overlay time
starts when the shared archive context is available. Offset open/read/validate
time is reported separately and remains included in both applicable totals.

TensorStore file-kvstore bytes are driver-level range bytes, not SMB wire
bytes. Report them honestly as such. Record total RSS rather than adding cache
limits; offsets, application pages, decoded chunks, video buffers, GPU staging,
and in-flight reads all consume memory outside the configured cache.

## Frozen Stage 2 Decision

Stage 2 runs only after the hybrid passes every Stage 1 gate. It repeats the
same five-process hybrid workload at 16 MiB and 64 MiB, with layout and
read-ahead held fixed.

The 16 MiB policy is accepted only if it passes every absolute Stage 1 gate,
adds no more than 1% to median Ready time, adds no more than 250 ms to
first-overlay p95, has no more than 5% additional total file bytes, and does
not increase peak RSS. If the two conditions are equivalent within those
limits, select 16 MiB as the smaller sufficient cache. Otherwise retain
64 MiB.

Neither stage promotes Palette's writer profile automatically. Passing both
creates promotion evidence for a separately versioned storage-policy decision.

## Required Evidence

For every fresh process retain:

- fixture manifest and content fingerprints;
- Crimson, TensorStore, Palette, macOS, machine, mount, and VPN identity;
- exact CLI, cache bytes, run path, process order, and workload seed/list;
- consolidated reads, direct/fallback opens, file operations, batched reads,
  file bytes, cache hits/misses/evictions, and concurrent reads;
- per-product startup timing, Ready time, first-overlay time, and offset timing;
- seek, traversal, deadline, cancellation, stale-result, and shutdown metrics;
  and
- peak RSS plus retained offsets, presentation-cache, and preload byte counts.

Reduction reports medians and cross-process p95 values without pooling samples
from different conditions. A failed correctness check is not averaged into a
performance result.
