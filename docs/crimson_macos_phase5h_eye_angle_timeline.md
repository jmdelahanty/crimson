# Crimson Phase 5H Eye-Angle Timeline

Date: 2026-07-14

Phase 5H adds Crimson's first native macOS analysis plot: a playback-synchronized
eye-angle timeline with maintained representation semantics, independent
left/right/vergence visibility, a current-frame cursor, and click-to-seek. The
Mac reads compact Zarr v3 analysis arrays through TensorStore; it does not load
the maintained application's eager in-memory analysis model or use Python Zarr.

This is not full UI parity. Motion, tail-kinematics, and stimulus timelines;
editing and review workflows; docking; and the rest of the maintained
Linux/Windows workspace remain later Phase 5 checkpoints.

## Shared Contract

`EyeAngleTimelineRepository` is a backend-neutral C++ interface. Its descriptor
contains the selected run, schema, representation order/default, and resolved
field metadata. Window requests contain a representation, inclusive camera
frame bounds, an anchor frame, a per-trace point limit, and a fallback frame
rate. Results distinguish mapped, missing, out-of-range, invalid-request, and
read-failed states.

The portable implementation provides:

- maintained left, right, vergence, and other trace roles;
- fixed overlapping page bounds;
- extrema-preserving per-trace decimation that retains endpoints and the
  current-frame neighborhood;
- stored-time interpolation with frame-rate fallback;
- exact time-to-camera-frame mapping for plot seeking; and
- `EyeAngleTimelineBuffer`, a one-worker asynchronous cache with generation
  invalidation, bounded pending work, and a three-page capacity.

The contract has no ImGui, ImPlot, Metal, OpenGL, CUDA, TensorStore, or Zarr
dependency. The maintained NVIDIA target compiles the portable layer, while
TensorStore adapters remain owned by the explicit repository object library.

## TensorStore Repository

`OpenEyeAngleTimelineRepository` accepts complete schema-5
`analysis.eye_angle_runs` using `compact_dense_v2`. Archive open reads only
small metadata and channel-index arrays. A window request slices the required
rows from `frame_angles` and `support/frame_time_seconds`; it never reads all
143,447 frame rows into the Mac process.

Representation order and default selection come from
`eye_angle_variant_schema`. Each representation uses its maintained
`default_plot_fields`. Requested smoothed fields fall back to the corresponding
unsmoothed field only when the smoothed channel is unavailable. Channels must
be marked frame-available and their indices and time-array dimensions are
validated before publication.

The production defaults are a 4,096-frame page, 2,048-frame step, at most 1,200
points per trace, and three cached pages. The repository publishes immutable
windows, so the UI never observes a partially filled TensorStore read.

## Native macOS UI

The transport toolbar exposes a `Timeline` command when eye-angle data is
available. The movable, resizable `Eye-angle timeline` window opens above the
transport and contains:

- the maintained representation selector, defaulting to
  `Bianco/Engert eye-frame angles` for the production fixture;
- independent left, right, and vergence checkboxes;
- a 1-to-30-second half-span control;
- colored ImPlot traces and a labeled current-frame cursor; and
- plot-click seeking through the existing pause and exact-frame
  `AppleVideoPlaybackBuffer::requestSeek` path.

Changing representation requests a new bounded page and shows a loading state
until it is atomically available. Missing, failed, and out-of-range states are
shown without removing the panel. The timeline is closed by default and does
not issue analysis reads during ordinary playback while unused.

`--show-eye-angle-timeline` opens the panel at launch for review.
`--require-eye-angle-timeline` makes a video smoke fail when the production
timeline cannot be opened or resolved. `--no-eye-angle-timeline` disables the
optional repository.

## Deterministic Coverage

`eye_angle_timeline_tests` covers representation/default selection, trace-role
classification, smoothed fallback metadata, spike-preserving decimation,
stored-time interpolation, exact click time-to-frame mapping, missing values,
invalid and out-of-range requests, overlapping page boundaries, asynchronous
publication, discontinuity handling, and a two-page cache bound.

`eye_angle_timeline_repository_tests` creates real Zarr v3 archives through
TensorStore. It verifies latest-run selection, schema and layout metadata,
representation order, field fallback, frame-available channels, bounded row
slices, decimation, stored frame times, gaze selection, and rejection of a
channel index that exceeds the compact array shape.

Both tests are labelled `timeline;headless;portable`; the repository fixture
also carries `tensorstore;zarr`. The standard Mac release preset builds the two
tests and the production probe.

## Production Validation

The mounted Mac fixture was:

```text
2026-05-29T18-11-16Z_arena_1_GoodCopBadCop_analysis.zarr
```

The repository selected
`eye_angles_goodcopbadcop_arena1_eye_shape_20260713_v001`: schema
`analysis.eye_angle_runs:5`, `compact_dense_v2`, 143,305 ROI rows, 143,447
camera-frame rows, 141 angle channels, and six representations. The default is
`eye_frame`.

The production Mac smoke passed camera frames 100 through 160. One 4,096-row
TensorStore read published 3,596 points across the three default traces in
130.4 ms. The following 49 timeline presentations were cache hits. It reported
zero missing, failed, or discarded windows, one cached page, one peak pending
request, zero PTS error, and zero late video presentations. The complete Mac
preset passed all 28 tests. Screenshot QA at the production window size
verified trace visibility, selector and span layout, cursor placement, and
non-overlap with the transport.

The cumulative source configured and completed its 233-step build in an isolated
NVIDIA worktree on `ws1`. The build retained CUDA 12.4 with architectures 80
and 86, TensorRT 10.0.1.6, OpenCV 4.10.0 with SFM, the NVIDIA FFmpeg stack,
NVDEC/OpenGL, the three CUDA translation units, and `redgui`. All 16 portable
CTest targets passed.

The server-local production probe selected the same run and read frames 0
through 227: 228 source rows became 378 plotted points in 44.9 ms after a
132.1 ms archive/repository open. The authenticated RTX A6000 maintained GUI
smoke then passed frames 0 through 300 of the June 14 recording with 350
presentations in 2.992 seconds.

## Remaining UI Parity

Phase 5H establishes the bounded analysis-series boundary the Mac can reuse.
It does not replace the maintained eager timeline loader or claim the complete
workspace. Subsequent checkpoints must decide which motion, tail, stimulus,
debug, and editing surfaces are required, connect them through similarly
explicit contracts, and finish the screenshot/image-difference parity gate.
