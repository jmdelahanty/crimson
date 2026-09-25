# Crimson Playback Remaining Work

Date: 2026-08-03

Lifecycle: **active consolidated worklist**.

## Purpose

This document is the current authority for playback work that remained after
reconciling the older seek, buffering, resume, and decoder-artifact plans. It
tracks observable gaps and acceptance gates, not one assumed decoder design.

## Accepted Baseline

The following work is implemented and is not part of this TODO:

- `src/playback_clock.*` owns backend-neutral 64-bit play, pause, seek, step,
  rate, readiness, clamping, and end-of-stream policy.
- `src/playback_seek.*` owns preview, committed, and discrete seek intent;
  exactness policy; generation supersession; cancellation; stale-result
  rejection; and queue/service telemetry.
- macOS/AVFoundation and Linux/NVIDIA both adapt that seek contract while
  retaining their native decoder mechanics.
- `src/frame_selection.*` provides shared exact, latest-at-or-before, nearest,
  preferred-slot, and retained-frame selection policy. The macOS playback
  buffer, Linux controller, and camera presenter use it.
- Paused exact frames already resident in the applicable buffer can be selected
  without a decoder seek. Committed misses still use an exact backend seek.
- Linux and macOS have maintained GUI playback smokes, while portable and
  semantic tests cover transport requests, generation handling, and UI state.

The implementation and verification checkpoint is
`docs/crimson_shared_seek_transaction_checkpoint_2026-07-31.md`.

## Remaining Work

### 1. Active And Staging Playback Windows

Frame selection is shared, but buffer occupancy and eviction are not. Linux
still has ring-specific advancement/release policy in `src/red.cpp`; macOS owns
an AVFoundation-specific bounded buffer. Neither backend exposes a portable
description of:

- the contiguous range ready for immediate playout;
- retained history and decoded lookahead;
- a staging range being prepared after a seek or discontinuity; or
- a sparse inspection cache that must not imply resumable continuity.

The next extraction should be a backend-neutral policy/model over frame IDs and
slot states. It must not attempt to share CUDA surfaces, `CVPixelBuffer`
objects, decoder threads, or native eviction mechanics.

Acceptance:

- report active contiguous, staging, history, and lookahead ranges;
- use resident frames for short backward/forward steps when exact data exists;
- stage a new contiguous range before resuming from a sparse island;
- keep memory bounded and allow each backend to declare its capacity;
- cover sparse and wrapped slot layouts with headless tests; and
- preserve forward progress so history retention cannot starve decoding.

### 2. Stimulus Settlement After Camera Seeks

The shared seek transaction intentionally excludes stimulus decoder mechanics.
The maintained NVIDIA application still coordinates camera settlement and
stimulus settlement through its platform session/controller path. This must be
validated as a dependent operation of the authoritative camera seek generation.

Acceptance:

- the UI event loop never blocks while waiting for camera or stimulus decode;
- a stimulus result from a superseded camera seek cannot publish;
- pause state cannot disable stimulus decoding while the active target is
  unsettled;
- timeout/cancellation leaves both streams recoverable; and
- repeated paused seeks end with matching camera and stimulus identities.

### 3. NVIDIA Post-Seek Artifact Closure

The historical investigation established decoder recreation on seek as the
known-good NVIDIA guardrail. It did not produce a final runtime record closing
the original stale/fading-frame reproduction. Treat this as an acceptance gap,
not evidence that the artifact is still present today.

Run the archived buffer-dump workflow on the original affected media and a
current representative recording. If it reproduces, first distinguish decoded
payload corruption from presentation of an invalid/stale slot. Do not weaken
decoder recreation until an alternative flush/prime sequence passes the same
gate.

Acceptance:

- repeated random seeks show no stale or fading content while paused or
  playing;
- requested, decoded, selected, and presented frame identities agree;
- buffer dumps contain internally consistent metadata and pixels; and
- the result is recorded against the executable revision and recording.

### 4. Weak-GPU Playback Rendering

Compact NV12 slots and late conversion are implemented. The remaining Windows
RTX A1000-class issue is the final camera presentation cost, not the old eager
RGBA conversion. The active deferred plan is
`docs/crimson_main_camera_playback_renderer_plan.md`.

This performance work is independent of active/staging buffer policy and must
preserve zoom, pan, overlay alignment, and paused full-fidelity inspection.

## Platform Boundary

The portable layer may own frame identities, ranges, readiness, policy,
generation, cancellation, and telemetry. Platform adapters continue to own
AVFoundation, FFmpeg/NVDEC, native decode surfaces, CUDA/Metal/OpenGL resources,
and the concrete work required to fill or evict a slot.

The goal is behavioral parity through a common contract, not one decoder or one
buffer implementation on every operating system.

## Verification Matrix

Before closing this worklist:

1. Run portable frame-selection, seek, and active/staging-window tests.
2. Run the complete macOS headless suite and a real-window pause/step/seek
   smoke.
3. Run the isolated Linux/NVIDIA focused suite and authenticated playback
   smoke, including stimulus when the recording provides it.
4. Record Windows runtime evidence separately; do not infer it from Linux.
5. Confirm no Zarr contract, selector, or archive mutation was introduced.

## Archived Background

Detailed historical evidence and discarded implementation prescriptions live
under `docs/archive/playback/`. In particular:

- `crimson_seek_event_refactor_todo.md` predates the shared seek contract;
- `crimson_decode_seek_artifact_todo.md` retains the original dump workflow;
- the bidirectional, contiguous-window, buffered-resume, and clipped-sync notes
  explain the active/staging model; and
- the preview-scale, late-conversion, and zoom-aware plans explain the renderer
  evidence that led to the current active renderer plan.
