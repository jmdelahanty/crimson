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

#### Current Phase 5 Checkpoint (2026-07-14)

Phase 5 remains active and is not the full UI/overlay parity gate. Phase 5H is
accepted for the native eye-angle analysis timeline. macOS now has a bounded,
TensorStore-backed eye-angle repository, an asynchronous page cache, maintained
representation and smoothed-field fallback semantics, left/right/vergence
controls, a current-frame cursor, and plot-click exact seeking. The portable
time/frame mapping and repository contracts compile on both platform stacks;
the maintained Linux/Windows application retains its existing eager timeline
loader. The complete contract, UI, deterministic-test, Mac production, and
NVIDIA validation record is in
`docs/crimson_macos_phase5h_eye_angle_timeline.md`. Phase 5G's shared read-only
overlay-control record remains in
`docs/crimson_macos_phase5g_overlay_controls.md`.

The native Mac shell is still intentionally much smaller than the maintained
Linux/Windows `redgui` workspace. Remaining Phase 5 work includes motion,
tail-kinematics, and stimulus timelines; edit/review interactions; and other
panels, docking, and multiwindow workflows identified by the parity inventory.
Phase 5 must not be marked complete until those workflows and the
screenshot/image-difference gate are satisfied.

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
