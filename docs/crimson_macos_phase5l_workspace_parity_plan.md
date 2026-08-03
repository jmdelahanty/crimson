# Crimson Phase 5L Maintained Workspace Parity Plan

Date: 2026-07-16

Lifecycle: **archive-ready completed plan**. Its macOS and Linux/NVIDIA
checkpoints are closed; deferred Windows runtime validation now has a separate
guide and validation record.

Phase 5L moves workspace parity ahead of edit/write workflows. The current
native Mac shell proves the Metal playback and read-only data paths, but its
window composition, navigation, visual density, and control placement differ
substantially from the maintained Linux/Windows `redgui` application.

The storage schemas and editing contracts are currently changing. Phase 5L
must therefore reproduce the maintained workspace using only stable playback,
presentation, read-only overlay, and read-only analysis contracts. It must not
freeze a write schema or introduce a second Mac-specific editing model.

## Scope Boundary

Phase 5L includes:

- a source-linked inventory of maintained menus, windows, panels, default
  visibility, placement, sizing, and lifecycle behavior;
- fixed-size Linux/Windows reference captures for representative workflows;
- the maintained application theme, font choices, spacing, control density,
  labels, ordering, and primary camera composition;
- session/video presentation, transport, camera transforms, representation
  selection, read-only overlay controls, analysis timelines, stimulus context,
  crop/stimulus presentation, and stable diagnostics surfaces;
- window open/close behavior, z-order expectations, remembered visibility, and
  any actual detached or multiwindow behavior used by the maintained build;
- backend-neutral workspace state where state must be shared, with ImGui,
  Metal, OpenGL, CUDA, and native window handles kept outside that contract;
  and
- deterministic structural and screenshot-based validation on both platform
  build graphs.

Phase 5L explicitly excludes:

- bounding-box, refined-keypoint, subject-mask, or other annotation writes;
- detection acceptance/rejection or scientific review mutations;
- new Zarr schemas, migration rules, write destinations, or atomicity policy;
- provisional adapters for storage contracts that are still changing; and
- inference parity, signing, notarization, and packaging work assigned to
  later top-level phases.

Existing write-only commands may be represented as disabled maintained
controls when that is necessary to preserve menu or panel structure. They must
not invoke placeholder writes, create output groups, or silently diverge from
the maintained data model.

## Match the Maintained Application

Workspace parity means matching the application that exists, not designing a
new Mac workspace. The first audit must read the maintained `redgui` source and
capture the running Linux/Windows build before choosing a layout abstraction.

The current source exposes independent ImGui windows and does not show an
active ImGui dockspace. Phase 5L must preserve that topology unless reference
captures demonstrate otherwise. It must not add docking merely because older
roadmap text listed docking as a possible gap. Likewise, GLFW remains valid for
window/input integration while Metal remains the Mac rendering backend; Metal
does not require a different application layout.

Existing backend-neutral UI modules should be reused where their dependencies
permit it. Platform-specific composition is acceptable around renderer-owned
textures and native lifecycle events, but labels, defaults, commands, and
workflow state must not be independently reinterpreted on Mac.

## Work Packages

### 5L.0 Reference and Source Inventory

- Run maintained Linux and Windows builds at fixed logical viewport sizes.
- Capture the initial workspace plus playback, crop/stimulus, overlay,
  analysis, representation, file/session, and stable diagnostics states.
- Record every visible command and window with its source file, default state,
  owner, input state, output action, and write/read-only classification.
- Record fonts, style values, minimum/default window sizes, layout constraints,
  and persisted visibility/position behavior.
- Mark write-dependent controls as deferred rather than inferring their future
  contract.

Status on 2026-07-16:

- complete: source-linked topology, command, default, ownership, persistence,
  and read/write classification in
  `docs/crimson_macos_phase5l_workspace_inventory.md`;
- complete: maintained Linux empty, menu, loaded/default, 1280x800 clipping,
  arranged-role, exact workspace, overlay, crop, stimulus-debug, and alternate
  eye-analysis captures with executable and PNG hashes in
  `docs/reference/phase5l/manifest.json`;
- complete: reusable read-only Linux capture harness at
  `scripts/capture_redgui_workspace_reference.sh`;
- source-complete, runtime-pending: equivalent DPI-aware Windows harness at
  `scripts/capture_redgui_workspace_reference.ps1`; it is not evidence until
  run and inspected on a real Windows host;
- complete: app-side exact-frame/panel/front-buffer ready contract with atomic
  JSON evidence and 60-frame stability;
- data-gated: `analysis-tail-stimulus`; the representative archive has no tail
  kinematics and the hook rejects it cleanly rather than fabricating a state.
  A 2026-07-16 metadata-only scan of 114 mounted production analysis archives
  found 103 stimulus-only, one tail-only, ten with neither, and zero with both;
  the tail-only archive passes the TensorStore tail probe and maintained
  `redgui` loads its tail run before cleanly rejecting the missing stimulus
  half with exit code 2;
- unavailable: real Windows runtime capture; the required procedure and
  evidence boundary are recorded in
  `docs/reference/phase5l/windows/README.md`.

The Linux/source portion of 5L.0 is closed except for the production-tail data
fixture. The cross-platform 5L.0 evidence gate remains open until a real Windows
capture is added; Linux pixels must not be relabeled as Windows evidence.

### 5L.1 Portable Workspace State

- Define only the backend-neutral state required to keep stable commands,
  visibility, selection, and playback behavior consistent.
- Reuse existing playback clocks, overlay controls, representation selection,
  and timeline controls instead of duplicating them.
- Add fixture tests for default window visibility, command enablement,
  selection propagation, and state restoration.
- Keep repository ownership and rendering resources in their existing
  platform/backend layers.

Status on 2026-07-16: complete. `crimson_workspace_contracts` now defines
capability-derived maintained window submission, stable command enablement,
shared source/representation/event selections, versioned snapshot restoration,
and playback intents. It reuses the existing crop and read-only overlay state
and leaves both playback clocks/controllers and all repository/rendering
resources in their platform owners. Mac passed 34/34 headless tests, the
native Mac Metal playback smoke passed frames 0 through 300, the isolated
NVIDIA build passed 24/24 tests, and the authenticated NVIDIA playback smoke
passed frames 0 through 300. The contract and evidence are documented in
`docs/crimson_macos_phase5l_portable_workspace_state.md`. Windows runtime
validation remains deferred; no Windows result is inferred from Mac or Linux.

### 5L.2 Mac Workspace Composition

- Reproduce the maintained primary camera layout and transport placement.
- Apply the maintained theme, typography, spacing, sizing, and control order.
- Present the already-supported crop, stimulus, overlay, and analysis surfaces
  in their maintained workspace roles rather than Mac-only arrangements.
- Preserve resizing, focus, close/reopen, and presented-frame behavior.
- Avoid new storage dependencies and avoid synchronous full-resolution GPU
  readbacks.

Status on 2026-07-16: complete for the Mac composition checkpoint. The native
Mac shell now uses the maintained independent-window topology, Classic theme,
Roboto/Fork Awesome typography, scaled first-use geometry, camera transport,
paused buffer inspection, crop/stimulus roles, read-only overlays, and separate
stimulus/analysis timelines. Metal presentation follows live ImGui content
rectangles without a full-resolution readback, normal sessions persist under
Application Support, and pre-parity camera geometry is isolated by a versioned
window identity. Mac passed 35/35 headless tests and both acquisition and live
geometry multistream smokes. The cumulative isolated NVIDIA build passed 24/24
tests and its authenticated 0:300 playback smoke. Details are in
`docs/crimson_macos_phase5l2_workspace_composition.md`. Windows validation is
deferred and is not inferred from those results; 5L.3 followed.

### 5L.3 Stable Window and Workflow Coverage

- Wire every stable read-only maintained window identified by the inventory.
- Exercise play/pause, seeking, stepping, zoom/pan/rotation, source selection,
  overlay toggles, timeline navigation, and window lifecycle behavior.
- Verify that camera, crop, stimulus, overlays, and timeline cursors continue to
  use the same presented/requested frame rules established by earlier phases.
- Leave deferred mutation controls disabled and assert that no write repository
  is opened.

Status on 2026-07-17: complete on macOS and the isolated Linux/NVIDIA build.
The Mac shell now wires the maintained stable file/session, transport, camera,
buffer inspection, Frame Inspect, crop/stimulus, timeline, diagnostics, help,
error, and window-lifecycle workflows. Missing read-only scene adapters and all
mutation controls remain visibly disabled rather than acquiring provisional
storage semantics. The portable workflow fixture covers command enablement,
playback, seeking/stepping, camera state, buffered identities, selections,
overlays, lifecycle, restoration, and the read-only invariant. Mac passed
36/36 headless tests plus acquisition and live-geometry multistream smokes with
zero presentation skew. The cumulative isolated NVIDIA build passed 25/25
tests and its authenticated 0:300 playback smoke. Details and explicit adapter
boundaries are in `docs/crimson_macos_phase5l3_stable_workflows.md`. Windows
validation remains deferred; 5L.4 visual and cross-platform acceptance is
next.

### 5L.4 Visual and Cross-Platform Acceptance

- Capture equivalent Mac and maintained reference states at the same logical
  content dimensions and data/frame selections.
- Compare window presence, hierarchy, labels, default visibility, control
  order, anchors, and content bounds structurally.
- Apply the existing Phase 5 geometry/raster tolerances to camera and overlay
  content. Compare UI text by exact content and anchor rather than
  cross-platform font raster pixels.
- Use masked or region-based screenshot comparisons for unavoidable operating
  system chrome and font-raster differences; document every mask and tolerance.
- Run the portable workspace tests on Mac and the isolated NVIDIA build, then
  run interactive production smokes on both platforms.

Status on 2026-07-17: complete on macOS and the isolated Linux/NVIDIA build.
The shared semantic recorder and exact-state capture hooks cover five
equivalent 1920x1080 states, including live crop, alternate `gaze` analysis,
and stimulus frame 1024, plus the Mac initial empty state. The cross-platform
comparator passes all 46 structural and clean-camera raster checks, with zero
window/control anchor delta and a maximum clean-camera block channel delta of
2.109375/255 against a 3/255 limit. Controlled portable and Metal fixtures
enforce the 0.25-source-pixel, 0.995 mask-IoU, 0.995 vector coverage, two-pixel
outlier, and 3/255 thresholds where the production subject is too small and
label-obscured for a defensible direct overlay pixel test. Mac passed 36/36
tests, both 1024:7024 production multistream modes, and the final six-state
capture refresh. The isolated NVIDIA build passed 26/26 tests and its
authenticated 0:300 production smoke. Windows runtime validation remains an
explicit user-approved deferral. The contract, masks, and evidence are
documented in `docs/crimson_macos_phase5l4_visual_acceptance.md`.

## Acceptance Gate

Phase 5L is accepted only when:

- the maintained workspace inventory has no unclassified stable read-only
  window, menu, panel, or command;
- the Mac initial workspace is recognizably and structurally the same as the
  maintained reference at the agreed logical sizes;
- default visibility, labels, control ordering, primary view composition, and
  window lifecycle behavior match the maintained application;
- all stable playback and read-only workflows pass deterministic behavior tests
  and representative production smokes;
- structural and region-based screenshot comparisons pass documented
  tolerances;
- Mac and NVIDIA builds/tests pass from the same cumulative source; and
- the checkpoint performs no annotation, review, or Zarr mutation and commits
  to no provisional write schema.

Phase 5 remains active after 5L. Edit/review implementation resumes only after
the shared storage and mutation contracts stabilize. Production-tail
acceptance and stimulus-overlay gaps may proceed independently when suitable
data and maintained reference behavior are available. The next implementation
checkpoint is the incremental read-only loader decomposition and chaser-polar
pilot in `docs/crimson_macos_phase5m_read_only_zarr_boundary_plan.md`; remaining
stimulus camera overlays move to Phase 5N.
