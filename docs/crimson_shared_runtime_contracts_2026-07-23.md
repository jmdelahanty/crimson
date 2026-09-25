# Crimson Shared Runtime Contracts

Date: 2026-07-23

## Purpose

Crimson's macOS and NVIDIA applications should share behavior where the
behavior is independent of the renderer, decoder, window system, and storage
implementation. The safe refactor direction is to extract small portable
contracts and adopt them from both application shells without moving platform
resource ownership into a new cross-platform god object.

This is an incremental refactor. Each slice must keep both applications
buildable and must have headless tests for its backend-neutral behavior.

## Extracted Slices

The portable behavior is split between `crimson_frame_contracts` and
`crimson_runtime_contracts`:

- `playback_clock.*` now exposes `PlaybackTransportController`, which owns the
  logical frame clock, play/pause/seek/step/rate commands, frame clamping,
  readiness gating, and end-of-stream pause. `LogicalPlaybackClock` remains a
  compatibility name for existing helper signatures.

- `session_lifecycle.*` tracks logical session identity, opening, replacement,
  closing, failures, and stale asynchronous completions by generation.
- `loading_progress.*` publishes stable per-product loading state, phase,
  timing, availability, cancellation, and failure snapshots.
- `frame_presentation.*` decides whether a missing candidate frame should hold
  the visible frame or clear it, and records backend-neutral presentation
  metrics.
- `diagnostic_report.*` formats the same session, loading, and presentation
  records for both application shells.
- `session_readiness.*` turns loading snapshots and required/optional product
  rules into one tested decision for modal visibility, interaction gating,
  playback gating, and autoplay permission.
- `session_open_transaction.*` coordinates lifecycle and loading transitions
  for one logical open attempt and rejects stale completions after a newer
  attempt begins. Its progress-settlement helper translates a child loader's
  readiness into parent product completion, failure, or session commit without
  taking ownership of the loader.
- `recording_open_workflow.*` owns the active session-open transaction,
  per-product timing, and child-loader settlement for a recording open. Both
  application shells use this controller while retaining ownership of their
  actual archive, decoder, repository, and GPU work.
- `session_loading_presentation.*` converts loading progress and readiness into
  renderer-independent modal content: title, phase, bounded progress, product
  statuses, visibility, error, and dismissibility.

The Metal shell and the NVIDIA shell consume these contracts. Neither shell
has been replaced, and the contracts do not call platform APIs.

## Ownership Boundary

The shared layer may own:

- value types and immutable snapshots
- state-machine transitions
- generation or request identifiers
- presentation policy decisions
- logical playback position and transport command transitions
- counters and diagnostic formatting

The shared layer must not own:

- GLFW, Cocoa, Metal, OpenGL, CUDA, or TensorRT objects
- AVFoundation or FFmpeg decoder sessions and frame buffers
- TensorStore handles, repositories, worker threads, or cache memory
- ImGui windows or platform event loops
- coordinate transforms, ROI placement, or renderer resource lifetimes

Platform adapters execute a shared decision using their existing resources.
For example, `FramePresentationAction::Present` does not upload a texture; it
only tells the adapter that its selected frame should become visible.

Likewise, a session-open transaction reports that `media` or `archive`
completed, but it does not open either resource. The macOS and NVIDIA adapters
continue to perform those operations and pass the result back to the
transaction.

`RecordingOpenWorkflowController` is the narrow orchestration layer over that
transaction. It centralizes begin, product completion/failure, commit, cancel,
and elapsed-time bookkeeping. Child-loader settlement requires the open
generation explicitly, so a late result from a previous recording cannot
settle a newer open. It deliberately has no archive, TensorStore, decoder,
thread-pool, or renderer dependency.

`PlaybackTransportController` has the same narrow boundary. The Metal and
NVIDIA shells use its logical playback state and accepted command results. The
Metal adapter still issues AVFoundation seeks, while the NVIDIA adapter still
owns buffered resume selection, camera/stimulus seek execution, decoder
requests, and FFmpeg/CUDA resources. Disabling transport controls immediately
anchors and pauses the logical clock, so readiness cannot race an autoplay or
late UI command.

The shared ImGui loading modal lives in `src/gui/session_loading_modal.*` rather
than in either renderer shell. It consumes only the portable presentation
value. It does not poll events, start work, install repository results, control
playback, or own graphics resources.

## Readiness Semantics

Product completion and product availability are separate:

- Every product included in a strict readiness plan must reach a terminal
  state before the loading UI releases the session.
- An optional product may complete unavailable without blocking the session.
- A required product must complete available or readiness becomes blocked.
- Missing products in an otherwise terminal loading snapshot are treated as a
  contract error rather than silently ignored.
- Autoplay is disabled by default and is independent of readiness. Reaching
  ready enables playback controls but does not start the playback clock.

The interactive macOS analysis path uses the strict policy. Its loading modal
cannot be dismissed into the background, analysis presentation remains gated,
and the last published loader result is installed before controls are enabled.
Smoke and deterministic reference paths may start playback only after their
loading work has already reached ready.

The transaction holds only generation-aware bookkeeping. A new transaction
invalidates older handles, and destroying the coordinator invalidates handles
that may still be held by asynchronous callbacks. It does not cancel or join
the platform work represented by those handles.

`settleSessionOpenFromProgress()` is the portable bridge between an independent
loader and that transaction. It evaluates the loader snapshot under an
explicit readiness policy, publishes the bound parent product once the loader
is terminal, and commits or fails the parent session. Platform code still
drains and installs completed repository results before invoking the bridge.

## Safety Rules

1. Extract behavior only after identifying the existing behavior on both
   platforms.
2. Keep the portable API free of platform and storage headers.
3. Integrate one call site at a time and preserve the old owner of every
   resource.
4. Test state transitions and policy decisions headlessly before relying on a
   GUI smoke.
5. Build and run the portable tests on macOS and Linux after each slice.
6. Treat a shared contract as a coordination surface, not as permission to
   merge all application state or all background work into one object.

## Deferred Boundaries

ROI inset placement, mirroring semantics, acquisition-to-analysis coordinate
transforms, and persisted coordinate provenance should not be generalized
beyond their already agreed backend-neutral presentation inputs until the
Palette coordinate contracts stabilize. Renderer-specific texture upload,
vertex/scissor setup, and GPU lifetime management should remain separate even
after those contracts stabilize.

The next low-risk extractions are UI-independent command/result types and
small workflow controllers whose dependencies can be injected narrowly.
Moving decoder scheduling, repository ownership, or the complete contents of
`red.cpp` or `crimson_macos_main.mm` into a single shared application object is
not a target.

## Verification

The runtime contract tests are labelled `headless;portable;runtime` in CMake:

- `session_lifecycle_tests`
- `loading_progress_tests`
- `frame_presentation_tests`
- `diagnostic_report_tests`
- `session_readiness_tests`
- `session_open_transaction_tests`
- `recording_open_workflow_tests`
- `session_loading_presentation_tests`

`playback_transport_tests` is labelled
`headless;playback;portable;transport` and covers command acceptance, readiness
gating, clamping, rate continuity, timeline changes, and end-of-stream pause.

`imgui_semantic_snapshot_tests` additionally verifies that the shared loading
modal is submitted as a visible ImGui window in a headless frame.

The macOS suite also exercises the shared loading snapshot through
`apple_analysis_repository_loader_tests`. GUI smokes remain valuable for final
renderer and interaction acceptance, but they are no longer the only coverage
for these behaviors.
