# Crimson Shared Camera Transport Checkpoint

Date: 2026-07-31

Status: implemented; macOS and isolated Linux/NVIDIA verification complete

## Outcome

The macOS and Linux/Windows camera windows now consume one backend-neutral
ImGui transport presentation in `src/gui/camera_view_transport_controls.*`.
The shared component owns:

- back-ten, previous, play/pause or restart, next, and forward-ten controls;
- transport tooltips and the current/total time readout;
- a signed 64-bit frame slider;
- preview-seek versus committed exact-seek action identity;
- bounded portable playback intents; and
- keyboard capture through the existing portable shortcut policy.

The contract uses a frame count and an explicit maximum frame number. It does
not inherit the legacy decoder's historically inconsistent
`estimated_num_frames` meaning.

## Platform Adapters

The shared component does not decode or present video.

The maintained Linux/Windows adapter retains:

- approximate decoder seeks while the slider is actively dragged;
- an exact seek when the drag is committed;
- paused decoder-ring selection and refill behavior;
- clipped-media routing and stimulus synchronization; and
- existing playback and frame-synchronization telemetry.

Linux's legacy decoder state still stores some frame values in `int`. The
shared result remains `int64_t`; the application adapter bounds values before
passing them to the legacy decoder API. This is a compatibility boundary, not
a Linux operating-system requirement.

The macOS adapter retains:

- a clock-only preview while the slider is actively dragged;
- exact resident-buffer selection when the committed frame is available;
- AVFoundation decoder seek fallback when it is not available; and
- the buffer-preserving pause behavior accepted in the preceding checkpoint.

Metal, CUDA, OpenGL, AVFoundation, FFmpeg, native texture handles, and decoder
ring ownership do not enter the shared presentation header.

## Verification

The headless semantic UI test renders the transport with a 5,000,000,001-frame
timeline and a current frame above the signed 32-bit range, proving that the
shared presentation does not narrow frame identity. Portable playback intent
and shortcut tests continue to cover boundary clamping, single-frame steps,
ten-frame arrow steps, and text-input suppression.

The complete macOS suite passes 69/69 when run serially, including transport,
workspace, semantic UI, AVFoundation, Metal, and application shell coverage.
An earlier parallel run encountered transient media-encoder and Metal-resource
contention; all affected tests pass in the coherent serial run.

An isolated Linux/NVIDIA checkout builds the full `redgui` target with CUDA
12.4 and TensorRT 10. The focused portable suite passes 4/4 for playback
transport, workspace state, detection quality timelines, and keypoint quality
timelines. The shared workstation checkout was not modified.

Native Windows compilation and hands-on GUI playback remain separate release
gates; neither is represented by these headless and isolated-build results.
