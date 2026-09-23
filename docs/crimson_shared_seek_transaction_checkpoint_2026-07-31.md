# Crimson Shared Seek Transaction Checkpoint

Date: 2026-07-31

Status: implemented; macOS and isolated Linux/NVIDIA verification complete

## Outcome

Crimson now has a backend-neutral seek transaction contract in
`src/playback_seek.*`. It is used by the shared camera controls and by both the
macOS/AVFoundation and Linux/NVIDIA application adapters.

The portable contract owns:

- preview, committed, and discrete seek phases;
- camera-control, keyboard, timeline, and programmatic origins;
- approximate-allowed versus exact accuracy policy;
- logical-cursor, resident-buffer, and backend-decoder execution paths;
- monotonically increasing generations and supersession;
- stale-result rejection and active-request cancellation; and
- common request, outcome, queue-time, service-time, and summary telemetry.

The contract clamps a request once against the authoritative signed 64-bit
frame count. A committed slider seek and a discrete frame step require exact
positioning. A drag preview may be approximate, but only when the adapter
declares an implementation for that capability.

## Adapter Policy

The macOS adapter declares logical-cursor preview, exact backend seek, and
resident-frame selection. Dragging the slider therefore moves the logical
cursor without repeatedly flushing AVFoundation. Committing the drag first
selects an exact buffered frame and otherwise submits one exact decoder seek.

The Linux/NVIDIA adapter declares approximate backend preview, exact backend
seek, and resident-ring selection. Dragging may use the legacy approximate
decoder path; committing and frame stepping prefer the paused ring and
otherwise issue an exact decoder seek. The controller now returns a typed
execution result, and its seek poller returns the terminal decoder result
before resetting legacy state to idle.

Metal, AVFoundation, CUDA, OpenGL, FFmpeg, clipped-media switching, stimulus
synchronization, decoder rings, and native texture handles remain outside the
portable contract. This keeps the execution policy reusable without pretending
that the backends share decoder mechanics.

## Telemetry

Each transaction records its generation, phase, origin, target, accuracy,
execution mode, selected execution path, resolved frame, queue time, service
time, status, and error. Linux emits these fields into its existing playback
and clipped-state traces. Both applications emit a common shutdown summary as
`[AppleTransportSeek]` or `[NvidiaTransportSeek]`.

A newer generation supersedes the active generation. A completion reported for
an older generation is classified as `discarded_stale` and cannot clear or
publish over the newer request. Session close cancels any active transaction.

## Verification

Portable tests cover capability-specific planning, frame clamping, unsupported
accuracy, generation supersession, stale completion, resident-buffer
completion, cancellation, and common diagnostics. The shared semantic UI test
continues to cover preview-versus-commit identity and frame counts beyond the
signed 32-bit range.

The complete macOS suite passes 69/69 serially, including portable transport,
semantic UI, AVFoundation, Metal, and application-shell coverage.

An isolated Linux/NVIDIA checkout builds the full `redgui` target with CUDA
12.4 and TensorRT 10. Focused playback-transport, workspace, detection-quality,
and keypoint-quality tests pass 4/4. The shared workstation checkout was not
modified.

Native Windows compilation and hands-on macOS/Linux GUI seek smokes remain
separate release gates.
