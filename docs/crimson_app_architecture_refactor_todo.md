# Crimson App Architecture Refactor TODO

Date anchored: 2026-04-03.

Lifecycle: **active architecture roadmap**.

## Recording Clip Index Media

- [x] Extract strict backend-neutral `recording_clip_index.json` parsing and
      parent-to-local frame mapping.
- [x] Preserve the clip-index source through the shared session-open and
      replacement transaction.
- [x] Add macOS AVFoundation clip switching on a global recording frame axis.
- [x] Add recording-identity-based archive discovery and mounted boundary
      smoke coverage.
- [ ] Adopt the shared mapping contract in the Linux/Windows FFmpeg/NVIDIA
      decoder adapter.
- [ ] Replace the compatibility envelope when Palette publishes a versioned,
      digest-bound recording clip index contract.

## 2026-07-23 Runtime Contract Checkpoint

The first backend-neutral runtime slices are now shared by the macOS and
NVIDIA application shells: session lifecycle, loading progress, frame
presentation policy/metrics, and diagnostic reporting. Their ownership rules
and cross-platform test surface are recorded in
`docs/crimson_shared_runtime_contracts_2026-07-23.md`.

Session readiness policy and generation-safe session-open transactions have
also moved into that shared runtime library. The macOS shell now uses strict
analysis readiness without background dismissal or autoplay, while both shells
use the transaction for session-open lifecycle/progress bookkeeping.

The macOS shell also uses the portable progress-settlement helper for its
initial session. The helper converts analysis-loader readiness into the parent
`analysis` product and commits or fails the session transaction; repository
result adoption, ImGui rendering, playback, and Metal ownership remain in the
shell.

Loading presentation is now split as well. A portable view model lives in the
runtime contracts, and the shared ImGui modal lives under `src/gui`. The macOS
composition root supplies snapshots and readiness decisions but no longer owns
the modal's labels, progress formatting, or popup lifecycle.

The next bounded controller slice is also complete. A backend-neutral
`RecordingOpenWorkflowController` now owns the active transaction, product
timing, failure/cancellation transitions, and generation-checked child-loader
settlement. The macOS and NVIDIA shells both use it for CLI and interactive
recording opens, while they continue to own archive reads, decoder creation,
repository adoption, threads, and GPU resources. This is progress toward the
Phase 3 `RecordingLoader` boundary, not completion of the broader Phase 3
controller extraction.

Playback transport is now a shared controller as well. The former portable
logical clock owns play/pause/seek/step/rate commands, readiness gating,
timeline clamping, and end-of-stream pause. macOS uses it directly for Metal
viewer controls; NVIDIA uses the same clock and command state while retaining
its existing buffer-aware resume, exact-seek, stimulus, FFmpeg, CUDA, and
OpenGL execution. This is a completed bounded slice of the Phase 3
`PlaybackController`, not completion of the platform playback adapters.

This checkpoint does not create the proposed catch-all `AppState`. Decoder,
repository, thread, window, and GPU-resource ownership remains in the existing
platform/application layers. Coordinate-sensitive ROI work remains deferred
until the acquisition-to-presentation contracts stabilize.

## 2026-07-31 Shared Affiliated-Media Checkpoint

Recording-root inference, persisted-path relocation, and strict affiliated-
video discovery are now backend-neutral. macOS and Linux standard archives use
the same `ArchiveContext` repository, including fail-closed authoritative
metadata handling; the Linux legacy source hint is used only when shared
metadata is absent. Clipped collection switching remains a compatibility
adapter. See `docs/crimson_shared_affiliated_media_checkpoint_2026-07-31.md`.

## 2026-08-03 Composable Frame Inspect Checkpoint

The macOS and Linux/Windows shells now compose Frame Inspect through one
backend-neutral ImGui window module. The shared module owns window and tab-bar
lifecycle, visible-module ordering, disabled interaction state, stable
programmatic tab selection, and header/footer placement. Both platform
composition roots register Detect, Keypoints, Subject Masks, and Eye Angles as
callbacks. Linux/Windows additionally registers its maintained Tail Kinematics
inspection tab; macOS exposes tail kinematics through Analysis Timeline.

This is intentionally a presentation boundary rather than a merge of platform
repositories or editing policies. macOS callbacks retain the strict read-only
repository surfaces, while Linux/Windows callbacks retain their legacy review
and editing panels. Both use the same `FrameInspectView` selection state, so
workspace restore and programmatic navigation no longer require a Linux-only
tab enum or conversion shim. Semantic ImGui coverage verifies that requested
tabs remain stable and hidden modules are not rendered.

The first feature module now uses that composition boundary. Detect has one
backend-neutral presentation model and ImGui renderer for surface/run identity,
frame readiness, complete observation rows, stable instance selection,
confidence, class, source provenance, and quality-timeline status. The strict
canonical/refined repository adapter and the Linux legacy-loader adapter are
separate. Linux review, diagnostics, and bounding-box editing remain extension
panels below the shared presentation rather than dependencies of it.

Keypoints now follows the same module boundary. One shared presentation model
and ImGui renderer owns surface/run identity, frame readiness, complete
observation rows, stable instance selection, pose and per-landmark confidence,
validity/edit state, and quality-timeline status. The strict keypoint-v2
repository adapter and Linux legacy-loader adapter remain independent. Overlay
visibility and styling, skeleton and heading diagnostics, review, and crop
editing remain platform extensions, so extracting the read-only surface does
not weaken either platform's maintained workflows.

Subject Masks now uses the same boundary. The shared presentation model and
renderer preserve complete per-frame observation ranges, ROI dimensions,
component/channel availability, pixel-payload and contour counts, source-crop
lineage, and stable `instance_key` selection. A resolved empty frame is
presented as zero observations rather than as an in-progress read. The strict
subject-mask repository adapter and the Linux legacy-loader adapter remain
separate; overlay modes and component visibility, subject-shape controls,
mask editing, and review metadata remain platform extensions. This keeps the
dense-mask and sampled-contour repositories out of the UI module while
preserving the Linux editing workflow.

Eye Angles now follows the same boundary. The shared presentation model and
renderer preserve complete per-frame observation ranges, row-scoped UI
selection, representation-specific scalar and vector fields, validity,
marginal/reason state, and source/crop lineage. The UI selection key is
explicitly not scientific observation identity. A strict eye-geometry
repository adapter and a Linux legacy-loader adapter remain separate; overlay
styling, QC filtering and navigation, and timeline controls remain platform
extensions.

Subject Shape now composes inside the Subject Masks view through the same
boundary. The shared model and renderer preserve every resolved observation,
row-scoped UI selection, source/refined/crop lineage when declared, ROI size,
per-feature validity, curve/sample counts, and failure reasons. It deliberately
does not infer one overall validity value: the legacy loader's row-valid flag
and the strict repository's feature-valid flags do not mean the same thing.
The strict subject-shape repository adapter and Linux legacy-loader adapter
remain separate, while Metal/OpenGL overlay controls, Linux QC navigation, and
mask editing remain platform extensions. This extraction does not freeze the
still-evolving Palette subject-shape storage manifest.

Subject Shape overlay controls now use a shared widget module as well. Its
backend-neutral state covers the common geometry visibility controls, while
capabilities explicitly gate the legacy body, swim-bladder, and eye contour
toggles that are not part of the strict macOS overlay surface. Thin adapters
translate the existing read-only Metal state and Linux camera-view options;
rendering, storage access, Linux QC, and editing ownership remain unchanged.

Eye Geometry overlay controls now follow the same pattern. The shared widget
owns visual-cone, gaze-ray, angle-arc, and label visibility. A capability keeps
the explicit macOS master toggle and its Review/Debug availability gating,
while Linux/Windows retains its direct detail toggles. The strict Metal adapter
only translates control state; mask modes, repositories, scene construction,
and platform renderers remain independently owned.

Subject Mask mode and component controls are now shared as a separate narrow
module. It reuses the existing backend-neutral Realtime/Review/Debug mode and
owns visibility controls for the body, swim bladder, and both eyes. Capability
inputs preserve macOS availability gating and Linux/Windows refined-mask
behavior, including their distinct Realtime tooltip semantics. Platform master
toggles, legacy eye-mask fallback, metadata, contour diagnostics, editing/QC,
repositories, scene construction, and renderers remain independently owned.

Frame Inspect workflow projection now has a backend-neutral controller as
well. It owns stable tab synchronization, workspace overlay/window projection,
portable keypoint-selection state, full-frame edit eligibility, and a
deterministic command sequence. The NVIDIA adapter translates the legacy Frame
Debug result and executes dataset switching, review/QC navigation, row seeks,
and in-memory bounding-box reset/selection commands. Archive writes, payload
construction, decoder diagnostics, repository reloads, and rendering remain
explicitly in the NVIDIA composition root pending their own transaction
boundaries. The portable controller has headless state and command-order tests;
the adapter is compiled with the isolated Linux application build. This is a
bounded Phase 3 extraction and does not change storage, editing, or coordinate
contracts.

## 2026-08-05 Playback Presentation Lifecycle Checkpoint

`PlaybackSessionController` now owns the playback-frame target and commit
lifecycle that was previously embedded in `red.cpp`. The portable policy uses
explicit target and commit input/output values, so paused and seek-settling
states, decode bounds, buffered fallback, non-regressing commits, and deferred
release decisions are headless-testable. The NVIDIA controller remains the
only layer that reads or releases concrete frame slots; render/GL/CUDA work and
trace serialization remain in `red.cpp`.

This is a bounded Phase 3 extraction. It deliberately does not change the
logical transport clock, clipped-media routing, stimulus handling, archive
state, or renderer ownership. macOS can consume the same portable policy when
its presenter reaches the corresponding explicit commit lifecycle.

## 2026-08-05 Playback Diagnostics And Clipped Handoff Checkpoint

Playback JSONL envelope writing and the common playback, seek, presenter,
buffer, stimulus, frame-sync, and clipped-state serializers now live in a
portable diagnostics module. The module has profile-specific serializers so
existing playback, frame-sync, and clipped event field sets do not silently
grow or drift. `red.cpp` still gathers concrete NVIDIA slot state, decoder
progress, resolver results, and GPU texture evidence; the dense
texture-draw and renderer-specific clipped-frame payload remained there at
this checkpoint.

Clipped-media boundary policy is likewise portable. It accepts already
resolved parent-frame bindings and emits only a load-and-seek command plus
request/load/settlement/failure outcomes. The NVIDIA composition root retains
Zarr resolver calls, media loading through `PlaybackSessionController`, decoder
seek execution, GPU resources, and trace-sink ownership. This keeps the
policy reusable without making the controller a media, renderer, or archive
adapter.

Headless tests cover JSONL envelope/flush behavior, field-profile stability,
buffer summaries, and boundary request/load/settlement/failure behavior. This
is a bounded Phase 3 extraction; it does not change clip-index storage,
decoder scheduling, rendering, or archive selection.

## 2026-08-05 NVIDIA Playback Diagnostics Adapter Checkpoint

The remaining NVIDIA playback trace rules are now separated from the
application loop. `nvidia_playback_trace_model` owns frame-sync change gating,
clipped-frame comparison and mismatch aggregation, source-label policy, and the
stable JSON payloads for resolver, bounding-box, texture-draw, and clipped-frame
events. It consumes plain snapshots and has no Zarr, CUDA, OpenGL, ImGui, or
decoder dependency. Headless tests cover change suppression, unknown-value
nullability, delta summaries, callback failures, and the established event
schema.

`red.cpp` remains the NVIDIA composition layer: it resolves concrete Palette
rows, inspects decoder and ring-buffer state, captures renderer texture IDs,
and adapts those values into trace snapshots. OpenGL texture and framebuffer
readback are isolated in `nvidia_gl_diagnostics`; that module is intentionally
backend-specific because its correctness depends on a current GL context and
restoring GL bindings and pixel-pack state after readback.

A follow-up adapter now owns the concrete bounding-box and texture conversions,
camera ring-buffer snapshot gathering, and the detailed clipped-playback buffer
summary. Resolver rows are converted to plain values at the composition edge;
the adapter does not acquire a new direct dependency on `zarr_loader.h`.
Standalone clipped texture-draw and texture-dump event schemas also moved into
the pure trace model. The render loop retains the timing-sensitive pre/post-draw
capture, GL readback call, media/resolver lookup, and log-writer invocation.

These extractions change neither playback scheduling nor the JSONL diagnostic
schema. The first checkpoint reduced `red.cpp` from 7,544 to 7,139 lines; the
follow-up reduces it to 6,855 lines, a cumulative 689-line reduction while
preserving the existing NVIDIA runtime evidence path.

## 2026-08-05 Shared Playback Presentation Adapter Checkpoint

Buffered playback target selection and presentation commit policy now use one
backend-neutral adapter contract. The contract consumes portable frame
candidates with optional platform slot identities, uses 64-bit frame numbers,
and emits exact, latest-at-or-before, or hold decisions. Commit policy treats a
committable presentation independently from slot identity, so a retained Apple
decode surface can advance presentation without pretending it belongs to an
NVIDIA ring slot.

The NVIDIA session controller supplies concrete ring-slot identities and keeps
snapshot reads, leases, playback-state mutation, and compare-frame history
release. The macOS adapter supplies slotless decoded frames and keeps
AVFoundation deque eviction and retained pixel-buffer ownership. Metal,
OpenGL, CUDA, FFmpeg, decoder scheduling, composite stimulus/crop alignment,
and renderer ownership remain in their platform layers.

Headless tests cover 64-bit targets, preferred exact slots, buffered fallback,
paused/discontinuous gating, slotless and slotted commits, non-regression, and
explicit-release policy. This completes the roadmap note that macOS should
adopt the same portable target/commit lifecycle without imposing NVIDIA buffer
semantics on the Apple backend.

The policy contract is 64-bit, but both current platform transport boundaries
remain legacy `int` surfaces: Apple decoded-frame metadata and the NVIDIA
session-controller API narrow frame numbers before entering or leaving the
adapter. End-to-end 64-bit playback remains a separate transport migration.

## Why This Exists

`crimson` has already done useful mechanical splits, but the core architecture
still has three god-object pressure points:

- `src/red.cpp` is still the application shell, playback coordinator, UI event
  loop, review workflow host, editing host, and large parts of runtime state.
- `src/gui.h` still contains real behavior instead of being a narrow interface
  layer.
- `src/zarr_loader.h` plus `src/zarr_loader*.cpp` still expose a single large
  "load everything" API surface even after file splitting.

This doc supersedes the archived
`docs/archive/legacy-plans/crimson_cli_and_modularization_todo.md`. That historical plan
intentionally left "AppState extraction from main()" out of scope; this doc
tracks the larger architectural pass and is the active source of remaining
work.

## Primary Goal

Keep Zarr as the canonical store and preserve the good parts of the current
data model, while inheriting the strongest application-structure ideas from the
`rob_ui_overhaul` branch:

- feature-local UI modules instead of one giant implementation sink
- explicit state buckets for windows and workflows
- small infrastructure helpers for notifications and deferred actions
- a thinner composition root in `main()`

Do not inherit the weak parts of `rob_ui_overhaul`:

- a giant catch-all `AppContext`
- header-only UI files that mix drawing, IO, and business logic
- tests/build targets that depend on recompiling nearly the whole app

## Non-Negotiable Guardrails

- Keep Zarr as the primary system of record.
- Do not regress chunked or lazy read paths that matter for large recordings or
  NFS-backed access.
- Keep the current Palette Zarr contract stable unless a separate tracked task
  explicitly changes it.
- Prefer narrow service/context objects over introducing a new all-knowing
  `AppState`.
- Refactor in small slices that keep the app runnable after each phase.

## Refactor Rules During This Work

- No new feature logic should land in `src/red.cpp` or `src/gui.h` unless it is
  a short-lived mechanical shim required by an active refactor PR.
- New UI workflow code should start in `src/gui/*.cpp` plus a narrow context,
  not in `src/gui.h`.
- New Zarr behavior should land behind a repository/service seam, not by making
  `ZarrDetectionLoader` a bigger public API.
- Any PR that increases coupling between UI code and TensorStore details should
  be considered a regression unless there is a documented exception.

## Good Parts To Keep

From current Crimson:

- Zarr as a single object store with hierarchy and provenance.
- TensorStore-backed reads and writes.
- Chunk-aware eye mask access and local cache/prefetch behavior.
- Exact dependency stack control via `CMakePresets.json`.

From `rob_ui_overhaul`:

- workflow-specific UI files under `src/gui/`
- explicit per-window or per-feature state structs
- narrow infrastructure helpers like deferred actions, popups, and toasts
- a project/session load path that is easier to reason about than raw globals

## Target Shape

The intended end state is:

1. `src/red.cpp` becomes a composition root and frame loop, not the home for
   most business logic.
2. `src/gui.h` stops being an implementation file and either becomes a very
   small facade or disappears entirely.
3. Zarr access is split into domain repositories/services with narrow public
   APIs:
   - detection review reads/writes
   - keypoint reads/writes
   - eye mask reads
   - stimulus alignment
   - movement/chaser data
   - archive discovery / path resolution
4. App state is split into a few intentional buckets:
   - session state
   - playback state
   - feature/window state
   - ephemeral command/edit state
5. UI features call services through narrow contexts instead of directly
   grabbing everything from globals or a giant loader object.

## Suggested File/Module Layout

This is a target, not a mandatory one-shot rename:

```text
src/
  app/
    session_state.h
    playback_state.h
    feature_state.h
    deferred_actions.h
    notifications.h
  gui/
    main_menu_bar.cpp
    recording_loader_panel.cpp
    review_panel.cpp
    bbox_editor_panel.cpp
    stimulus_panel.cpp
    playback_panel.cpp
  zarr/
    archive_context.h
    archive_discovery.cpp
    detection_repository.cpp
    keypoint_repository.cpp
    eye_mask_repository.cpp
    stimulus_repository.cpp
    movement_repository.cpp
    review_write_repository.cpp
```

## Immediate PR Sequence

These are the first three safe PRs. They are intentionally smaller than the
full phase list below.

### PR1: State Buckets + Fixture Baseline

Goal: create intentional state boundaries and land regression fixtures before
touching loader architecture.

- [ ] Add:
  - `src/app/session_state.h`
  - `src/app/playback_state.h`
  - `src/app/feature_state.h`
  - `src/app/notifications.*`
  - `src/app/deferred_actions.*`
- [ ] Add miniature fixture archives for:
  - raw detect frame lookup
  - refined detect dataset switching
  - eye mask chunk reads
  - stimulus alignment metadata
- [ ] Add baseline tests that encode current expected behavior for those
      fixtures before further refactors.
- [ ] Wire the new state structs into existing code mechanically, with no
      workflow redesign yet.

Acceptance:

- App behavior is unchanged.
- Fixture tests pass.
- New state structs exist and are used in at least one path.
- No new feature code was added to `src/red.cpp` or `src/gui.h`.

#### PR1 Detailed Implementation Checklist

This is the recommended file-by-file slice for PR1.

1. Add `src/app/session_state.h`.
   Initial struct should absorb "loaded session / archive / media identity"
   concerns from `src/red.cpp`, not playback counters or widget toggles.
   First-pass fields:
   - `UiPathConfig ui_path_config`
   - `std::string start_folder_name`
   - `std::string recording_root`
   - `std::string skeleton_root`
   - `std::string archive_path`
   - `std::string affiliated_video_path`
   - `std::string stimulus_video_path`
   - `bool zarr_loaded`
   - `bool input_is_imgs`
   - `std::vector<std::string> camera_names`
   - `std::vector<CameraParams> camera_params`
   - detection-dataset selection metadata now tied to the loaded archive
     rather than to ad hoc globals

2. Add `src/app/playback_state.h`.
   Do not replace existing playback structs yet. Wrap and group them.
   First-pass struct should aggregate:
   - existing `PlaybackState`
   - existing `SeekProgress`
   - existing `StimulusPlayback`
   - `double video_fps`
   - `double inst_speed`
   - `float set_playback_speed`
   - `int current_frame_num`
   - `int label_buffer_size`
   - `int stimulus_buffer_size`
   - `bool stimulus_use_cpu_buffer`
   - `bool stimulus_use_software_decode`
   - frame-sync debug fields now living near playback, not near unrelated UI

3. Add `src/app/feature_state.h`.
   This should absorb UI/workflow state that is currently spread across
   `src/red.cpp`, `src/review_frame_state.h`, and `src/zarr_bbox_edit.h`.
   First-pass grouping:
   - help / modal / error flags:
     - `bool show_help_window`
     - `bool show_error`
     - `std::string error_message`
   - review flow:
     - `ReviewFrameFilters review_filters`
     - `ReviewFrameCache review_cache`
     - `std::string review_frame_status`
   - bbox review/edit flow:
     - `ZarrBBoxEditState bbox_edit`
     - `std::string bbox_payload_status`
     - `std::optional<ManualDetectPayloadPreview>` or equivalent preview struct
     - manual write setting fields currently in static locals
   - debug/status strings that are not playback-owned:
     - `std::string decode_debug_status`

4. Add `src/app/notifications.h` and `src/app/notifications.cpp`.
   Keep this intentionally small in PR1.
   First-pass types:
   - `enum class NotificationLevel { Info, Warning, Error }`
   - `struct NotificationMessage`
   - `class NotificationQueue`
   This is not a UI system rewrite. It just stops random status strings from
   being threaded through unrelated code.

5. Add `src/app/deferred_actions.h` and `src/app/deferred_actions.cpp`.
   Keep this tiny and generic.
   First-pass API:
   - `void push(std::function<void()>)`
   - `void run_all()`
   - `bool empty() const`
   Use it only for actions already being effectively deferred via ad hoc flags.

6. Update `src/red.cpp` mechanically.
   First PR should only replace local-group sprawl with explicit grouped state.
   Concrete locals to move first:
   - `UiPathConfig ui_path_config`
   - `start_folder_name`
   - `show_help_window`
   - `show_error`
   - `error_message`
   - `video_fps`
   - `inst_speed`
   - `set_playback_speed`
   - `PlaybackState ps`
   - `SeekProgress seek_progress`
   - the `stimulus_player` singleton
   - `ReviewFrameFilters review_frame_filters`
   - `ReviewFrameCache review_frame_cache`
   - `review_frame_status`
   - `decode_debug_status`
   - `bbox_payload_status`
   - manual-write preview/settings locals
   - `g_zarr_bbox_edit_state`
   Keep behavior identical. Avoid moving controller logic in PR1.

7. Create `tests/` and `tests/fixtures/` if they do not already exist.
   Recommended first committed fixtures:
   - `tests/fixtures/palette_small_review.zarr/`
   - `tests/fixtures/palette_small_eye_masks.zarr/`
   - `tests/fixtures/palette_small_stimulus.zarr/`
   - `tests/fixtures/palette_long_sparse.zarr/`
   The long sparse fixture should have many frames and low per-frame detection
   counts to guard offset-based lookup behavior for long recordings.

8. Add `tests/test_support.h`.
   Keep it minimal:
   - `CRIMSON_TEST_CHECK(cond)`
   - `CRIMSON_TEST_EQUAL(a, b)`
   - helper for resolving `tests/fixtures/...` relative to the test file

9. Add `tests/test_app_state_defaults.cpp`.
   Verify:
   - new state structs default-construct cleanly
   - `FeatureState` does not own playback counters
   - `SessionState` does not own transient drag state
   - `PlaybackRuntimeState` owns seek/playback/stimulus state and frame-sync
     counters

10. Add `tests/test_zarr_fixture_detection_lookup.cpp`.
    Verify against `palette_small_review.zarr` and `palette_long_sparse.zarr`:
    - archive opens successfully
    - `getDetectionsForFrame(frame)` matches expected counts
    - `getBoundingBoxesForFrame(frame)` returns expected rows
    - sparse far-apart frame lookups behave correctly
    - refined dataset switching still preserves correct frame-local lookup

11. Add `tests/test_zarr_fixture_eye_mask_chunks.cpp`.
    Verify against `palette_small_eye_masks.zarr`:
    - archive opens successfully
    - eye masks can be requested for selected ROI/frame rows
    - repeated access to nearby rows succeeds
    - adjacent chunk prefetch path is exercised indirectly by neighboring ROI
      requests

12. Add `tests/test_zarr_fixture_stimulus_alignment.cpp`.
    Verify against `palette_small_stimulus.zarr`:
    - archive opens successfully
    - stimulus alignment is detected
    - representative camera-frame to stimulus-frame lookups are stable
    - missing or out-of-range frame queries fail cleanly

13. Update `CMakeLists.txt`.
    For PR1, use simple test executables plus `add_test(...)`.
    Recommended first targets:
    - `test_app_state_defaults`
    - `test_zarr_fixture_detection_lookup`
    - `test_zarr_fixture_eye_mask_chunks`
    - `test_zarr_fixture_stimulus_alignment`
    Link them the same way current small Zarr tools are linked:
    - `tensorstore::all_drivers`
    - `Threads::Threads`
    - `nlohmann_json::nlohmann_json`
    and the minimum Crimson source files needed for the loader path.

14. PR1 done means:
    - the app still launches and behaves the same
    - there is now a `tests/` home for fixture-based regression checks
    - state is grouped intentionally enough that PR2 can add repository seams
      without digging through hundreds of unrelated locals first

### PR2: Repository Facades Without Implementation Migration

Goal: define the future public seams while keeping the current loader as the
backing implementation.

- [ ] Add:
  - `src/zarr/archive_context.*`
  - `src/zarr/detection_repository.*`
  - `src/zarr/eye_mask_repository.*`
  - `src/zarr/stimulus_repository.*`
  - `src/zarr/review_write_repository.*`
- [ ] Implement these as thin adapters over the existing loader first.
- [ ] Move no heavy logic yet. This PR is about public shape, not code motion.
- [ ] Update tests to target repository interfaces where possible.

Acceptance:

- UI-facing code can consume repository interfaces without importing
  TensorStore-heavy details.
- Existing chunk-sensitive behavior still passes fixture tests.
- `ZarrDetectionLoader` is no longer the only public seam future UI work can
  depend on.

### PR3: Review/BBox Workflow Extraction

Goal: prove the new seams by extracting one real workflow from `red.cpp` and
`gui.h`.

- [ ] Add:
  - `src/gui/review_panel.*`
  - `src/gui/bbox_editor_panel.*`
  - `ReviewController`
  - `BBoxEditController`
- [ ] Make the review and bbox-edit path consume:
  - state buckets from PR1
  - repository facades from PR2
- [ ] Remove review/bbox behavior from `src/gui.h` where possible.
- [ ] Reduce direct review/bbox mutation code in `src/red.cpp`.

Acceptance:

- The review/bbox workflow still works end to end.
- Review/bbox panel code no longer reaches directly into `ZarrDetectionLoader`
  internals.
- `src/gui.h` gets smaller and loses behavior, not just declarations moved
  around.

## Phase 0: Freeze The Seams

- [ ] Write down the current public contracts that must remain stable during the
      refactor:
  - `docs/crimson_detect_bbox_read_contract.md`
  - `docs/crimson_keypoint_read_contract.md`
  - `docs/crimson_refined_detect_manual_contract.md`
  - `docs/crimson_keypoint_manual_write_contract.md`
  - `zarr_structure.md`
- [ ] Add a short architecture note to each refactor PR stating:
  - which public API moved
  - which contract stayed the same
  - which large object got smaller
- [ ] Keep a running inventory of which parts of `red.cpp` and `gui.h` still
      own real behavior after each phase.

## Phase 0.5: Fixture And Regression Baseline

- [ ] Add miniature Zarr fixtures for the current critical access patterns:
  - raw detect frame lookup
  - refined detect manual/interpolated preference
  - eye mask chunk loading
  - stimulus alignment lookup
- [ ] Add a synthetic "long recording" fixture with many frames and sparse
      detections to guard frame-offset access behavior.
- [ ] Add tests that lock in current behavior before any repository or loader
      surgery:
  - frame-local detection lookup via offsets
  - eye mask chunk cache + adjacent prefetch
  - dataset switching behavior
  - manual review write payload generation where applicable
- [ ] Make these tests required before Phase 4 implementation migration starts.

## Phase 1: Split Runtime State Without Creating A New God Object

- [ ] Create `src/app/session_state.h`.
  - Hold current archive path, affiliated video path, calibration info,
    selected detection dataset, and loaded-run metadata.
  - Do not put playback counters, temporary drag state, or per-panel toggles
    here.
- [ ] Create `src/app/playback_state.h`.
  - Hold frame number, seek state, play/pause, decoder coordination state, and
    stimulus playback coordination.
- [ ] Create `src/app/feature_state.h`.
  - Hold per-feature UI state now scattered across `red.cpp`, `gui.h`, and
    globals such as review filters, bbox edit mode, selected box, and active
    tool toggles.
- [ ] Move "notification" concerns into `src/app/notifications.*`.
  - Toasts, warnings, and transient status should no longer be ad hoc strings
    threaded through unrelated code.
- [ ] Move deferred one-frame-later actions into `src/app/deferred_actions.*`.
  - This is a direct inheritance from the useful `rob_ui_overhaul` pattern,
    but keep it tiny and explicit.

## Phase 2: Kill `gui.h` As An Implementation Sink

- [ ] Inventory every non-trivial function in `src/gui.h`.
  - Classify each as draw-only, state mutation, IO, parsing, threading, or
    domain logic.
- [ ] Move draw-only routines into `src/gui/*.cpp` by workflow:
  - review / detection browsing
  - bbox editing
  - stimulus playback
  - loading / startup flows
  - timeline / overlays
- [ ] Move non-draw routines out of `gui.h` into the correct domain module.
  - CSV parsing and filesystem traversal do not belong in UI code.
  - Thread spawning does not belong in UI code.
  - Triangulation / geometry does not belong in UI code.
- [ ] Reduce `gui.h` to declarations or delete it once call sites are updated.
- [ ] Ban new behavior-heavy header-only UI helpers unless they are genuinely
      tiny and side-effect free.

## Phase 3: Turn `red.cpp` Into A Composition Root

- [ ] Define the maximum responsibilities `red.cpp` is allowed to keep:
  - bootstrapping the app
  - wiring module instances together
  - per-frame dispatch order
  - high-level shutdown
- [ ] Extract feature controllers/services from `red.cpp`:
  - `RecordingLoader`
  - `PlaybackController`
  - `ReviewController`
  - `BBoxEditController`
  - `StimulusController`
  - The portable recording-open lifecycle/timing controller is extracted and
    used by both shells; platform media/storage execution and full loader
    ownership remain to be extracted before `RecordingLoader` is complete.
  - The portable playback transport/timing controller is extracted and used by
    both shells; decoder scheduling, buffer policy, stimulus synchronization,
    and graphics publication remain platform-owned before `PlaybackController`
    is complete.
- [ ] Replace direct mutation of unrelated globals with explicit command calls.
  - Example: "select detection dataset", "apply frame edits", "schedule seek",
    "reload archive", "mark review accepted".
- [ ] Move per-feature initialization and teardown out of `main()`.
- [ ] Keep `red.cpp` focused on orchestration, not storage details or
      per-feature state transitions.

## Phase 4: Break `ZarrDetectionLoader` Into Domain APIs

### Phase 4a: Introduce Public Repository Facades

- [x] Introduce a small shared archive context layer.
  - Hold TensorStore context, kvstore, root path, and common metadata helpers.
  - This replaces the need for every domain service to rediscover the archive.
  - `ArchiveContext` now owns the bounded TensorStore context/kvstore, root and
    recording-root paths, common array specs, and affiliated-media discovery.
    macOS retains it for strict repositories; Linux standard media discovery
    now uses it, while full Linux session ownership remains a later extraction.
- [ ] Split the public loader API by domain, even if implementation initially
      delegates to the current code:
  - `DetectionRepository`
  - `KeypointRepository`
  - `EyeMaskRepository`
  - `StimulusRepository`
  - `MovementRepository`
  - `ReviewWriteRepository`
- [ ] Move call sites toward those facades before moving implementation.
- [ ] Keep `ZarrDetectionLoader` as a backend adapter during this phase.

### Phase 4b: Migrate Implementation Behind Those Facades

- [ ] Shrink `ZarrDetectionLoader` so it is no longer the dominant public type.
- [ ] Move domain-specific data structs beside their domain APIs.
  - Eye-mask types should not live in the same giant public type as movement
    series and detect review status.
- [ ] Keep chunked eye-mask caching local to the eye-mask repository.
- [ ] Keep flattened per-detection offsets/caches local to the detection
      repository.
- [ ] Remove "load everything" assumptions from the public API surface.
  - Loading detections should not imply loading crop images.
  - Loading movement data should not imply loading eye masks.
  - Writing manual detect review should not require the full reader surface.

## Phase 5: Add Narrow Feature Contexts

- [ ] For each major panel/workflow, define a small context struct containing
      only what that feature needs.
- [ ] Recommended first contexts:
  - `ReviewPanelContext`
  - `BBoxEditorContext`
  - `StimulusPanelContext`
  - `PlaybackPanelContext`
  - `RecordingLoaderContext`
- [ ] Pass repositories/services into these contexts explicitly.
- [ ] Do not pass a giant `AppContext` equivalent.
- [ ] If multiple features need the same state, decide whether it belongs in:
  - session state
  - playback state
  - feature state
  - a shared service
  instead of defaulting to "put it in one bigger context".

## Phase 6: Separate Sparse Manual Review Data From Dense Derived Arrays

- [ ] Keep Zarr as the primary store for both sparse and dense data.
- [ ] Define sparse review/edit payloads explicitly instead of forcing every UI
      concern through dense arrays.
- [ ] Use [crimson_zarr_keypoint_editor_plan.md](./crimson_zarr_keypoint_editor_plan.md)
      as the keypoint-specific design reference for refined-run editing and
      review acceptance.
- [ ] Use [crimson_zarr_keypoint_review_window_plan.md](./crimson_zarr_keypoint_review_window_plan.md)
      as the UI/workflow reference for replacing the legacy `Labeling Tool`
      mental model with a Zarr-native review window.
- [ ] For manual detection and keypoint review:
  - expose frame-local read/write APIs
  - preserve chunked storage for dense backing arrays where it matters
  - avoid eager full-archive reads when only current-frame review data is needed
- [ ] Treat CSV/JSON exports as interchange/debug outputs, not the canonical
      storage model.

## Phase 7: Testing And Verification

- [ ] Expand repository-level tests against the miniature fixture Zarr archives.
- [ ] Add tests for chunk-sensitive paths:
  - eye mask chunk reads
  - neighboring chunk prefetch
  - frame-local detection lookup via offsets
- [ ] Add controller/state tests that do not require full GUI boot:
  - seek state transitions
  - dataset switching
  - edit-apply-discard flows
  - review acceptance payload generation
- [ ] Add at least one integration test proving that a long recording can:
  - open
  - seek
  - render current-frame detections
  - access eye masks without bulk-loading unrelated data

## Suggested Execution Order

1. Phase 0: freeze contracts and seams
2. Phase 0.5: land fixture archives and regression tests
3. Phase 1: state split
4. Phase 4a: public repository facades
5. Phase 2: `gui.h` extraction
6. Phase 3: `red.cpp` controller extraction
7. Phase 4b: implementation migration behind repository seams
8. Phase 5: narrow feature contexts
9. Phase 6: sparse-vs-dense review API cleanup
10. Phase 7: tests and acceptance hardening

## Success Criteria

- `src/red.cpp` is mostly orchestration, not domain behavior, and is under
  4,000 lines.
- `src/gui.h` is either deleted or reduced to a narrow declaration-only file of
  roughly 150 lines or less.
- No single public loader class owns detections, keypoints, eye masks,
  stimulus, movement, and writeback together.
- No file under `src/gui/` includes `zarr_loader.h` directly.
- Review and bbox panels do not call TensorStore or kvstore APIs directly.
- Zarr remains the canonical storage layer.
- Chunk-aware paths for large recordings still exist where they matter.
- Fixture tests cover long-recording frame access and chunk-sensitive paths.
- Adding a new UI workflow no longer requires touching all of:
  `red.cpp`, `gui.h`, and `zarr_loader.h`.

## Explicit Anti-Goals

- Replacing Zarr with CSV snapshots.
- Porting `rob_ui_overhaul` literally.
- Introducing a framework-heavy abstraction layer before the current seams are
  stable.
- Pausing feature work for a giant all-at-once rewrite.
- Changing read/write contracts and application structure in the same commit
  without a very strong reason.
