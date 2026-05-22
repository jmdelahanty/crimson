# Crimson Buffered Frame Resume Design

Date anchored: 2026-04-07.

## Status Update

As of 2026-05-22, the first-pass buffered resume behavior exists, but the
explicit `BufferedResumePoint` struct described below was not added as a
separate state object.

Current implementation:

- `PlaybackSessionController::resumeFromBufferedFrame()` resets the playback
  clock from the selected frame, finds the exact buffered slot, sets
  `read_head`, and re-enables stimulus decode without a hard stimulus seek.
- `applyPlaybackToggle()` uses `resumeFromBufferedFrame()` only when the
  selected frame is inside the newest contiguous buffered span.
- If the selected frame is in an older sparse island, `applyPlaybackToggle()`
  uses a camera re-anchor seek with `skip_stimulus_hard_seek=true`.
- The per-tick stimulus alignment lookup is frozen during paused buffer
  browsing and seek settle.
- Clipped collection playback additionally has frame-keyed target selection and
  frame-number-based slot release to avoid advancing into missing ring slots.

Remaining design work:

- pull the frame lookup/release helpers out of `src/red.cpp` and the controller
  into a tested playback-buffer module
- make non-clipped live playback use the same frame-keyed helper after tests
- decide whether an explicit `BufferedResumePoint` object is still worthwhile
  once the larger active/staging playback window model exists

Related docs:

- `docs/crimson_contiguous_playback_window_design.md`
- `docs/crimson_live_playback_bidirectional_buffer_todo.md`

## Problem Summary

When playback is paused, Crimson lets the user browse already-decoded camera
frames from the in-memory ring buffer. That browsing path is useful because it
is immediate and does not require a full seek.

The problem appears when the user presses play again from one of those buffered
frames.

The April baseline and follow-up experiments showed two failure modes:

- if playback simply restarts its clock without a real seek, playback can stop
  at the old buffered tail instead of continuing forward from the selected
  frame
- if playback resumes through the full camera+stimulus seek path, camera resume
  can become correct while stimulus playback and seek smoothness regress

The root issue is architectural:

- the paused buffer browser selects a **presentation slot**
- playback resume needs a **canonical session resume point**

Those are not the same thing.

This resume design should now be read as a follow-on to
`crimson_contiguous_playback_window_design.md`: buffered frame resume is one
consumer of the larger playback-window/staging-window model, not a standalone
slot-management fix.

## Current Architecture

Relevant files:

- `src/red.cpp`
- `src/playback_session_controller.cpp`
- `src/gui/camera_view_presenter.cpp`
- `src/stimulus_playback.cpp`
- `src/render.h`

Current paused browsing behavior:

- paused mode enumerates occupied `display_buffer` slots from the visible camera
- selecting a buffered frame updates `ps.to_display_frame_number`,
  `ps.slider_frame_number`, `ps.read_head`, and `ps.pause_seeked`
- this works because paused presentation can directly reuse a matching ring
  buffer slot

Current playback behavior:

- playback is driven by `ps.accumulated_play_time`
- that clock yields a requested camera frame
- presentation looks for a ring-buffer slot near that target frame
- the playback loop advances `ps.read_head` and releases intermediate slots
- stimulus follows the camera frame through zarr alignment

Main architectural mismatch:

- a paused buffered frame is just "a slot that currently contains frame N"
- it is not a durable decoder checkpoint
- it is not a multi-camera synchronization token
- it is not a stimulus resume token

## Why Slot Resume Is Not Sufficient

### 1) Slot identity is camera-local

Paused browsing currently resolves against one visible camera. Playback,
however, uses one shared target frame and advances all cameras together.

So "resume from slot 7" is not meaningful globally:

- camera A slot 7 might contain frame 1200
- camera B slot 7 might contain a different frame or no valid frame
- stimulus has no corresponding slot concept at all

### 2) Playback consumes the ring buffer

During active playback, the loop advances `ps.read_head` and marks prior slots
available to write again. That means paused browsing is looking at transient
buffer occupancy, not a preserved history object with stable ownership.

### 3) Stimulus sync is frame-based, not slot-based

Stimulus resume is derived from camera frame alignment. A correct resume point
therefore has to be expressed in canonical camera-frame terms, not in terms of
which camera ring slot happened to be visible when the user was paused.

### 4) Presentation already assumes transient misses

The camera presenter can fail to find an immediate slot for the requested
playback frame and currently clears the display in that case. That behavior
fits a target-frame model, not a "keep showing this specific buffer slot and
continue from it" model.

## Design Goal

Add a proper **buffered resume point** abstraction so the app can resume
playback from paused buffered browsing without:

- getting stuck at the old buffered tail
- forcing a heavier-than-necessary stimulus seek
- turning trackbar scrubbing into seek spam
- pretending a presentation slot is the same thing as playback state

## Non-Goals

First pass non-goals:

- redesigning the decoder buffer format
- making ring-buffer slots durable checkpoints
- changing zarr alignment contracts
- solving every seek smoothness issue at once

## Proposed Abstraction: Buffered Resume Point

Introduce a small runtime struct owned by playback/session state:

```cpp
struct BufferedResumePoint {
    bool valid = false;
    int camera_frame = -1;
    int visible_camera_index = -1;
    int preferred_slot = -1;
    bool frame_present_in_visible_buffer = false;
};
```

Semantics:

- `camera_frame` is the canonical resume token
- `visible_camera_index` and `preferred_slot` are presentation hints only
- `preferred_slot` may help keep the current paused frame visible on the first
  resume tick, but must never be treated as the source of truth

Key rule:

- playback resumes from `camera_frame`
- slot identity is advisory, not authoritative

## Proposed Resume Semantics

### Paused buffered browsing

While paused and browsing buffered frames:

- update the canonical displayed frame as today
- populate `BufferedResumePoint` with:
  - selected `camera_frame`
  - visible camera index
  - preferred paused slot
- do **not** trigger a full playback seek yet

### Pressing play from paused buffered browsing

When the user resumes playback from a buffered frame:

1. Promote `BufferedResumePoint.camera_frame` to the playback target frame.
2. Reset playback clock from that frame.
3. Keep presentation pinned to the selected slot only as a short-lived hint.
4. Re-enable camera decode.
5. Let cameras converge to the target frame with normal playback progression.
6. Let stimulus follow via camera-frame alignment, with an explicit policy:
   - either one deferred sync at resume
   - or normal catch-up if already close

Important:

- this is not a full general seek
- it is a **resume-from-frame** transition with optional presentation hinting

## Required Policies

### Policy 1: Camera frame is authoritative

All resume logic must resolve from `camera_frame`, not from `preferred_slot`.

### Policy 2: Preferred slot is temporary

Use `preferred_slot` only for first presentation after resume if it still
matches `camera_frame`. Once playback advances or decode catches up, drop the
hint.

### Policy 3: Stimulus should not hard-seek unless necessary

Resume from buffered browsing should prefer:

- "follow camera from this frame"

over:

- "force immediate heavy stimulus reseek every time"

Recommended first-pass stimulus rule:

- if aligned stimulus frame for `camera_frame` is already close to current
  stimulus playback state, resume through normal catch-up
- if it is far away, perform one explicit stimulus sync on resume

### Policy 4: No slider drag seek spam during playback

Trackbar drag and buffered browsing should remain distinct:

- paused browsing may directly choose buffered frames
- active playback drag should not continuously trigger hard seek/reseek cycles

## Original Implementation Plan

This plan is partially implemented by the status update above. The remaining
useful pieces are the explicit state cleanup, presenter hinting, and broader
active-window extraction.

### Phase 1: Add explicit buffered resume state

Files:

- `src/stimulus_playback.h`
- `src/playback_session_controller.cpp`
- `src/red.cpp`

Changes:

- add `BufferedResumePoint` to playback/session state
- populate it from paused-buffer browsing
- clear it on unrelated hard seeks

### Phase 2: Add resume-from-frame transition

Files:

- `src/playback_session_controller.cpp`
- `src/playback_session_controller.h`

Add a dedicated path such as:

```cpp
void resumePlaybackFromBufferedFrame();
```

Responsibilities:

- adopt `BufferedResumePoint.camera_frame`
- reset playback clock from that frame
- restore decode requests
- preserve temporary slot hint for the first resume presentation
- avoid going through the full generic seek path unless necessary

### Phase 3: Teach presenter about temporary resume hinting

Files:

- `src/gui/camera_view_presenter.cpp`

Behavior:

- when resuming from buffered browsing, prefer the hinted slot if it still
  matches the canonical frame
- if no matching frame is currently available, keep the last valid texture
  briefly instead of clearing immediately

This should also remove the black flash observed during resume experiments.

### Phase 4: Define stimulus follow policy explicitly

Files:

- `src/stimulus_playback.cpp`
- `src/playback_session_controller.cpp`

Behavior:

- add a dedicated "resume from buffered frame" stimulus policy
- avoid treating resume as equivalent to a generic hard seek
- use one-shot resync only when alignment drift is too large

### Phase 5: Tighten speed/readout UX

Files:

- `src/gui/file_browser_window.cpp`

Behavior:

- speed readout should suppress or label itself as settling immediately after
  resume or hard seek
- do not present noisy frame-jump measurements as meaningful playback speed

## Suggested Invariants

- `BufferedResumePoint.camera_frame` is always the canonical resume identity.
- Playback resume from paused buffered browsing must not depend on a slot
  remaining occupied after decode restarts.
- Stimulus sync behavior for buffered resume must be explicit and distinct from
  generic hard seek behavior.
- Presentation should prefer continuity over clearing during the first resume
  tick if a valid previous texture exists.

## Validation Plan

### Functional

1. Pause during playback.
2. Browse buffered frames using the paused buffer window.
3. Press play.
4. Confirm:
   - playback continues forward from the selected frame
   - playback does not stop at the prior buffered tail
   - stimulus stays visually smooth
   - no black flash appears on resume

### Stress

1. Repeat the above near the start and end of recordings.
2. Repeat with multiple visible cameras.
3. Repeat with stimulus loaded and not loaded.
4. Alternate between:
   - paused buffer browsing
   - random trackbar seeks
   - normal play/pause

### UI/UX

1. Verify buffered browsing still feels instantaneous while paused.
2. Verify play-resume does not feel like a full restart.
3. Verify playback speed readout does not oscillate wildly after resume.

## Recommendation

Do not patch this by teaching playback to "keep going from the currently shown
slot."

Instead:

- formalize a `BufferedResumePoint`
- keep `camera_frame` as the canonical resume identity
- treat slot reuse only as a temporary presentation optimization
- give stimulus an explicit buffered-resume policy instead of reusing the hard
  seek path

That is the lowest-risk way to make paused buffer browsing and playback resume
work together without destabilizing the smoother playback path that already
works well today.
