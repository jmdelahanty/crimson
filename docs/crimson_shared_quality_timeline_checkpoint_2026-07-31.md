# Crimson Shared Quality Timeline Checkpoint

Date: 2026-07-31

Status: implemented and validated on macOS and the isolated Linux/NVIDIA build;
native Windows validation remains pending

## Outcome

Detection and keypoint quality timelines now use one backend-neutral C++
presentation and session boundary across Crimson's application shells.

The shared implementation owns:

- timeline controls, loading/error states, plot preparation, and click-to-seek
  requests in `src/gui/quality_timeline_window.*`;
- asynchronous exact-schema repository opening, shared-scheduler requests,
  page/overview buffers, and lifecycle cleanup in
  `src/gui/quality_timeline_session.*`;
- canonical/refined detection selection and explicit keypoint-v2 artifact
  selection; and
- window availability and visibility through the portable workspace state.

The macOS UI retains thin Apple-named wrappers for compatibility, but delegates
the timeline windows to the shared presentation. The Linux/Windows application
path creates the same shared session, exposes both windows from the View menu,
and routes plot navigation through its existing playback session controller.

## Playback Shortcut Correction

Comma and period previously depended on an adjacent selectable entry inside the
paused decoder-ring window. They silently did nothing when the logical cursor
had no neighboring ring entry or the buffer window was collapsed and therefore
did not process its contents.

Playback shortcuts now resolve through the portable workspace policy:

- comma and left arrow request one exact frame backward;
- period and right arrow request one exact frame forward;
- Shift accelerates only the arrow shortcuts to ten frames;
- Space toggles playback; and
- all playback shortcuts are suppressed while text input is active.

The resulting step uses the normal playback seek contract. A backend may still
satisfy the request from an existing decode-ring entry, but ring membership no
longer determines whether stepping is allowed.

The Mac pause transition now matches the maintained Linux policy. It anchors
the logical clock to the frame actually being presented and selects that frame
inside the existing AVFoundation decode buffer. It does not clear, seek, or
refill the buffer when the presented frame is resident. Exact frame steps and
timeline seeks also reuse resident frames; a hard decoder seek is issued only
when the requested frame is absent.

## Compatibility Boundary

This checkpoint does not replace the Linux/Windows legacy monolithic Zarr
loader. It adds exact-schema quality sidecars alongside that loader. Legacy
detection and keypoint layouts remain behind their existing compatibility
paths and cannot silently satisfy the new repository contracts.

Subject masks, subject shape, eye geometry, scientific editing, and cross-
surface ROI selection remain outside this checkpoint while their Palette
contracts are being finalized.

## Verification

The checkpoint is covered by:

- portable workspace shortcut, command, and visibility tests;
- detection and keypoint timeline repository/buffer tests;
- the complete macOS headless/Metal suite;
- an isolated Linux CUDA 12.4, architecture 86 `redgui` build; and
- focused Linux workspace, detection-quality, and keypoint-quality tests.

Native Windows compilation and runtime inspection remain explicit follow-up
work on the Windows laptop.
