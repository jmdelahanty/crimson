# Crimson Seek Refactor: Lean Plan

Date anchored: 2026-02-11.

Related follow-on design note:

- `docs/crimson_buffered_frame_resume_design.md`

## Observed Problem

After seeking the camera video (slider release, keyboard step, event click),
the stimulus window shows:

- "Waiting for stimulus frame..."
- Latest decoded stimulus frame: -1
- Valid frames: 0 / 100

The stimulus buffer is empty after a seek. The decoder either never completes the
seek, or gets shut off before it can write frames. Root causes identified:

1. `seek_all_cameras` blocks the UI thread (sequential per-camera polling,
   5-second timeout each). During the block the stimulus decoder may run, but
   `scheduleStimulusSeek` then wipes the buffer and re-seeks.
2. `pause_seeked` is a single flag shared between camera and stimulus concerns.
   When `stabilizePausedSeekFrame` sets it `true`, the stimulus decode request
   (`base_decode_request = ps.play_video || !ps.pause_seeked`) becomes `false`,
   shutting off the stimulus decoder via `window_need_decoding` before the
   target frame is decoded.
3. If the stimulus seek times out (5 seconds) or the decoder fails silently,
   no recovery path re-enables decoding. The buffer stays at 0 frames permanently
   until the next seek or play/pause toggle.

## Design Approach

Use the existing shared-state polling architecture (no event queue). Add a
generation counter (`seek_id`) and a small state machine (`SeekState`) polled
on the UI thread each frame. Keep decoder thread changes minimal.

### What we keep from the original plan

- **`seek_id` / generation concept** — monotonic counter for stale-completion
  filtering.
- **Explicit invariants** — display state only advances for the active
  `seek_id`; stimulus seek never executes against a stale camera frame.
- **Zarr contract guardrails** — no changes to zarr loader APIs, palette
  layout, or read contracts during this refactor.

### What we change

- **No event queue.** State machine polled each frame on the UI thread.
- **No `SeekCancelled` event.** Cancellation = superseded by a newer `seek_id`.
  The decoder has no cancel path and we don't add one.
- **File scope is narrow.** Primary changes in `src/red.cpp` and
  `src/stimulus_playback.cpp`/`.h`. Minimal touch to `src/decoder.cpp`
  (add `seek_id` to `SeekInfo`, thread it through `mark_seek_done`).

## Invariants

- At most one `seek_id` is authoritative at any time.
- Display state can only advance from completions matching the active `seek_id`.
- Stimulus seek must not execute against a stale camera frame from an old
  `seek_id`.
- UI thread must not block on decoder wait loops.
- Timeout fails a seek but must not deadlock decode or UI.
- `pause_seeked` must not shut off the stimulus decoder while a stimulus seek
  for the active `seek_id` is still in flight.

## Zarr Contract Guardrails

- Keep existing zarr loader contracts unchanged.
- Coordinator and playback code consume zarr data only through current loader
  APIs.
- Do not change palette layout assumptions.
- If any zarr schema/API change is needed, do it in a separate tracked task.

### Contract References

- `docs/crimson_detect_bbox_read_contract.md`
- `docs/stimulus_alignment_overview.md`
- `docs/crimson_keypoint_read_contract.md`

## SeekState Machine

```
Idle ──▶ WaitingCameras ──▶ WaitingStimulus ──▶ Ready
  ▲            │                    │               │
  │            ▼                    ▼               │
  └──────── TimedOut ◀─────────────┘               │
  └────────────────────────────────────────────────┘
```

- **Idle**: No active seek. Normal playback or paused display.
- **WaitingCameras**: Camera seek requests issued for current `seek_id`.
  Polled each frame. Transitions to `WaitingStimulus` when all cameras
  report `seek_done` for this `seek_id`, or to `TimedOut` on deadline.
- **WaitingStimulus**: Camera settle confirmed. Stimulus seek issued.
  Polled each frame. Transitions to `Ready` when stimulus reports
  `seek_done` for this `seek_id`, or to `TimedOut` on deadline.
  **Stimulus `window_need_decoding` stays `true` in this state regardless
  of `pause_seeked`.**
- **Ready**: Seek complete. Stimulus buffer should contain the target frame.
  Render loop uploads it. Transitions back to `Idle`.
- **TimedOut**: Logged. Transitions to `Idle` with best-effort display.

## Implementation Steps

### Step 1: Add `seek_id` to `SeekInfo` and completion path

**Files:** `src/decoder.h`, `src/decoder.cpp`, `src/stimulus_playback.h`

- Add `uint64_t seek_id` field to `SeekInfo` struct (default 0).
- When the UI sets `use_seek = true`, it also sets the current `seek_id`.
- In `mark_seek_done` (decoder.cpp), copy `seek_id` through to the
  completion so the caller can check it.
- Add `uint64_t settled_seek_id` field to `SeekInfo` for the decoder to
  write back on completion.
- No behavior change yet — existing code ignores the new fields.

### Step 2: Add `SeekState` enum and tracking struct

**Files:** `src/stimulus_playback.h` (or a new small header if preferred)

- Define `enum class SeekState { Idle, WaitingCameras, WaitingStimulus, Ready, TimedOut }`.
- Define `SeekProgress` struct:
  ```
  struct SeekProgress {
      SeekState state = SeekState::Idle;
      uint64_t  seek_id = 0;
      int       target_camera_frame = 0;
      int       target_stimulus_frame = -1;
      bool      accurate = false;
      int       cameras_settled = 0;
      int       cameras_total = 0;
      std::chrono::steady_clock::time_point deadline;
  };
  ```
- Instantiate one `SeekProgress` in the UI scope (local to the main loop
  in `red.cpp`).

### Step 3: Make camera seek non-blocking

**Files:** `src/stimulus_playback.cpp`, `src/red.cpp`

- Split `seek_all_cameras` into two parts:
  - `initiate_camera_seeks(scene, frame, seek_id, accurate)` — sets
    `use_seek = true` with `seek_id` on all cameras. Returns immediately.
  - `poll_camera_seeks(scene, seek_id)` — returns count of cameras whose
    `seek_done == true && settled_seek_id == seek_id`. Non-blocking.
- In `seekToFrame`, instead of calling blocking `seek_all_cameras`:
  - Increment `seek_id`.
  - Call `initiate_camera_seeks`.
  - Set `SeekProgress` to `WaitingCameras` with a deadline.
  - Return immediately (UI stays responsive).
- Each render frame, if state is `WaitingCameras`:
  - Call `poll_camera_seeks`. If all settled, transition to
    `WaitingStimulus` and call `scheduleStimulusSeek` (non-blocking).
  - If deadline passed, transition to `TimedOut`.

### Step 4: Make stimulus seek non-blocking and gated on `seek_id`

**Files:** `src/stimulus_playback.cpp`, `src/red.cpp`

- Remove the blocking `wait_for_completion` loop from
  `scheduleStimulusSeek`. It always returns immediately.
- Each render frame, if state is `WaitingStimulus`:
  - Check `stim.seek.seek_done && stim.seek.settled_seek_id == seek_id`.
  - If done, transition to `Ready`.
  - If deadline passed, transition to `TimedOut`.
- **Critical fix:** While in `WaitingStimulus`, force
  `window_need_decoding[stimulus_player.window_name] = true` regardless
  of `pause_seeked`. This prevents the decoder from being shut off before
  it writes the target frame.

### Step 5: Protect stimulus decode-enable from `pause_seeked`

**Files:** `src/red.cpp`

- In the stimulus window rendering block (~line 5514), change:
  ```cpp
  bool base_decode_request = ps.play_video || !ps.pause_seeked;
  ```
  to:
  ```cpp
  bool seek_needs_stimulus = (seek_progress.state == SeekState::WaitingStimulus);
  bool base_decode_request = ps.play_video || !ps.pause_seeked || seek_needs_stimulus;
  ```
- This is the direct fix for the observed bug: the stimulus decoder stays
  alive while the seek state machine is waiting for it.

### Step 6: Clean up legacy blocking paths

**Files:** `src/red.cpp`, `src/stimulus_playback.cpp`

- Remove the sequential camera polling loop from `seek_all_cameras`
  (or delete the function entirely if fully replaced).
- Remove the blocking `stabilizePausedSeekFrame` wait loop. The state
  machine handles completion polling.
- Remove the `wait_for_completion` parameter and blocking loop from
  `scheduleStimulusSeek`.
- Set `ps.pause_seeked` from the state machine's `Ready` transition
  rather than from `stabilizePausedSeekFrame`.

### Step 7: Observability

**Files:** `src/red.cpp`

- Add a small debug display (in existing stimulus debug panel or camera
  debug panel) showing:
  - Current `seek_id`
  - Current `SeekState` name
  - Cameras settled count / total
  - Time elapsed since seek start
- Use existing `std::cout` logging for seek state transitions.

## Non-Goals (Same as Before)

- Rewriting `FFmpegDemuxer` internals.
- Replacing decoder threads with coroutines.
- Redesigning all playback UI windows.
- Removing accurate seek mode entirely.
- Changing zarr palette loading/read contracts.
- Adding a decoder-side cancel path.
- Introducing an event queue or message bus.

## File-Level Changes Expected

| File | Change Scope |
|------|-------------|
| `src/decoder.h` | Add `seek_id`, `settled_seek_id` fields to `SeekInfo` |
| `src/decoder.cpp` | Thread `seek_id` through `mark_seek_done` |
| `src/stimulus_playback.h` | Add `SeekState` enum, `SeekProgress` struct |
| `src/stimulus_playback.cpp` | Split `seek_all_cameras`, remove blocking waits |
| `src/red.cpp` | Wire state machine, fix `base_decode_request` |

## Acceptance Criteria

- After a paused seek, the stimulus buffer contains the target frame and
  the texture displays it.
- No blocking wait loops in the UI path for seek completion.
- Stale seek completions (old `seek_id`) never update display state.
- `pause_seeked` does not shut off stimulus decoding while a stimulus seek
  is in flight.
- Play/pause/play remains stable.
- Random paused seeks do not deadlock and recover within timeout.
- Zarr palette read contracts remain intact with no behavior regressions.

## Open Decisions

- Camera settle timeout value (currently 5 seconds; may want shorter for
  non-blocking since UI stays responsive).
- Stimulus settle timeout value.
- Whether seek completion requires all cameras or only visible cameras.
- Whether to keep `stabilizePausedSeekFrame` as a fast-path optimization
  or remove it entirely.
