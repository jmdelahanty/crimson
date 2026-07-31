# Crimson Native Apple Silicon Port — Codex Goal

Use this document as the authoritative goal and execution contract for a Codex
session running in the Crimson worktree on an Apple Silicon Mac.

## Paste-Ready `/goal`

```text
/goal Build a native Apple Silicon macOS backend for Crimson while preserving
all user-visible workflows, scientific data semantics, frame identity,
Zarr behavior, editing behavior, overlays, synchronization, and supported
inference workflows. Keep the existing Linux/Windows NVIDIA backend working and
unchanged by default. Implement the port additively in this repository using
backend abstractions: AVFoundation/VideoToolbox for macOS video frame access,
Metal for rendering and GPU presentation, and a validated Apple inference
backend selected only after viewer parity is established. Work incrementally,
validate every milestone on real representative data, document measured
differences, and do not mark the goal complete until the parity and packaging
acceptance gates in docs/crimson_macos_port_codex_goal.md are satisfied.
```

## Mission

Produce a native arm64 macOS build of Crimson for Apple Silicon. The macOS build
must provide the same scientific and user-facing behavior as the maintained
NVIDIA build, except for explicitly documented platform concepts that have no
literal Apple equivalent. The port must coexist with—not replace or weaken—the
current CUDA, NVDEC, TensorRT, OpenGL, Linux, and Windows paths.

This is a backend port inside the existing repository, not a separate product
rewrite. Shared application behavior remains shared. Platform-specific decode,
GPU-surface, renderer, inference, and packaging code belongs behind explicit
interfaces.

## Evidence Already Established

Treat the following as measured starting evidence, not assumptions:

- Apple M3, 24 GB unified memory, 10-core GPU, Metal 4.
- AVFoundation-to-`CVPixelBuffer`-to-Metal playback of representative
  4512 x 4512, 100 fps HEVC remained clock-correct with approximately 11–22 ms
  presentation lag.
- First native Metal-presented frame was available in approximately 172 ms.
- FFmpeg's VideoToolbox decode path did not sustain the logical source clock;
  do not use that isolated result to reject AVFoundation.
- AVFoundation exact random access and consecutive paused stepping passed for
  representative main and crop recordings.
- The original acquisition stimulus H.264 artifacts were not compatible with
  AVAssetReader random access. A controlled 120 fps H.264 derivative with a
  closed 240-frame GOP passed exact access and stepping.
- Main, crop, and recovered stimulus passed real Zarr-mapped frame access with
  no measured error except one 0.002-frame timestamp representation difference.
- A portable alignment fixture exporter and Mac probes exist under
  `experiments/macos_video_probe`.
- The actual mapping precedence is documented in
  `docs/stimulus_alignment_overview.md` and implemented in
  `src/zarr_loader_stimulus.cpp`.

Read these before changing architecture:

- `docs/crimson_ubuntu_macos_platform_strategy.md`
- `docs/stimulus_alignment_overview.md`
- `docs/crimson_main_camera_playback_renderer_plan.md`
- `docs/crimson_video_color_range_and_luma_display.md`
- `docs/crimson_threading_architecture_notes.md`
- `docs/crimson_packaging_and_distribution_plan.md`
- `experiments/macos_video_probe/README.md`

## Non-Negotiable Constraints

### Existing Platforms

1. Linux and Windows NVIDIA builds must remain supported.
2. Existing presets must retain their current CUDA/TensorRT/NVDEC/OpenGL
   behavior unless a separately justified change is required.
3. CUDA must remain enabled by default for the maintained NVIDIA presets.
4. Do not replace the NVIDIA backend with portable CPU code merely to make
   macOS configure.
5. Do not remove features, sources, presets, tests, or dependencies used by
   current supported builds.
6. Every backend separation change must be compiled or tested on an available
   NVIDIA host before it is considered complete.

### macOS Platform

1. Target native Apple Silicon arm64. Do not make Rosetta/x86_64 the supported
   architecture.
2. Do not introduce CUDA, TensorRT, NVDEC/CUVID, NPP, GLEW, or OpenGL as macOS
   requirements.
3. Use Metal for the maintained macOS rendering path.
4. Use native `CVPixelBuffer`/IOSurface-backed surfaces and
   `CVMetalTextureCache` where feasible. Avoid routine full-frame CPU copies.
5. Use AVFoundation as the first macOS video backend candidate. Crimson—not
   independent AVPlayer clocks—owns the logical application timeline.
6. VideoToolbox is a codec facility, not the application clock or Zarr
   alignment authority.
7. MLX is not a renderer. Metal shaders perform image conversion, composition,
   masks, and overlays.
8. Do not select an inference backend based only on convenience. Validate ONNX
   Runtime/Core ML, Core ML, MPS, and/or MLX against the actual models and
   preprocessing before committing to one.

### Repository and Change Safety

1. Work in a dedicated branch/worktree. Do not push directly to the protected
   main branch.
2. Preserve unrelated user changes and dirty-worktree content.
3. Keep commits scoped and reversible. Separate architecture seams, Mac
   backend implementation, behavior changes, tests, and documentation where
   practical.
4. Do not commit videos, model binaries without explicit approval, build
   directories, generated metrics, `.app` build products, or large test
   packages.
5. Prefer repository-relative resources and executable/app-bundle-relative
   runtime lookup. Do not add developer-machine absolute paths.
6. Do not silently change file formats, Zarr schemas, frame numbering,
   coordinate systems, color interpretation, or write semantics.
7. Any discovered source-data/acquisition defect must be reported separately;
   do not hide it with an application-side data rewrite.

## Definition of Feature Parity

Parity means equivalent user-visible and scientific behavior, not identical
GPU APIs or necessarily bit-identical floating-point/raster output.

### Required Exact Semantics

The following must remain exact unless an approved contract explicitly allows
otherwise:

- source frame identity and integer frame numbering;
- camera/crop frame correspondence;
- camera-to-stimulus mapping selection and resolved stimulus frame numbers;
- Zarr group/array discovery and latest-run selection;
- read/write destinations, schemas, dimensions, dtypes, and identity fields;
- paused frame stepping and requested-frame settlement;
- seek target interpretation;
- clipped-recording parent/local frame mapping;
- annotation/edit acceptance and persistence behavior;
- coordinate systems for boxes, masks, keypoints, crops, and overlays;
- event/step selection and timeline navigation targets;
- no silent frame duplication, deletion, or retiming in scientific workflows.

### Required User-Visible Workflows

Inventory and preserve at least:

- opening real recording/Zarr sessions;
- main camera playback;
- crop and multiview playback;
- stimulus inset/window playback;
- pause, resume, random seek, slider seek, forward/backward stepping;
- clipped playback and boundary handoffs;
- main/crop/stimulus synchronization;
- zoom, pan, rotation, crop, preview scaling, and renderer modes;
- masks, contours, boxes, keypoints, skeletons, eye overlays, stimulus overlays,
  and analysis plots/timelines;
- keypoint, mask, bounding-box, and review/edit workflows currently exposed by
  the application;
- detection/review acceptance behavior;
- inference workflows currently supported in the maintained application;
- debug/performance surfaces needed to validate playback and frame identity;
- file/session discovery and resource loading;
- packaging and launch without a developer shell.

Before claiming parity, produce a feature inventory tied to concrete source
locations and an acceptance test or documented manual check for every item.

### Allowed Numerical Differences

Metal rasterization and Apple inference may not be bit-identical to
OpenGL/CUDA/TensorRT. Numerical differences are allowed only when:

1. a tolerance is defined before acceptance;
2. the tolerance is scientifically and visually justified;
3. representative fixtures are compared;
4. regressions outside the tolerance fail validation; and
5. the difference is documented in the supported-backend matrix.

Do not describe an unmeasured difference as acceptable.

## Target Architecture

```text
Shared application behavior
  ├── session/Zarr model
  ├── logical playback clock and frame identities
  ├── interaction/edit/review workflows
  ├── overlay preparation and coordinate semantics
  └── backend-neutral requests/results
       ├── NVIDIA backend
       │    ├── FFmpeg/NVDEC/CUVID
       │    ├── CUDA/NPP surfaces and processing
       │    ├── OpenGL presentation
       │    └── TensorRT inference
       └── Apple backend
            ├── AVFoundation/VideoToolbox frame provider
            ├── CVPixelBuffer/IOSurface surfaces
            ├── Metal presentation/compute
            └── validated Core ML/ONNX/MLX inference backend
```

Backend interfaces should express capabilities and data contracts, not expose
CUDA or Metal types through shared application headers. Platform-native handles
may live behind opaque backend-owned objects.

## Execution Plan and Gates

### Phase 0 — Baseline and Inventory

Deliverables:

- record branch, commit, compiler, SDK, CMake, dependency, and host details;
- capture current Linux build/test status before refactoring;
- inventory platform-coupled source files and link dependencies;
- inventory user-visible workflows and existing test coverage;
- identify current golden datasets, model files, and expected outputs;
- document gaps rather than assuming existing coverage proves parity.

Gate:

- baseline is reproducible and the parity inventory is checked into `docs/`.

### Phase 1 — Optional CUDA and macOS Build Skeleton

This is the first active implementation slice.

Deliverables:

- refactor top-level CMake so CUDA is enabled only for backends/presets that
  request it;
- preserve existing Linux/Windows preset behavior;
- replace uncontrolled platform source globbing with explicit/conditioned
  source lists where needed;
- isolate Objective-C++ and Apple framework linkage to Apple targets;
- add an arm64 macOS preset;
- build a minimal native macOS application target with GLFW/Cocoa, ImGui, and
  the official ImGui Metal backend;
- load app resources from an executable/app-relative location;
- retain the existing `redgui` NVIDIA target behavior.

Gate:

- existing NVIDIA configure/build still succeeds;
- macOS configures without a CUDA compiler or NVIDIA libraries;
- macOS builds and opens a native Metal-backed ImGui window;
- no production Crimson feature is claimed yet.

### Phase 2 — Backend-Neutral Frame and Presentation Interfaces

Deliverables:

- define logical decoded-frame metadata: stream identity, global/local frame,
  PTS/time base, dimensions, pixel format, pitch/planes, color range/matrix,
  and lifetime/ownership;
- define backend-owned frame-surface and presentation-texture interfaces;
- adapt the NVIDIA implementation without changing behavior;
- add mocks or CPU fixtures for selection/lifetime tests;
- keep CUDA/OpenGL interop out of shared headers.

Gate:

- NVIDIA playback smoke still passes;
- shared frame-selection tests cover exact, nearest-buffered, skipped-present,
  release, and paused behavior;
- both backends can represent NV12 and RGBA/BGRA requirements explicitly.

### Phase 3 — Apple Main-Camera Viewer

Deliverables:

- AVFoundation track loading and native pixel-buffer output;
- Crimson logical clock remains authoritative;
- Metal NV12 limited/full-range conversion using declared metadata;
- native-surface lifetime and bounded buffering;
- main camera pause, resume, display-rate playback, exact seek, and stepping;
- metrics for requested/presented frame, PTS error, repeats, dropped
  presentations, memory, startup, and seek latency.

Gate:

- representative 4512 x 4512, 100 fps video plays at rate 1 without
  accumulating lag;
- paused/seeked frame identity matches the golden fixture;
- color/range comparison is accepted against Crimson/QuickTime references;
- ten-to-twenty-minute thermal and memory run remains bounded.

### Phase 4 — Crop, Stimulus, and Zarr-Aligned Multistream Playback

Deliverables:

- retain one Crimson logical camera clock;
- map camera frames through the same corrected/legacy precedence as current
  `ZarrDetectionLoader`;
- resolve crop and stimulus frame identities without autonomous clock drift;
- support missing and interpolated mappings explicitly;
- exact multistream seeks and paused stepping;
- clipped boundary/handoff behavior;
- composite Metal presentation and measurable per-stream skew.

Gate:

- actual Zarr-mapped fixtures pass;
- displayed frame identities match the mapping, not merely common media time;
- skew is bounded and does not accumulate;
- no periodic hard-seek oscillation or visible stimulus stutter;
- seek settlement reproduces existing Crimson semantics.

### Phase 5 — UI, Rendering, and Overlay Parity

Deliverables:

- migrate full ImGui/ImPlot application surfaces to Metal presentation;
- port main texture, staging/front buffers, crop/rotation, masks, contours,
  boxes, keypoints, skeletons, stimulus, debug readbacks, and required
  multiwindow behavior;
- preserve zoom/pan/coordinate transformations and layer ordering;
- implement Metal compute/render passes where CUDA/GLSL behavior is required;
- avoid synchronous full-resolution readbacks in interactive paths.

Gate:

- overlay coordinates and frame identities match golden fixtures;
- accepted screenshot/image-difference tolerances pass;
- all edit/review interactions operate on the same data and frames as NVIDIA;
- no required panel, overlay, or interaction is omitted.

#### Current Phase 5 Checkpoint (2026-07-15)

Phase 5 remains active and is not the full UI/overlay parity gate. Phase 5I is
accepted for the shared sparse-series contract and the native motion/tail
analysis tabs. macOS now exposes Motion, Eye angles, and Tail in one bounded
analysis window with maintained source/trace defaults, asynchronous TensorStore
pages, current-frame cursors, and plot-click exact seeking. Motion passed the
mounted production and NVIDIA gates. Tail passed real TensorStore-written Zarr
fixtures; production-tail acceptance remains open because neither representative
archive currently contains `analysis/tail_kinematics_runs`. The maintained
Linux/Windows application retains its existing eager timeline loader. The
complete Phase 5I record is in
`docs/crimson_macos_phase5i_motion_tail_timelines.md`; the eye-angle-specific
contract remains documented in
`docs/crimson_macos_phase5h_eye_angle_timeline.md`.

Phase 5J is accepted for the shared stimulus event/interval contract and native
stimulus context workflows. The Mac analysis window now includes compact
stimulus context lanes beside Motion, Eye angles, and Tail plus a rich Stimulus
tab with canonical step details, event-type filters, tooltips, an event list,
and exact camera-frame seeking. The native TensorStore adapter reads the
current columnar event layout and retains read-only compatibility with the
legacy packed structured event-array fallback used by the maintained loader.
Corrected stimulus alignment resolves missing event camera frames without
Python Zarr. The production Mac and NVIDIA probes agreed on the selected run,
33 events, one step, 19 types, and zero unresolved camera frames; both native
GUI smoke paths passed. The mounted production archives are columnar, so a
representative legacy structured archive remains a residual compatibility
risk. The complete Phase 5J record is in
`docs/crimson_macos_phase5j_stimulus_context_timeline.md`.

Phase 5K is accepted for the portable swim-bout candidate/interval contract and
the native Motion review surface. The Mac timeline now selects candidates that
are compatible with the active motion run, track, and speed variant; renders
inclusive outer/core bout spans; and exposes a lazy, extrema-preserving
detector-response trace without presenting it as physical speed. The native
TensorStore adapter reads compact tabular v2 and hierarchical v1 archives,
preserves candidate and detector provenance, and uses generation-cancelled,
bounded asynchronous pages. Both representative mounted archives passed the
Mac probes, the required native production smoke passed, the server-local
probe agreed with Mac, the isolated NVIDIA build passed all portable tests,
and the maintained authenticated GUI smoke passed. The complete Phase 5K
record is in `docs/crimson_macos_phase5k_swim_bout_timeline.md`.

Phase 5L has now closed maintained read-only workspace and visual parity on Mac
and the isolated Linux/NVIDIA build. Remaining Phase 5 work includes incremental
decomposition of stable read-only responsibilities from the monolithic
`ZarrDetectionLoader`, stimulus overlay parity beyond the event/context
surfaces, production-tail data acceptance, and eventually the edit/review
interactions deferred below. Phase 5 must not be marked complete until those
workflows and the screenshot/image-difference gate are satisfied.

#### Revised Phase 5L — Maintained Workspace Parity

Phase 5L established workspace parity before edit/write implementation. It
first captured and inventoried the running maintained UI, then reproduced its
actual window topology, primary camera composition, transport, menus, stable
panels, styling, control density, and lifecycle behavior on Mac.

Phase 5L is restricted to stable playback and read-only contracts. It must not
define a provisional Zarr write schema or implement bounding-box, refined
keypoint, subject-mask, detection-acceptance, or other scientific mutations.
Write-dependent commands may remain disabled where needed to preserve the
maintained workspace structure. Edit/review implementation resumes only after
the shared storage and mutation contracts stabilize.

The maintained source currently exposes independent ImGui windows rather than
an active ImGui dockspace. The audit must match that observed topology and must
not introduce docking as an assumed parity requirement. GLFW window/input
integration and Metal rendering are compatible with the maintained UI
composition; the rendering backend does not justify a different application
layout.

The Phase 5L gate requires a source-linked window/command inventory, fixed-size
Linux/Windows reference captures, structural assertions for window presence,
labels, defaults, ordering, and content bounds, region-based screenshot
comparisons with documented masks/tolerances, stable workflow tests, Mac and
NVIDIA builds, and interactive production smokes. It also requires evidence
that no write repository was opened and no archive mutation occurred. The full
plan is in `docs/crimson_macos_phase5l_workspace_parity_plan.md`.

Phase 5L.0 source and Linux runtime evidence is recorded in
`docs/crimson_macos_phase5l_workspace_inventory.md` and
`docs/reference/phase5l/manifest.json`. The maintained Linux build now has a
read-only exact-frame/panel/front-buffer hook and deterministic references for
workspace, overlays, live crop, stimulus debug, and alternate eye analysis.
Both final suites pass (34 macOS tests and 24 isolated NVIDIA tests), and the
archive mtime is unchanged. Production tail evidence remains data-gated because
the reference archive has no tail kinematics. Windows runtime evidence remains
an explicit open gate because no real Windows host was available; Linux
captures are not accepted as a substitute.

Phase 5L.1 portable workspace state is complete and documented in
`docs/crimson_macos_phase5l_portable_workspace_state.md`. The shared C++17
contract now owns capability-derived window submission, stable command
enablement, source/representation/event selections, versioned restoration, and
playback intent while existing clocks, repositories, decoder rings, and GPU
resources remain in their backend owners. The current checkpoint passes 34/34
Mac headless tests, the native Mac Metal 0:300 playback smoke, 24/24 isolated
NVIDIA tests, and the authenticated NVIDIA 0:300 playback smoke. Windows
build/runtime validation is deliberately deferred until the new Windows laptop
is used.

Phase 5L.2 Mac workspace composition is complete and documented in
`docs/crimson_macos_phase5l2_workspace_composition.md`. The Mac shell now
matches the maintained independent-window topology, Classic theme,
Roboto/Fork Awesome typography, first-use role geometry, primary camera
transport, paused buffer inspection, crop/stimulus presentation, read-only
overlays, and separate stimulus/analysis timelines. Metal targets the live
ImGui content rectangles without synchronous full-resolution readback. Mac
passed 35/35 headless tests plus acquisition and live-geometry multistream
smokes; the cumulative isolated NVIDIA build passed 24/24 tests and the
authenticated 0:300 playback smoke. Windows remains explicitly deferred.

Phase 5L.3 stable window and workflow coverage is complete and documented in
`docs/crimson_macos_phase5l3_stable_workflows.md`. The Mac shell now wires the
stable file/session, transport, camera, buffer inspection, Frame Inspect,
crop/stimulus, timeline, diagnostics, help, error, and lifecycle workflows.
Controls without stable read-only adapters and every mutation command remain
disabled. Mac passed 36/36 headless tests plus acquisition and live-geometry
multistream smokes with zero presentation skew; the cumulative isolated
NVIDIA build passed 25/25 tests and the authenticated 0:300 playback smoke.
Windows remains explicitly deferred. Phase 5L.4 visual and cross-platform
acceptance followed and is recorded below.

Phase 5L.4 visual and cross-platform acceptance is complete and
documented in `docs/crimson_macos_phase5l4_visual_acceptance.md`. Equivalent
Mac and isolated NVIDIA captures now share exact frames, logical dimensions,
window roles, semantic labels, control order, and stable read-only markers.
The deterministic comparator passes 46/46 structural and clean-camera raster
checks; Mac passes 36/36 tests, while the isolated NVIDIA build passes 26/26
tests and its authenticated 0:300 production smoke. Controlled portable and
Metal fixtures enforce the Phase 5 overlay geometry and raster thresholds. Mac
also passes both 1024:7024 multistream production modes, and all six final Mac
captures were refreshed from the current executable with valid checksums.
Windows runtime remains the explicit user-approved deferral; no result is
inferred for it.

#### Phase 5M — Read-Only Zarr Boundary and Chaser-Polar Pilot

Phase 5M begins an incremental, feature-scoped decomposition of
`ZarrDetectionLoader`. It must not become a big-bang loader rewrite. The
existing loader remains a maintained Linux/Windows compatibility facade for
unmigrated behavior, while newly extracted stable read-only slices receive
portable scientific types, explicit provenance and missing-data semantics, a
TensorStore repository adapter, a legacy compatibility adapter, shared
fixtures, and platform render adapters.

The chaser-distance polar inset is the pilot. The representative recording has
a complete 140,035-frame, two-chaser production dataset, while the maintained
implementation currently couples run/component selection, eager array loading,
camera-frame lookup, angle assumptions, radial scaling, color precedence, and
ImGui drawing through concrete `ZarrDetectionLoader` types. Phase 5M must make
those rules explicit without changing their accepted behavior.

Deliverables:

- a responsibility/consumer inventory and migration ledger for
  `ZarrDetectionLoader`;
- characterization tests for polar run selection, schema compatibility,
  frame lookup, missing/invalid samples, conventions, colors, and radial scale;
- portable descriptor, exact-frame sample, repository, availability, and scene
  contracts with units, conventions, provenance, and frame identity;
- read-only TensorStore and maintained-loader adapters that agree on the same
  fixtures;
- one shared polar scene consumed by maintained ImGui and Mac Metal
  presentation; and
- dependency enforcement preventing new shared or Mac code from consuming the
  monolithic loader directly.

Gate:

- both adapters agree on production and synthetic fixture values;
- unsupported coordinate/angle conventions fail explicitly;
- unavailable, missing-frame, valid-empty, ready, and failed states remain
  distinguishable;
- shared scene/UI code has no storage, Metal, CUDA, OpenGL, or concrete-loader
  dependency;
- equivalent Mac and NVIDIA renders pass Phase 5 tolerances;
- Mac and isolated NVIDIA builds/tests and production smokes pass; and
- no archive mutation, write schema, or unrelated loader migration occurs.

The detailed plan is in
`docs/crimson_macos_phase5m_read_only_zarr_boundary_plan.md`.

Phase 5M.0 responsibility and behavior inventory is complete and documented
in `docs/crimson_macos_phase5m0_zarr_loader_inventory.md`. The actual legacy
loader now has synthetic characterization coverage for selection, schema,
lookup, invalid samples, colors, and radial scaling; its 28-file direct-include
baseline is enforced without admitting new Mac consumers. A production
GoodCopBadCop descriptor and exact-frame samples are recorded under
`docs/reference/phase5m`. The characterization also exposed and removed global
NVIDIA-side C++ `-Ofast`/`-ffast-math`, restoring the finite-value checks used
throughout scientific data paths. Mac passed 37/37 headless tests; the isolated
NVIDIA build passed 28/28 tests and its authenticated 0:300 production smoke.
That completed the Phase 5M.0 checkpoint.

Phase 5M.1 portable polar contracts are complete and documented in
`docs/crimson_macos_phase5m1_portable_polar_contract.md`. Backend-neutral
descriptor, exact-frame sample, point, provenance, availability, convention,
color, and radial-scale types now sit behind an abstract read-only repository
interface. Focused tests cover the production descriptor and frame values, all
six availability states, exact-frame enforcement, convention rejection,
validity filtering, color precedence, and radial scaling. The macOS arm64
Release app passed 40/40 tests; the isolated Linux/NVIDIA build passed 29/29
tests and its authenticated GoodCopBadCop 0:300 production smoke. First-party
compile commands use ordinary Release optimization without `-Ofast` or
`-ffast-math`. Repository-controlled TensorStore dependency builds now strip
bundled `dav1d`'s upstream fast-math option, and the provided OpenCV build
helpers disable their fast-math options. Phase 5M.2 storage adapters are the
next checkpoint; the full Phase 5M gate remains open.

Phase 5M.2 polar storage adapters are complete and documented in
`docs/crimson_macos_phase5m2_polar_storage_adapters.md`. The native path uses a
read-only TensorStore repository with bounded radial scanning, exact one-row
matrix reads, and a bounded asynchronous lookahead/cache. The maintained path
adapts `ZarrDetectionLoader` to the same portable descriptor and frame sample
while preserving latest-complete selection, compatibility fallback, duplicate
frame, color, sparse-identity, and radial-scale behavior. Synthetic fixtures
compare both adapters field by field, and the production GoodCopBadCop
comparison passed at frames 0, 56, 1024, 7024, and 140034 with unchanged polar
and stimulus metadata fingerprints. Repository-provided OpenCV build helpers
now disable fast-math as well, and repository-built TensorStore dependencies
strip bundled `dav1d`'s upstream fast-math option. The generated macOS and Linux
compile commands contain neither `-Ofast` nor `-ffast-math`. The complete macOS
arm64 Release build passed 42/42 tests. The isolated Linux/NVIDIA build compiled
`redgui`, passed 31/31 tests, and passed its authenticated GoodCopBadCop 0:300
production smoke in 2.9958 seconds. The post-smoke chaser-distance metadata
fingerprint matched all 2,538 pre-run stat and SHA-256 records. Phase 5M.3
shared scene and platform presentation is the next checkpoint; the full Phase
5M gate remains open.

Phase 5M.3 shared scene and platform presentation is complete and documented
in `docs/crimson_macos_phase5m3_shared_polar_scene.md`. One backend-neutral
scene now owns the inset's ordered background, rings, orientation, points,
colors, labels, readout, opacity, sizing, and clipping semantics. The maintained
camera view resolves portable samples through the legacy repository and draws
that scene through a thin ImGui adapter; the native Mac path resolves exact
frames through the read-only TensorStore repository and bounded buffer, then
draws the same vector scene through Metal and its text through ImGui. Shared
camera presentation no longer consumes the loader's concrete polar-frame type.
The macOS arm64 Release suite passed 43/43 tests, including an offscreen Metal
polar raster test, and a deterministic native app smoke rendered four polar
presentations with eight points, two ready resolves, and zero failures. The
isolated Linux/NVIDIA build passed 32/32 tests and its authenticated production
0:300 smoke in 2.99164 seconds. All 4,908 size, modification-time, and SHA-256
records in the production chaser-distance subtree matched before and after.
The production archive was not mounted on the Mac, so equivalent same-frame
production captures and the final cross-platform visual comparison remain the
Phase 5M.4 checkpoint; the full Phase 5M gate remains open.

Phase 5M.4 cross-platform acceptance and the full Phase 5M gate are complete
and documented in
`docs/crimson_macos_phase5m4_cross_platform_acceptance.md`. The maintained
legacy adapter and read-only TensorStore adapter agree on synthetic fixtures
and on current production run `chaser_distance_v1_20260718`, component
`egocentric_bearing_v1_20260718`, at frames 0, 56, 1024, 7024, and 140034.
Equivalent frame-1024, 1920x1080 maintained OpenGL and Mac Metal captures use
the same 156x213 shared scene; the acceptance comparator passed all 20 checks,
including exact scene semantics and descriptor provenance, 1.0 vector
coverage, zero opaque channel delta, and a maximum 0.2613-pixel raster anchor
delta. The final Mac suite passed 43/43 tests and its 1024:1324 production
smoke reached frame 1324 with zero polar failures. The isolated NVIDIA suite
passed 32/32 tests and its authenticated 1024:1324 smoke passed in 2.99145
seconds. Generated compile commands on both platforms contain no `-Ofast` or
`-ffast-math`; all 10,736 production chaser-distance files retained identical
paths, sizes, modification times, and SHA-256 hashes. Phase 5N completion is
recorded below.

#### Phase 5N — Remaining Stimulus Overlay Parity

Phase 5N applies the Phase 5M extraction pattern to maintained camera-view
stimulus event overlays and step-direction indicators. It must reuse the
existing corrected/legacy alignment precedence and portable stimulus timeline
contracts, preserve exact presented-camera-frame authority and layer order,
and feed common scene semantics to maintained and Metal renderers.

Gate:

- event and step overlays resolve from the exact presented camera frame;
- corrected alignment and declared legacy fallback produce characterized,
  tested results;
- controls, geometry, labels, colors, and layer order match the maintained
  reference within Phase 5 tolerances;
- Mac and NVIDIA deterministic tests and production smokes pass; and
- the implementation remains read-only and introduces no provisional storage
  or edit contract.

Status: complete on 2026-07-18. The portable scene and maintained compatibility
adapter preserve corrected-first alignment with characterized legacy fallback,
exact presented-frame authority, sticky event labels, moving-grating direction
geometry, colors, controls, and explicit layer order. The production legacy
adapter and TensorStore adapter agree at frames 0, 56, 1024, 7024, and 140034.
Equivalent frame-1024 Linux/OpenGL and Mac/Metal captures passed all 17
acceptance checks with identical semantic signatures and 1.0 panel-border
raster coverage. The final Mac suite passed 44/44 tests, the isolated NVIDIA
suite passed 34/34 tests, and both 1024:1324 production smokes reached exact
frame 1324. Generated compile graphs contain no fast-math flags. All 6,950
files under the production stimulus-run and event-enum subtrees retained
identical paths, sizes, mtimes, and SHA-256 hashes. Full evidence is in
`docs/crimson_macos_phase5n_remaining_stimulus_overlay_parity.md`.

#### Phase 5O — Shared Bounded Data Access and Scheduling

Phase 5O is the next planned checkpoint. It combines the native Mac path's
bounded TensorStore range reads with the maintained Linux path's demand-first
asynchronous prefetch and cache-only playback policy. Large analysis products
must page through a portable range contract instead of being implicitly
materialized in full, while explicitly budgeted small products may retain the
maintained full-series preload optimization.

Deliverables:

- portable source/range, field-selection, priority, generation, result-state,
  cancellation, and byte-budget contracts;
- one application-owned analysis I/O scheduler with bounded concurrency,
  demand-first priority, direction-aware read-ahead, duplicate suppression, and
  seek/reload cancellation;
- lazy archive adapters that do not synchronously read million-row optional
  mappings before the application shell can remain responsive;
- byte-accounted CPU/GPU caches with storage-specific retained forms, including
  sparse exact mask pixels and bounded persistent frame-index blocks;
- an explicit hybrid policy for budgeted full-series preload versus paged range
  access; and
- maintained-loader compatibility adapters and cross-platform production
  measurements over local storage and mounted PRFS.

Gate:

- optional analysis cache misses never block video presentation;
- paused exact-frame inspection settles deterministically or reports a clear
  unavailable/failed state;
- cache memory remains bounded and observable during long playback and random
  seeks;
- current demand outranks speculative work and stale generations are discarded;
- Linux, Windows, and macOS share the portable scheduling contract without
  changing frame, coordinate, provenance, schema, or write semantics; and
- small-series preload and large-series paging follow one documented, tested
  byte-budget policy.

The detailed direction and staged rollout are in
`docs/crimson_macos_phase5o_bounded_data_access.md`.

Phase 5O.0 is in progress. Portable subject-mask repository and presentation
metrics now separate archive-open stages, individual mapping columns, decoded
and retained bytes, mask reads, sparse conversion, contours, and cache
retention. Mounted-PRFS probes show that the 1.18-million-row Sleepyfish mask
repository spends 17.9 seconds of an 18.6-second warm open reading mappings;
its three 1,024-row-chunked subject columns each take 5.4-5.7 seconds, while the
larger 16,384-row-chunked crop mapping reads complete much faster. The first
dense chunk read 134.5 MB and retained 3.24 MB after sparse conversion. After
lazy mapping, the same repository opens in 3.4 seconds and moves its compact
index and exact mapping pages to first demand. Linux, local-storage, timeline,
traversal, and long-run memory baselines remain open. Evidence and metric
semantics are in
`docs/crimson_macos_phase5o0_data_access_characterization.md`.

Phase 5O.1 is complete. Backend-neutral source/range, field, priority, access-
pattern, generation, result-state, cancellation, and CPU/GPU byte-budget types
now have deterministic portable tests. The bounded queue enforces demand-first
ordering, deduplication/promotion, generation cancellation, and capacity
pressure. The weighted LRU cache enforces byte and item budgets while retaining
active and higher-priority entries. `DataAccessQueue` is the policy core, and
`DataAccessScheduler` now adds the bounded worker layer used by the first Phase
5O.3 adapters. Details are in
`docs/crimson_macos_phase5o1_portable_data_access_contract.md`.

Phase 5O.2 is in progress. Subject-mask runs with `frame_counts` now avoid full
mapping-column reads at repository open, create a compact count/prefix index on
first demand, and page exact subject/crop mapping chunks through 8 MiB and
16 MiB caches. Exact row metadata is validated before publication; unordered
legacy rows have a capped fallback index, and runs without `frame_counts` keep
the eager compatibility path. Malformed inventories fail once without retaining
a partial index. Post-migration PRFS probes are now recorded; remaining
repositories and RLE/ragged metadata are not yet migrated.

Motion and tail TensorStore timelines now persist their 16,384-row frame-index
blocks across windows in a 2 MiB/16-block cache. Filtered, smoothed, raw, and
averaged motion variants share the cache when they share one row mapping. The
repository and Mac shutdown summary expose block reads, warm hits, evictions,
source bytes, current/peak retained bytes, and maximum read latency.

The native Mac archive-open path now publishes archive readiness separately
from optional products. After archive validation, independent repository jobs
run through the shared bounded scheduler; the GUI enters the video workspace
and adopts each completed product between render frames. The small motion,
eye-angle, and tail preload operations remain one serialized job so their shared
128 MiB budget is enforced. Initial video prebuffer waiting uses the same
responsive event/render loop. Cancellation is cooperative; active TensorStore
calls remain non-preemptive.

The first full Sleepyfish trial remained interactive but required 154.9 seconds
to publish the former repository bundle: keypoints took 53.0 seconds, subject
shape 59.5 seconds, and eye geometry 31.9 seconds. Maintained keypoint runs with
`frame_counts` and explicit `source_crop_row_ids` now retain TensorStore handles
plus a validated frame-prefix index, read exact keypoint/crop rows through a
scheduler-backed frame cache, and never resolve a cache miss on the GUI thread.
Older keypoint layouts retain the eager compatibility path. Subject-shape and
eye-geometry lazy adapters remain open work.

A subsequent mounted Sleepyfish run verified four-way asynchronous repository
opening (`11` accepted jobs, four peak-active workers) and completed the product
set in about 111.6 seconds. Keypoints, masks, eye geometry, and subject shape
still each required roughly 81--99 seconds while overlapping. The archive's
keypoint `frame_counts` contains only 1,188,000 `int32` values in ten outer
shards, while subject-mask contour metadata alone took 49.4 seconds to open.
This identifies high-latency per-array metadata access and the remaining eager
million-row adapters, rather than GUI-thread serialization, as the next
bottleneck. The next storage-facing contract work is consolidated run metadata,
persisted frame-row offsets, and lazy subject-shape/eye-geometry handles.

Interactive Zarr sessions now remain paused while a responsive analysis-loading
modal reports initialization. Completion closes the modal but does not start
playback; the user starts playback explicitly. The modal is not dismissible
while initialization is running. First-frame analysis demands remain gated
until every scheduled product reaches a terminal state, so large mask reads do
not compete with repository opening by default. Optional products may finish
unavailable, while failure of the required archive blocks session readiness.
The modal now consumes a backend-neutral loading presentation model and is
rendered by a shared ImGui component rather than by the Metal composition root.
Recording-open lifecycle is shared as well: a portable controller now owns the
active transaction, product timing, cancellation/failure transitions, and
generation-checked settlement of the analysis loader. Both the Metal and
NVIDIA application shells use it, but each shell still owns its storage,
decoder, repository, threading, and GPU operations. The slice is covered by a
portable contract test and has compiled in both the macOS application and an
isolated NVIDIA `redgui` build.

Playback transport is also backend-neutral. The shared controller owns the
logical frame clock, play/pause/seek/step/rate transitions, readiness gating,
frame clamping, and end-of-stream pause. Metal and NVIDIA use that state while
retaining their decoder, buffer, exact-seek, stimulus, and GPU adapters. The
portable transport tests pass on macOS and Linux; real playback smokes reached
frame `300` on both the Metal application and the isolated NVIDIA build. This
is a bounded Phase 3 controller extraction and does not move coordinate or
storage behavior into the playback layer.

Phase 5O.3 is in progress. Native repository initialization, keypoints, subject
masks, motion, and tail now share one application-owned 64-request scheduler on
macOS. Four workers provide bounded cross-product concurrency; one source cannot
consume multiple workers. Current frame demand outranks initialization and
lookahead, stale generations are cancelled, and deterministic tests cover
archive-first/product-level publication, demand reservation, source isolation,
keypoint cache reuse, seek invalidation, and promotion-aware timing attribution.
Bounded queue-wait and callback-service aggregates are now available by priority
and source in macOS shutdown diagnostics and benchmark JSON. A mounted
full-archive trace measured a 28.99-second current-frame wait behind four long
initialization callbacks. The portable scheduler now reserves one current-frame
slot in the four-worker application pool; the matching trace reduced maximum
queue wait to 0.98 ms with zero traversal deadline misses. Active work remains
non-preemptive. Byte-weighted in-flight admission, remaining buffer migrations,
and most Linux/Windows adapters remain open. The shared Linux/Windows entry
point now owns an equivalent `64`-request, four-worker scheduler with one
reserved current-frame worker. Refined subject-mask current chunks and
lookahead are the first compatibility workload migrated into it; discontinuous
seeks advance the source generation, stale queued lookahead is cancelled, and
standalone loader consumers retain the old fallback worker when no scheduler is
injected. Apple and NVIDIA shutdown logs use one backend-neutral diagnostic
formatter. Native Windows execution is not yet validated. Evidence is in
`docs/diagnostics/scheduler_current_frame_reservation_2026-07-27.md` and
`docs/diagnostics/phase5o_nvidia_scheduler_adoption_2026-07-29.md`.

Phase 5O.4 is in progress. The native Mac loader shares a 128 MiB preload budget
across the selected default motion, eye-angle, and tail series. Repositories
retain native-precision arrays only when the complete candidate fits the
remaining budget; otherwise they preserve bounded TensorStore paging and frame-
index caches. Motion preload includes its selected position, speed, heading,
mask, and time fields, while eye-angle preload includes the complete selected
default representation and its optional frame times. Non-default variants and
eye-angle representations remain paged. Repository metrics and headless tests
distinguish resident and paged resolution paths. Refined subject masks now use
the shared scheduler from the maintained Linux/Windows application, while its
other compatibility adapters and representative PRFS measurements remain
open.

The shared native `ArchiveContext` now adds one bounded 64 MiB TensorStore cache
pool per open archive. Every maintained TensorStore repository uses one
read-only array-spec policy with metadata and data revalidation scoped to array
open, matching the immutable published-run contract. This permits reuse of
decoded inner chunks and Zarr v3 shard indexes across repository reads while
keeping driver memory bounded and distinct from application page/sparse/GPU
caches. A headless indexed-sharding test proves that an identical second read
adds no file request or transferred bytes, and the Mac archive trace reports the
active pool and policy. A mounted Sleepyfish Cam2010095 frame probe confirms
production compatibility with the 64 MiB pool. A headless mounted-PRFS control
over the sharded keypoint `frame_counts` layout shows why a nonzero driver cache
is required: 700 adjacent two-value reads fell from 1,400 file operations and
107,800 compressed bytes at zero cache to two operations and 154 bytes at
64 MiB; 100 fixed random reads fell from 200 operations/22,844 bytes to 70
operations/6,479 bytes. A complete eager read remained 11 operations and 7,428
bytes in both modes. The control is a storage-layout proxy for future persisted
frame-row offsets, not an end-to-end playback trace. Application-level
traversal, random seek, cache pressure, and long-run memory measurements remain
open before accepting 64 MiB as the final budget.

The Phase 5O.4 canonical-detection consumer benchmark is complete and recorded
in `docs/crimson_macos_phase5o4_canonical_detection_storage_benchmark.md`.
Crimson's bundled TensorStore accepted Palette's exact regular and access-aware
hybrid Zarr v3 stores, including indexed sharding, Zstandard payloads, CRC32C
shard indexes, exact declared dtypes, and inline consolidated metadata. A
balanced 20-process mounted-SMB matrix compared zero-byte and 128 MiB driver
caches across five matched repetitions. All decoded values matched, offsets
were read and retained exactly once, UI random-frame tail latency stayed below
150 ms, and storage traversal exceeded 1,400 FPS. The hybrid reduced zero-cache
UI and contract traversal file bytes by about 7.2-7.5 times. The profile is not
promoted: the one-time offset median exceeded 100 ms in three conditions, and
first-pass eight-column random reads had cross-repetition p95 outliers above
150 ms in every condition. The follow-up 80-process mounted-Mac cache/read-
ahead sweep is also complete. Every frozen deadline, cancellation, stale-
publication, transfer-waste, concurrency, retained-offset, and RSS gate passed.
A 16 MiB cache reduced first-phase hybrid transfer from 3,826,500 to 76,530
bytes; 32, 64, and 128 MiB did not reduce it further. Zero speculative read-
ahead had no post-warmup misses while the bounded presentation cache evicted
normally. The next paired full-analysis fixture therefore uses 16 MiB with
only a one-page asynchronous demand lead. This does not change production's
64 MiB cache yet. Process-first offset initialization remained variable at
135.2 ms median and 143.3 ms p95. The full result is in
`docs/crimson_macos_phase5o4_prefetch_cache_benchmark.md`.

The paired full-analysis contract is now frozen in
`docs/crimson_macos_phase5o4_full_analysis_fixture_contract.md`. Its first
stage compares otherwise-identical regular and hybrid archives with the
production 64 MiB cache; its second stage compares 16 MiB and 64 MiB only after
the hybrid passes. Exact fixture identity, simultaneous products, recording
association, deterministic workload, consolidated-discovery scope, and numeric
startup/overlay/memory/transfer/deadline gates are fixed before publication.
The normal macOS application now has a canonical-detection repository,
bounded 70-frame presentation pages with one asynchronous lead page, a shared
overlay-scene adapter, and a fail-closed `--detection-run` override. Palette's
2,048-frame regular/hybrid integration pair is now published, and Crimson's
fresh-process integration gate accepts both through required-product readiness,
all first presentations, full fixture traversal, shared overlay construction,
and seek cancellation. Both layouts produced the same 2,048-detection digest,
used exact typed opens and one offsets read, and published no stale work. This
small pair is compatibility evidence only because its canonical arrays each fit
in one inner chunk. Real Metal smokes also passed both candidates through frame
300 with zero skipped or late frames. The full-duration five-process-per-layout
Stage 1 matrix is also complete. Every correctness, exact-selection,
one-offset-read, cancellation, and 700 FPS deadline check passed, but the frozen
promotion gate failed first-overlay latency, absolute 2 GiB peak RSS, and the
required `0.25x` hybrid traversal-byte ratio. The observed hybrid ratio was
`0.606x`; median total bytes were `0.987x` and median Ready time was `1.011x`
regular. The first detection page spent about 51-52 seconds queued behind four
non-preemptive initialization jobs even though its repository read/decode took
well under one second. Stage 2 does not run and the physical profile remains
unpromoted. Integration evidence is in
`docs/diagnostics/crimson_macos_phase5o4_full_archive_integration_2026-07-26.md`;
the full-duration verdict is in
`docs/diagnostics/crimson_macos_phase5o4_full_duration_stage1_2026-07-26.md`.

Phase 5O.5 completed the byte-budgeted residency strategy gate. The original
25-store physical-layout matrix is cancelled and the reduced three-candidate
matrix remains deferred: background residency passed its readiness, deadline,
cancellation, stale-publication, RSS, and maintained-product gates on the
existing full-duration fixtures. Paging remains the scalable fallback, while
small decoded UI working sets may be promoted atomically after first-page
readiness. The contract and evidence are in
`docs/crimson_macos_phase5o5_detection_residency_gate.md`.

Native macOS production activation is now complete. The selected canonical or
refined detection repository starts a speculative resident build only after a
real first page is available and only when its exact decoded UI hot set fits a
64 MiB budget. Builds use 512 KiB chunks; current-frame work retains its
reserved scheduler capacity, and rejection, cancellation, or failure remains
paged and nonfatal. A mounted full-duration activation run kept current-frame
queue wait below 0.15 ms, published one exact 28,490,088-byte snapshot, and had
zero stale publications or traversal misses. Evidence is in
`docs/diagnostics/canonical_detection_production_residency_activation_2026-07-28.md`.

The first mounted long-running acceptance checkpoint now passes. Twenty
deterministic cycles exercised simultaneous maintained products, rapid seek
cancellation, and alternating 3,500-frame traversals distributed across the
full Sleepyfish frame domain. RSS and reported retained allocation curves both
plateaued after warmup; mask mapping pages reached their bounded eviction
regime at about 16.6 MiB, and mask payload chunks evicted continuously without
unbounded retention. There were zero misses across 980 post-warmup traversal
pages, zero stale publications, zero offset rereads, and no scheduler failures.
The portable workload/plateau core also passed in an isolated Linux build, and
the maintained NVIDIA application linked successfully. Native Windows and
multi-process release acceptance remain open. The checkpoint is in
`docs/diagnostics/full_archive_endurance_2026-07-29/`.

Phase 5O.6 is now in progress for Palette refined-detection v1 consumption.
The backend-neutral repository, fail-closed refined-first selector, stable row
identity, lazy source-audit boundary, paging/residency integration, shared
overlay propagation, and macOS session wiring are implemented and covered by
headless synthetic tests. Normal explicit refined selection requires selector
eligibility; selector-ineligible shadow inspection uses the separate
`--benchmark-refined-detection-run` API. Legacy mutable editing remains a
separate compatibility path. A portable real-shadow harness is also complete.
Crimson adopted Palette's narrow Zarr-Python group comparator: absent, null,
and exact empty-inline nested consolidated declarations are equivalent for
group nodes only; arrays and all other fields remain exact. The unchanged real
Palette handoff passed typed open, selection, full traversal, identity,
overlay, cancellation, lazy-audit, and residency checks. The deterministic
suite covers the required `[2, 0, 1, 3]` raw/manual frame pattern. The result is
bound to clean immutable Crimson implementation commit
`28537f64bcae765b062374b17dd879c0a9614ade`. The paired
regular/access-aware refined-snapshot physical gate is now complete. Five
fresh mounted-macOS processes per layout passed every correctness, readiness,
current-frame, cancellation, deadline, RSS, transfer, and shutdown gate at
clean Crimson commit `9cf04acee9682a6f4f5fae005c0af6077ec5cc4b`.
Access-aware transferred median `0.132x` traversal bytes and `0.315x` total
process bytes relative to regular, with exact paired logical digests and zero
post-warmup deadline misses. Crimson recommends the unchanged access-aware
profile for Palette's separate versioned promotion; Crimson does not activate
Palette profiles itself. Evidence is in
`docs/diagnostics/refined_detection_physical_profile_canary_2026-07-27/`.
The implementation boundary is documented in
`docs/crimson_refined_detection_v1_consumer.md`.

The shared coordinate foundation is now implemented independently of Metal,
OpenGL/CUDA, TensorStore, and ImGui. Camera overlays explicitly present
`source_camera_continuous_pixels`; the canonical/refined detection adapter uses
the shared normalized-center-size to half-open-XYXY transform and validates
source dimensions. Strict ROI placement requires source dimensions plus exact
crop manifest and policy digests. Legacy keypoint/crop names remain behind
compatibility adapters pending persisted provenance and the movement-trail
policy. The contract is in `docs/crimson_shared_coordinate_contract_v1.md`.
Palette's persisted coordinate catalogs at commit `154d7888` have also passed
Crimson's static cross-language digest, vocabulary, binding, tampering, and
transform review. Refined run-manifest v2 validation is implemented without
changing refined v1. Palette's selector-ineligible canonical-v3/refined-v2/
crop-v2 mounted canary is now complete and accepted. Crimson validated the
handoff hash, exact manifests and dtypes, consolidated declarations, retained
offsets, lazy refined source audit, crop lineage and pixel authority, and both
frozen coordinate samples at clean implementation commit `ce478c7d`. The
normalized float32-to-double projection differed by only `0.000109` pixel and
passed the `0.001`-pixel cross-language tolerance; ROI-to-source placement was
exact. No selector, registry, writer default, production archive, or canary was
modified. Palette owns any later production activation. The static review and
mounted result are in
`docs/diagnostics/coordinate_catalog_cross_language_review_2026-07-28.md` and
`docs/diagnostics/coordinate_catalog_canary_2026-07-29/README.md`. The
Palette-facing decision is in
`docs/coordinate_catalog_palette_acceptance_handoff_2026-07-29.md`.
The separate backend-neutral crop-v2 read harness is now complete. It validates
all 13 exact declarations without dtype probing, retains the offsets after one
read, exercises concurrent five-field UI reads, random seeks, 70-frame windows,
and cancellation, records TensorStore file/cache and RSS telemetry, and proves
that geometry-only access never opens `roi_images`. The mounted coordinate
canary passed at clean implementation commit `e972cef`; the result is in
`docs/diagnostics/crop_geometry_v2_read_benchmark_2026-07-29/`. This remains an
integration checkpoint rather than storage-profile promotion evidence. The
same frozen workload must be rerun against Palette's later persistent
publisher-produced candidate.

The first production-shaped full-duration memory-attribution checkpoint is
also complete at Crimson commit `1ba2ba3`. A portable current/peak RSS sampler
and repository-owned retained-byte contract passed the full macOS suite and an
isolated Linux build. The mounted 1,188,000-frame hybrid/resident run peaked at
1.764 GiB; 631.9 MiB of the 1,062.8 MiB pre-shutdown RSS was explicitly
attributed. Crop geometry, subject shape, and eye geometry account for about
468 MiB, while detection residency and its page cache account for 27.4 MiB.
Repository release reduced RSS to 343.6 MiB. This changes the next memory
priority from detection/cache tuning to compact, pageable geometry placement
and frame-index representations. The result remains one diagnostic trial, not
a leak, thermal, or promotion gate. Evidence is in
`docs/diagnostics/full_duration_memory_attribution_2026-07-29/`.

The first compact geometry slice is now complete at implementation commit
`9e5dd54c`. Crop geometry no longer retains a row object plus hash-map entry
for every frame; it uses one dense frame-offset index and compact resident
columns behind the same backend-neutral repository API. Empty frames, gaps,
unsorted input, and multiple rows per frame are covered while legacy inset
presentation retains its stable first-row behavior. The full macOS suite and
an isolated Linux CUDA/NVIDIA `redgui` build passed. On two clean mounted
full-duration trials, the crop repository lower bound fell reproducibly from
199.9 MiB to 62.6 MiB, a 137.3 MiB / 68.7% reduction. Required-ready process
RSS was 174.8--187.1 MiB lower, while later lifetime peaks remained too
variable to attribute to this owner. Payload remains resident: the next step
is direct compact construction followed by a byte-budgeted pageable policy for
larger multi-instance data, never a synchronous GUI-thread storage read.
Evidence is in
`docs/diagnostics/compact_crop_geometry_memory_2026-07-29/`.

The direct-construction follow-up is complete at commit `bd62080`. Strict
crop-v2 now retains its validated persisted offsets and converts exact typed
arrays into final compact columns sequentially. The legacy reader now uses
flat fixed-width matrices and derives offsets without constructing a
million-row object vector. Two clean mounted trials reduced crop-product
initialization from 7.62--8.23 seconds to 1.26--1.37 seconds and RSS at crop
readiness from 1,516.6--1,534.7 MiB to 739.1--777.3 MiB. The retained crop
lower bound stayed at 62.6 MiB, proving this removes temporary construction
memory rather than changing the resident payload. Later whole-process peaks
remained dominated by other products and allocator/cache timing. The full
macOS suite and isolated Linux CUDA/NVIDIA build passed. Evidence is in
`docs/diagnostics/direct_crop_geometry_construction_2026-07-29/`.

The selector-ineligible keypoint-v2 consumer checkpoint is now complete on
macOS and isolated Linux. One backend-neutral repository validates Palette's
exact raw-v2, quality-v1, refined-v2, and body-frame-v1 manifests and
declarations, retains the selected frame offsets once, preserves observation
identity and refined decision state, leaves quality payloads lazy, and uses only
bound body-frame heading. Compact page columns are issued concurrently through
TensorStore and publish through the existing shared keypoint repository/scene
boundary. Both mounted handoffs passed, including the three refined
correction/rejection/recovery cases; the GUI playback smoke, all 65 macOS tests,
the Linux portable and mounted-store gates, and the full CUDA/NVIDIA `redgui`
build passed. Linux timings use the on-site `/groups` path and are not compared
with the Mac's Wi-Fi/VPN mount timings. Production selection, edit/write
behavior, source-matched visual evidence, and a long-recording physical-profile
gate remain open. The checkpoint is documented in
`docs/crimson_keypoint_v2_consumer.md`.

The deterministic long-duration keypoint-v2 harness is now implemented, and
the shared presentation cache has direction-aware reverse lookahead. The
versioned headless workload measures first readiness, random and 70-frame
forward/reverse access, rapid seeks, stale-result prevention, queue versus
service time, physical file/cache behavior, retained offsets, lazy quality,
RSS, and close. Both small mounted raw/refined fixtures pass; Palette's pending
full-duration artifacts remain necessary for scale and physical-profile
evidence. See `docs/crimson_keypoint_v2_long_duration_benchmark.md`.

The selector-ineligible subject-mask dense-core v1 consumer checkpoint is now
implemented behind a strict explicit-run option. The backend-neutral reader
validates the exact manifest and all 13 direct/consolidated declarations,
retains the authoritative frame offsets after one read, preserves every row
and `instance_key`, and leaves derived metric payloads lazy during playback.
The mounted headless and Metal visual gates passed against Palette's 23,287-
frame fixture, including exact sparse mask presentation and zero stale output.
The NRS source video's `hev1` tag required a stream-copy `hvc1` compatibility
remux for this Mac's AVFoundation decoder; no pixels were re-encoded. This is a
correctness/demo result, not physical-profile promotion or production
selection. See `docs/crimson_subject_mask_v1_consumer.md`.

The deterministic subject-mask v1 long-duration harness is now implemented.
It exercises the strict repository through the shared scheduler and
presentation cache, measures queue wait separately from storage service,
separates physical file transfer from logical dense decode and sparse retained
bytes, and covers random access, 70-frame forward/reverse traversal, rapid-seek
cancellation, stale-publication prevention, RSS, close, retained offsets, lazy
derived metrics, and the absence of `roi_images`. Direction-aware reverse
lookahead is shared and no longer resets the generation for each normal reverse
step. Palette's 23,287-frame integration fixture passed five fresh mounted Mac
processes with zero deadline misses and zero stale visible frames. This accepts
the harness, not long-duration storage-profile performance; see
`docs/crimson_subject_mask_v1_long_duration_benchmark.md`.

Read-only overlay frame request/presentation policy is now backend-neutral.
One tokenized coordinator distinguishes inactive, unavailable, pending,
missing, failed, exact, rejected, and stale results, forwards discontinuities
only after an accepted request, and emits shared diagnostics. A typed subject-
mask adapter owns the shared buffer-to-scene glue, so the Mac render loop no
longer maps repository statuses or tracks its own last mask request. The first
adoption reduces `crimson_macos_main.mm` by ten lines, preserves both strict-v1
and legacy repositories, passes the mounted Metal mask smoke, and builds/tests
with the isolated Linux/NVIDIA target. Keypoints are the next stable typed
adoption; Linux legacy mask loading remains a separate adapter step. See
`docs/crimson_shared_overlay_frame_coordinator_checkpoint_2026-07-31.md`.

The detection-first quality timeline checkpoint is now implemented. The
existing Detect inspector identifies the selected canonical/refined surface
and exposes every observation in the presented frame. Its separate dockable
timeline lazily opens exact canonical confidence or refined source-audit
columns only on request, retains frame offsets once, pages 8,192-frame windows
through the shared scheduler, distinguishes refinement outcomes and reason
codes, and routes plot clicks through the shared seek contract. Plot data is
prepared once per published page. Full-duration mounted canonical-v3 and
refined-v1 gates passed with one and two retained offset reads respectively;
the refined 8,192-frame window issued four concurrent field reads and decoded
64 KiB (the canonical window decoded 32 KiB). Synthetic tests cover empty/
multiple-observation frames and stale-page cancellation. The boundary and
measurements are documented in
`docs/crimson_detection_quality_timeline.md`.

#### Deferred read-only feature queue

The following backend-neutral presentation features are explicitly recorded as
deferred Phase 5 follow-ups. They may proceed while Palette's subject-mask,
subject-shape, eye-geometry, and editing/storage contracts continue to settle,
but none is required to accept those storage surfaces:

- completion of multi-observation ROI-inset routing: detection and keypoint rows
  are exposed independently, while cross-surface ROI selection remains
  deferred;
- overlay filters and styling for confidence, class, labels, skeletons, and
  layer opacity; and
- workspace-local frame bookmarks, navigation history, and next/previous
  flagged-observation commands without writing scientific archive state.

The detection-first quality timeline now visualizes model confidence and
refinement outcomes over the camera-frame domain, shares the maintained
timeline navigation contract, and seeks the video when the user selects a
point. Raw confidence, accepted/rejected decisions, manual rows, and reason
codes remain distinct series. Confidence is the model's reported certainty,
not a ground-truth accuracy or performance measurement. The keypoint-quality
extension is now implemented as a separate lazy, backend-neutral repository,
page buffer, Frame Inspect surface, and dockable timeline. It preserves raw
model confidence separately from refined success/edit/review state, reads and
retains quality frame offsets once on first use, leaves quality payload arrays
untouched during ordinary playback, and aggregates catalog-declared metrics and
flags by complete per-frame row ranges. Details are in
`docs/crimson_keypoint_quality_timeline.md`. Friendly model identity and numeric
refinement cutoffs must be displayed only when a future validated provenance
envelope declares them; they are never inferred from external paths.

The quality-timeline presentation and session lifecycle are now shared by the
macOS and Linux/Windows application paths. The shared boundary owns lazy
repository opening, page/overview buffers, loading and failure presentation,
plot controls, and click-to-seek requests; platform adapters retain decoder and
renderer ownership. Comma/period frame stepping was also moved off the paused
decoder-ring listing and onto the portable exact-frame playback command, so a
missing or undisplayed ring entry can no longer silently disable navigation.
The Mac pause transition now anchors to the presented frame and preserves its
AVFoundation decode buffer, matching Linux buffer-hit behavior instead of
clearing and refilling the buffer on every pause. macOS and the isolated
Linux/NVIDIA build pass; native Windows validation remains pending. The
checkpoint is documented in
`docs/crimson_shared_quality_timeline_checkpoint_2026-07-31.md`.

The macOS composition root has now adopted the shared session itself and
deleted its two inline asynchronous timeline state machines. Optional
repository factories let macOS reuse its already-open analysis archive and
keypoint repository, while Linux/Windows retains the exact-schema path-based
adapter. Durable shared metrics preserve open, physical-read, page-cache, and
overview evidence across timeline close. The adoption removes 196 net lines
from `crimson_macos_main.mm` and is documented in
`docs/crimson_shared_quality_timeline_session_adoption_2026-07-31.md`.

Camera transport presentation is now shared as well. One backend-neutral ImGui
component owns the five transport commands, tooltips, 64-bit frame slider,
time readout, preview-versus-commit action identity, bounded playback intents,
and portable shortcut capture. The Linux/Windows adapter retains its paused
decoder-ring behavior, approximate drag seeks, exact committed seeks, clipped
media routing, and telemetry. The macOS adapter retains clock-only drag
preview, AVFoundation resident-buffer selection and seek fallback, and the
buffer-preserving pause policy. Legacy Linux decoder fields are narrowed only
at that adapter boundary; the shared contract does not impose a 32-bit frame
limit. Details are in
`docs/crimson_shared_camera_transport_checkpoint_2026-07-31.md`.

Seek execution policy is now shared independently of that presentation. The
portable transaction contract owns preview, commit, and discrete phases;
approximate-allowed versus exact accuracy; generation supersession; stale and
cancel handling; execution-path identity; and a common telemetry vocabulary.
macOS retains logical-only drag preview, resident AVFoundation-buffer selection,
and decoder fallback. Linux/NVIDIA retains approximate decoder preview, paused
ring selection, exact decoder submission, clipped-media routing, and stimulus
synchronization. Backend-native resources do not cross the contract. Details
are in
`docs/crimson_shared_seek_transaction_checkpoint_2026-07-31.md`.

Phase 5 also remains open for production-tail acceptance, movement-trail
coordinate policy, any required chaser-data densification, and edit/review
workflows whose shared storage contracts are still changing. Those blockers
must be reviewed explicitly before beginning Phase 6; they must not be silently
reclassified as complete.

### Phase 6 — Inference Parity

Deliverables:

- inventory every TensorRT engine, preprocessing path, output decoder, NMS,
  keypoint/box/mask convention, and downstream consumer;
- preserve original `.pt` and `.onnx` sources and conversion provenance;
- benchmark candidate Apple backends on actual models;
- implement one inference interface with NVIDIA and Apple implementations;
- compare preprocessing tensors, raw outputs when possible, postprocessing,
  detections, keypoints, masks, and scientific downstream outputs;
- define and enforce numerical tolerances.

Gate:

- all current inference workflows run locally on Apple Silicon;
- accepted fixtures pass declared tolerances;
- no TensorRT `.engine` is treated as a portable model source;
- packaging includes models, metadata, and required Metal/Core ML resources.

### Phase 7 — Full Workflow, Packaging, and Release

Deliverables:

- native arm64 `.app` bundle;
- executable-relative resources and correct framework/library embedding;
- signing, hardened runtime, and notarization plan;
- clean-machine installation and launch test;
- full parity matrix across Linux, Windows, and macOS;
- performance, memory, thermal, seek, playback, edit, write, inference, and
  regression results;
- documented supported macOS/hardware baseline.

Gate:

- a user can install and open a real session without a developer shell;
- all parity inventory items are passed or have an explicitly approved,
  documented exception;
- existing NVIDIA builds/tests remain green;
- no required work remains before marking the goal complete.

## Validation Rules

1. Validate proportionally after each change; do not defer all testing to the
   end of a phase.
2. Run syntax/configure/build checks before runtime tests.
3. Use real representative main/crop/stimulus/Zarr data for playback gates.
4. Record exact commands and result locations.
5. Add automated unit/integration coverage for backend-neutral behavior.
6. Preserve GUI smoke coverage on NVIDIA and create equivalent Mac smoke paths.
7. A window opening is not playback parity; a frame appearing is not seek
   parity; a model loading is not inference parity.
8. Test failure paths: missing video, incompatible stimulus encoding, missing
   mapping, invalid frame, end-of-stream, clipped boundary, and device/resource
   failure.
9. Do not weaken assertions or tolerances merely to obtain a pass.

## Prohibited Shortcuts

- Do not delete or disable existing features on macOS to claim completion.
- Do not ship a viewer-only build as full parity.
- Do not run inference remotely unless the deployment architecture is
  explicitly changed and approved.
- Do not materialize every decoded frame as CPU RGBA for the maintained Mac
  playback path.
- Do not let independent media-player clocks replace Crimson's logical frame
  authority.
- Do not use periodic hard seeks as steady-state synchronization.
- Do not infer scientific alignment from equal media times; apply the Zarr
  mapping.
- Do not use lossy re-encoded derivatives as canonical source data without an
  explicit scientific-data decision.
- Do not silently normalize nonzero PTS origins; preserve or record offsets.
- Do not mark a phase complete because the remaining work is difficult.

## Working Protocol for the Mac Codex Session

At the start:

1. Read repository `AGENTS.md` and all files listed under Evidence.
2. Inspect `git status`, branch, remote, and recent history.
3. Confirm AppleClang/Xcode SDK, architecture, CMake, Homebrew dependencies,
   and available representative fixtures.
4. Run or record the current experiment build before refactoring.
5. Create a tracked plan with only one in-progress step.

During work:

- communicate concise progress updates;
- make informed, scoped assumptions and record them;
- keep the NVIDIA path compiling conceptually and validate it on the Linux host
  at phase boundaries;
- prefer small interfaces introduced at real coupling points over speculative
  generalized frameworks;
- update architecture/parity documentation as evidence changes;
- stop and request direction before changing scientific semantics, deployment
  architecture, canonical data, model outputs, or supported-feature scope.

At each handoff:

- state the user-visible outcome first;
- list modified files;
- report configure/build/test commands and results;
- identify what could not be tested on that host;
- distinguish confirmed defects from hypotheses;
- state the next smallest safe milestone.

## Immediate First Task

Begin with Phase 0 and Phase 1 only. Do not start porting overlays, Zarr writes,
or inference until the build/backend skeleton is reviewed.

Concrete first task:

1. Audit the top-level CMake CUDA requirements and platform-coupled source/link
   lists.
2. Propose the smallest additive target/preset structure that preserves current
   NVIDIA defaults.
3. Implement optional CUDA language enablement and explicit Apple/NVIDIA source
   selection.
4. Add a native arm64 macOS preset and minimal ImGui Metal application target.
5. Build and launch it on the Mac.
6. Provide Linux validation instructions for the corresponding branch.
7. Stop for architecture review before moving shared Crimson runtime behavior
   behind new interfaces.

## Goal Completion Rule

Do not mark the master `/goal` complete after the minimal Mac window, viewer,
or first successful inference. Complete only when the Phase 7 gate and the full
feature-parity inventory are satisfied and no required implementation,
validation, packaging, or documentation work remains.
