# Canonical eye-overlay integration

Baseline: `a31f6e0`, `codex/main-integration-20260922`.
Scope: read-only August Sleepyfish eye geometry on the Linux canonical route.
Keep the legacy eye reader and macOS callers compatible. Do not rewrite the
archive, rerun analysis, independently select upstream latest runs, or couple
ordinary shape availability to optional eye data.

## Evidence and architecture

The selected schema-7 eye run names its exact shape-v5 geometry source. Existing
canonical selection validates its keypoint/mask/shape bindings. The legacy eye
reader instead expects ellipse geometry and crop lineage under an older refined
mask layout; its failure does not establish corrupt August lineage.

Add a bound canonical eye repository consuming `CanonicalOverlaySelection` and
returning the existing `EyeGeometryOverlayResolution`. Preserve the legacy
opener. Extend the eye descriptor/row with explicit source identity and stable
instance keys. Normalize geometry deliberately to the existing ROI-local scene
contract using validated, row-bound crop placement; do not use the full-camera
rectangle as a substitute subject-size reference. Ellipse centers are published
in full-camera pixels; eye-angle support/body-frame origin is ROI-local, and
direction vectors do not acquire a translation.

The new eye product has its own state, errors, demand, scheduler source, and
immutable frame snapshots. No rendering-thread storage access. Missing or failed
eye data must not suppress keypoints, masks, contours, shapes, or video.

### Storage correction

The 417,792-by-5 outer ellipse grid is a shard, not the decompression unit.
`sharding_indexed` contains 8,192-by-5 float32 chunks (160 KiB per eye).
Eye angles use 4,096-by-16 inner chunks (256 KiB), and gaze vectors use
8,192-by-2-by-2 inner chunks (128 KiB). TensorStore supports indexed byte-range
reads. Request the named angle-channel subset rather than all 141 columns.
Bound decoded caches and pending work; never equate requested rows with measured
physical NFS bytes or claim a fresh process flushes OS/NFS caches.

## Implementation checklist

### 1. Bound reader and scientific contract

- [x] Add `OpenBoundEyeGeometryOverlayRepository` with immutable selection,
      observation/read/cache limits and inspectable access/memory metrics.
- [x] Validate exact eye/shape/mask source identities, publication/lineage
      records, frame domain, array shapes/dtypes, and coordinate authority.
- [x] Resolve rows with frame offsets and check instance keys plus acquisition
      frames across all contributing sources; reject duplicates/mismatches.
- [x] Preserve ellipse width/height/angle semantics; verify major/minor-axis
      construction against producer declarations and stored eye channels.
      Do not assume width is always major solely from five sampled rows.
- [x] Normalize camera ellipse centers with exact bound crop translation;
      preserve per-subject ROI scale and body/vector coordinate conventions.
- [x] Resolve angle/vector/QA channels by name, not hardcoded numeric position.
      Distinguish eye-frame angle labels from body-frame signed gaze arcs.
- [x] Respect ellipse success, per-eye/frame QA, body validity and nonfinite
      values. Invalid values must not be published as measured geometry.
- [x] Use bounded lazy payload reads and reusable inner-chunk-aware caching.
      Keep body/eye payload off the render thread and preserve legacy opener.
- [x] Add synthetic real-Zarr tests for source/key/frame mismatch, missing
      optional data, multiple/reordered observations, invalid eyes, angle axes,
      translation, channel selection and read/cache limits; retain legacy tests.

### 2. Optional scheduled product and presentation

- [x] Provide a canonical shared-scheduler eye buffer with bounded lookahead,
      cache retention, current-frame priority and source-generation cancellation.
      Preserve legacy behavior when no shared scheduler is supplied.
- [x] Add eye repository/state/error/snapshot to canonical session lifecycle;
      opening/retirement remains asynchronous and failure-isolated.
- [x] Add explicit eye demand; no eye payload work while disabled. Cancel or
      invalidate unneeded speculative work on seeks, close and source changes.
- [x] Publish only exact presented-frame, selected-source, unique-instance-key
      matches. Never borrow eye values from a preceding video frame.
- [x] Feed validated ROI-local results through the existing eye scene adapter;
      expose an independent eye scene/status in canonical presentation.
- [x] Test slow readers, cancellation, stale generations, eye-only demand,
      disabled demand, read errors and continued availability of other products.

### 3. Linux renderer and controls

- [x] Implement generic scene Polygon fill/outline and text-annotation drawing
      in the Linux ImPlot path, preserving transforms, Y orientation, clipping,
      layering, font/background styles and alpha.
- [x] Test actual Linux drawing submission for polygons and annotations without
      requiring a GPU where feasible; shared Metal tests alone are insufficient.
- [x] Connect eye availability/demand and existing per-feature controls; remove
      the canonical hardcoded false only after the product is integrated.
- [x] Include eye scene/status in render diagnostics, keeping eye lines distinct
      from mask-contour counters and preserving controls during playback/seeks.
- [x] Cover axes, gaze rays, signed arcs, visual cones/overlap, angle/vergence
      labels, visibility toggles, ROI sizing and invalid-gaze fallback.

### 4. Supervised integration gates

- [x] Review each implementation against source identity, coordinate and
      scheduler contracts; resolve API boundaries before merging shared edits.
- [x] Register targets/tests and build with the approved Ubuntu 22 builder.
- [x] Run focused tests and the full CTest suite; retain old eye/macos-compatible
      contracts. No claim of macOS runtime validation without a Mac.
- [x] Probe bounded August rows and seeks, reporting exact values, validity,
      scene counts and elapsed times. Do not scan the full recording.
- [x] Measure first-access and warm playback/seek behavior, cache/read counters,
      disabled-overlay reads, and stale-frame rejection. Distinguish TensorStore
      logical/decoded bytes from measured filesystem/NFS transfers.
- [x] After building redgui, run the authenticated-display playback smoke per
      AGENTS.md, plus an August eye-overlay smoke and visual inspection. Do not
      control or terminate an existing user-owned Crimson session.
- [x] Update this checklist with evidence and honest limitations, and provide
      a runnable user test command. No commit, push or package publication is
      implied by this implementation request.

## Work ownership

- Reader implementer: bound reader, eye repository descriptor/identity fields,
  reader fixtures/probe. No canonical-session, renderer or CMake edits.
- Session implementer: eye buffer, canonical loader/session/presentation and
  focused session/presentation tests. No reader implementation, red.cpp or CMake.
- Renderer implementer: generic Linux polygons/text, renderer tests and metrics.
  No canonical reader/session, red.cpp or CMake.
- Supervisor: common interface coordination, CMake, red.cpp/control integration,
  cross-component review, approved-builder runs and authenticated GUI validation.

Progress: implementation and supervised Linux validation complete. The user
subsequently requested a local commit; this checklist accompanies that commit.
The checklist was created before delegation. See evidence and limitations below.
The complete reader inventory is in
[`canonical_eye_overlay_array_inventory_2026-09-23.md`](canonical_eye_overlay_array_inventory_2026-09-23.md).

## Validation evidence (2026-09-23)

- Approved Ubuntu 22/CUDA 12.4/TensorRT 10 builder: full build succeeded;
  **116/116 CTest tests passed** after the final memory-accounting correction.
  Logs: `/tmp/crimson-eye-memory-accounting-build.log`,
  `/tmp/crimson-eye-memory-accounting-tests.log`.
- Synthetic reader fixtures cover distinct digest canonicalizations, exact
  publication selection, keys/frames/duplicates, named-channel permutation and
  availability, 0/90-degree ellipse axes, mixed-coordinate translation, per-eye
  QA/ellipse success, body validity/nonfinite gaze, missing frames and read/cache
  admission limits. Legacy eye reader tests remain passing.
- Session tests cover disabled demand (zero reads), slow/stale/throwing readers,
  cancellation, exact-frame rejection and other-product availability. Renderer
  tests inspect real ImGui polygon/text vertices, colors, clip commands and
  label placement. These are not macOS runtime tests.
- Required authenticated-display legacy playback smoke passed frames 0–300:
  `/tmp/crimson_playback_smoke_20260923_165350.log`; repeated on the final binary
  in `/tmp/crimson_playback_smoke_20260923_170617.log` (passed).
- August GUI captures passed at frames 0 and 54000: exact keys/frame, three
  labels, two visual cones and one binocular overlap. Screenshot inspected;
  displayed frame-0 angles match the probe (left 29.5°, right 31.6°, vergence
  61.1°). Evidence:
  `/tmp/crimson-canonical-overlay-smoke.DdZNJ5`,
  `/tmp/crimson-canonical-overlay-smoke.Ztw7dA`.
  Disabled case `/tmp/crimson-canonical-overlay-smoke.IifuVs` recorded **zero eye
  payload reads**. Metadata/handle/index opening is still performed off-thread
  when the canonical session opens, including with overlays disabled.
- Real-data probe: frames 0, 1, repeated 0, 8191, 8192, 54000 and 600000
  succeeded. Open time 365.8 ms; individual reads 163.0, 1.43, 0.027 (frame-cache
  hit), 4.75, 1.09, 46.44 and **4906.74 ms**. Retained offsets: 23,500,840 bytes
  (22.4 MiB). Six uncached observations requested 1,182 logical bytes via 168
  array reads; this is **not** their decoded or NFS byte count.
  `/tmp/crimson-bound-eye-probe-20260923.json`.
- Bounded playback comparison, frames 8180–8480 at 30 source FPS / 60 UI cap
  after 10 s warmup: both runs passed, no skipped/late video frames. Across 630
  sampled playback draws, eye-on was ready every time with zero stale-frame or
  expected/actual draw-count mismatches. Active loop work (loop minus cap sleep):
  eye-off median/p95/max 2.55/4.41/6.61 ms; eye-on 2.56/4.68/7.30 ms.
  Median capped loop ~16.83 ms in both runs. Eye-on maximum reader service time
  was 127.8 ms (asynchronous); decoded frame cache reached 32,768 bytes.
  `/tmp/crimson-eye-playback.VxZog9/{off,on}.{log,jsonl}`.
  This is one paired run, not a statistically controlled regression benchmark.

## Limits and next performance investigation

- The original 4.9-second distant read has total resolve timing but no
  per-array timing or concurrent filesystem trace. Its precise cause is
  **unattributed**. Do not call it proven ellipse decompression or NFS latency.
- A subsequent fresh-process, syscall-traced probe at frame 600000 took 140 ms;
  adjacent frame 600001 took 6.45 ms and repeat 600000 took 0.013 ms.
  OS/NFS caches were not flushed, and tracing adds overhead. This does not
  reproduce or explain the original outlier.
  `/tmp/crimson-eye-distant-read-reprobe-20260923.json`,
  `/tmp/crimson-eye-distant-read-syscalls-20260923.log`.
- Current metrics: per-product scheduler queue/service time; total/max eye
  resolve; payload call count/logical returned bytes; frame-cache hits and
  retained decoded bytes. Opt-in performance traces also record whole-process
  TensorStore file-read counters. Per-array latency/decode time and on-wire NFS
  traffic are not currently instrumented.
- One uncached frame uses 28 sequential logical array reads; a cold seek may
  need several inner chunks and shard indexes. Chunk reuse amortizes this
  during playback. A useful next optimization is a bounded multi-array read
  plan (overlap independent reads, reuse shared identity pages, and prefetch
  upcoming storage chunks), with current-frame priority/cancellation retained.
  Do not add unbounded concurrency or remove provenance checks.
- TensorStore uses a shared 64 MiB cache pool; the reader's reported memory
  excludes that shared allocation and reports incomplete accounting. Resolved
  cache is limited by both bytes (32 MiB) and 64 entries; the canonical buffer
  retains 8 frames with 2-frame lookahead. Decoded inner-chunk admission is
  capped at 2 MiB per array chunk. These are not a hard total-process memory cap.
- Source selection validates publication manifests; the bound reader rechecks
  its source/coordinate contracts and individual row keys/frames. It does not
  rehash every source payload. Published runs are treated as immutable.
- No exhaustive measurement-quality audit, cold-NFS guarantee, macOS/Windows
  runtime validation, commit, push, or replacement Ginny package is implied.

## Local user test

From the integration worktree, use the newly built executable rather than an
older packaged binary:

```bash
cd /home/delahantyj@hhmi.org/gitrepos/crimson-main-integration-20260922
crimson_runtime="$PWD/dist/Crimson-linux-integration-20260922"
DISPLAY=:1 \
XAUTHORITY=/run/user/64406/.mutter-Xwaylandauth.MXCPR3 \
LD_LIBRARY_PATH="$crimson_runtime/lib:$crimson_runtime/lib64:$crimson_runtime/lib/crimson/private:$crimson_runtime/lib64/crimson/private" \
./release/redgui \
  --zarr /misc/public/forGinny/recordings/2026_08_06_19_13_35_cam2010093/zarr/2026_08_06_19_13_35_cam2010093_analysis.zarr \
  --show-subject-masks --show-eye-geometry --swap-interval 0 --frame-cap-fps 60
```

Eye controls are in Frame Inspect → Eye Angles. The display/auth pair was
verified for this session; reprobe if the desktop login changes.
