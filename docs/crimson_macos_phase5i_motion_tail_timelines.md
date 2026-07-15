# Crimson Phase 5I Motion and Tail Timelines

Date: 2026-07-14

Phase 5I extends the native macOS analysis timeline with maintained motion and
tail-kinematics scalar traces. Motion, eye angles, and tail kinematics now share
one `Analysis timeline` window with tabs, a common time span, current-frame
cursors, and plot-click exact seeking.

This checkpoint does not claim full UI parity. Stimulus context and event
timelines, swim-bout rectangles, editing/review actions, docking, and the final
golden-image gate remain later Phase 5 work.

## Maintained Semantics

The contract follows the existing Linux/Windows analysis timeline rather than
inventing Mac-only trace definitions.

Motion source discovery checks the latest `offline`, `online_refined`, and
`online` track-kinematics runs. Each track can expose `filtered`, `smoothed`,
`raw`, and `averaged` variants. The maintained default visibility is:

- selected-variant speed: visible;
- raw speed comparison: hidden;
- raw heading: hidden;
- smoothed heading: visible; and
- X and Y position: visible.

Tail kinematics exposes the maintained scalar rows:

- tail-tip angle and maximum absolute tail angle: visible;
- tail-tip lateral deflection: visible; and
- maximum absolute tail curvature: hidden.

The Mac adapter does not replace or modify the maintained eager
`zarr_loader_movement.cpp` and `zarr_loader_tail_kinematics.cpp` paths. Both
platform stacks retain their platform-appropriate repository implementations
behind portable timeline contracts.

## Shared Sparse-Series Contract

`AnalysisSeriesTimelineRepository` is a backend-neutral interface shared by
motion and tail kinematics. Descriptors identify the timeline kind, sources,
track and variant metadata, trace roles, row grouping, units, and default
visibility. Requests contain one source, inclusive camera-frame bounds, an
anchor frame, a point limit, and a fallback frame rate.

The portable implementation provides:

- exact sparse camera-frame mapping with missing-frame support;
- stored-time interpolation with frame-rate fallback;
- extrema-preserving per-trace decimation;
- role-stable colors and row grouping for speed, heading, position, and tail
  measures;
- exact time-to-camera-frame mapping for plot seeking; and
- `AnalysisSeriesTimelineBuffer`, a one-worker, generation-aware asynchronous
  cache with bounded pending work and a three-page capacity.

The contract has no ImGui, ImPlot, Metal, CUDA, OpenGL, TensorStore, or Zarr
dependency. It compiles and runs on both the Apple and NVIDIA builds.

## TensorStore Adapters

`OpenMotionSeriesTimelineRepository` discovers maintained track-kinematics
sources and opens only compatible arrays. It accepts `int64` or `int32` frame
maps, `float` or `double` scalar/vector values, and boolean or byte validity
masks. Position uses millimeters when available and otherwise preserves pixel
units.

`OpenTailKinematicsTimelineRepository` selects the requested or latest run,
accepts `frame_index` or `row_to_frame`, and publishes the four maintained
scalar fields that are present with matching row counts.

Sparse row bounds are found with a lazy 16,384-row frame-index block cache.
Only the intersecting trace/time rows are then sliced from TensorStore. The
reported `source_rows_read` metric counts the selected timeline rows, not the
internal frame-index lookup blocks. No complete motion or tail value array is
materialized during archive open or page resolution.

Production defaults remain a 4,096-frame page, 2,048-frame step, at most 1,200
points per trace, and three cached pages. Published windows are immutable.

## Native macOS UI

The transport `Timeline` command is enabled when any analysis timeline is
available. It opens one movable, resizable `Analysis timeline` window with the
available Motion, Eye angles, and Tail tabs.

The Motion and Tail tabs provide source selection and trace visibility controls
initialized from the maintained defaults. Traces are grouped into separate
ImPlot rows by compatible units. Every row uses the shared time span and a
labeled current-frame cursor. Plot clicks pause playback and call the existing
exact-frame `AppleVideoPlaybackBuffer::requestSeek` path.

The window is closed by default. Ordinary playback does not issue timeline page
reads while it is closed. Source changes publish a new page atomically and show
a loading state until the requested page is available.

Launch controls are:

- `--show-analysis-timeline` opens the unified window;
- `--show-eye-angle-timeline` remains a compatible alias and selects the Eye
  angles tab;
- `--require-motion-timeline`, `--require-eye-angle-timeline`, and
  `--require-tail-kinematics-timeline` add deterministic smoke gates; and
- matching `--no-...-timeline` flags disable each optional repository.

## Deterministic Coverage

`analysis_series_timeline_tests` covers sparse frame mapping, missing frames,
stored and fallback time mapping, exact seeking, extrema retention, invalid and
out-of-range requests, source switching, discontinuity invalidation, page
bounds, and cache eviction.

`analysis_series_timeline_repository_tests` creates actual Zarr v3 arrays
through TensorStore. Its motion fixture verifies latest-run discovery, four
source variants, sparse `int64` frames, validity-mask filtering, six trace
roles, positions, row slicing, and defaults. Its tail fixture verifies latest
selection, `int32` frames, all four scalar rows, decimation, and maintained
visibility defaults.

Both tests carry `headless;portable;repository;timeline`; the adapter fixture
also carries `tensorstore;zarr`. The production probe is built by the standard
Mac preset and by the NVIDIA validation build.

## Production Validation

The mounted Mac motion fixture was:

```text
2026-05-29T18-11-16Z_arena_1_GoodCopBadCop_analysis.zarr
```

The repository selected
`track_kinematics_goodcopbadcop_arena1_core_20260714_v003`, track `id_0`,
filtered variant. The recording has 143,305 sparse track rows and 143,447
camera frames. Frames 0 through 4,095 selected 4,029 source rows and published
6,062 points across filtered speed, raw speed, raw heading, smoothed heading,
X position, and Y position.

The final Mac native smoke passed frames 0 through 100 directly from the mounted
network recording. It reported 69 timeline presentations after one resolved
page, 69 cache hits, zero missing, failed, or discarded windows, one peak
cached page, one peak pending request, zero PTS error, and zero late
presentations. All 29 Mac headless tests passed.

Neither the May 29 nor June 14 representative production archive currently
contains `analysis/tail_kinematics_runs`. Tail production-data acceptance
therefore remains open; this checkpoint validates the adapter and UI data
contract with the real TensorStore-written Zarr v3 fixture instead of claiming
a production tail probe.

The cumulative source completed the isolated NVIDIA build with CUDA 12.4,
architectures 80 and 86, TensorRT 10.0.1.6, OpenCV 4.10.0 with SFM, the NVIDIA
FFmpeg stack, NVDEC/OpenGL, all three CUDA translation units, and `redgui`. All
18 portable tests passed.

The server-local motion probe selected the same source and published the same
4,029 rows and 6,062 points in 55.2 ms after an 86.8 ms open. The authenticated
RTX A6000 maintained GUI smoke then passed frames 0 through 300 of the June 14
recording with 351 presentations in 2.990 seconds.

## Remaining UI Parity

Phase 5I establishes a reusable bounded sparse-series path and adds the first
maintained motion and tail surfaces to the Mac shell. Phase 5 remains active.
The next timeline checkpoint should address stimulus events/context without
forcing event records into this scalar-series contract. Tail must also be
probed against a production archive once one is available. The final Phase 5
gate still requires the remaining edit/review workflows, workspace behavior,
and screenshot/image-difference acceptance.
