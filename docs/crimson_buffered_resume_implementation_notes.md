# Buffered Resume Implementation Notes

Date anchored: 2026-04-07.

Working document for cross-agent coordination.

Related docs:

- `docs/crimson_buffered_frame_resume_design.md`
- `docs/crimson_contiguous_playback_window_design.md`
- `docs/crimson_live_playback_bidirectional_buffer_todo.md`

## What Exists Today

### Pause-intent tracking

`PlaybackState` now has two fields for tracking whether the user browsed
buffered frames after pausing:

- `paused_frame_on_toggle` — the camera frame that was displayed when the user
  pressed pause
- `buffer_browsed_since_pause` — set true when the user selects a buffered frame
  that differs from `paused_frame_on_toggle`

These are set in three places in `src/red.cpp`:

- clicking a frame in the buffer browser list (line ~1373)
- pressing comma to step backward through buffered frames (line ~1390)
- pressing period to step forward through buffered frames (line ~1405)

All three compute `buffer_browsed_since_pause` the same way:

```cpp
ps.buffer_browsed_since_pause =
    (ps.paused_frame_on_toggle >= 0 &&
     item.frame != ps.paused_frame_on_toggle);
```

Both fields are reset on pause (line ~385-386) and after resume (line ~378).

### The three resume paths in applyPlaybackToggle

`PlaybackSessionController::applyPlaybackToggle()` in
`src/playback_session_controller.cpp:355-391` has two branches when resuming
(`play_video` becomes true). Together with the underlying `seekToFrame`
sub-paths, there are effectively three paths play-resume can take:

#### Path 1: Normal smooth resume (the one that works well)

Condition: `!browsed_since_pause` (user paused and pressed play without
browsing any other buffered frame).

Code: calls `syncPlaybackStartToCurrentFrame()`.

What it does:

- resets playback clock (`accumulated_play_time`, wall-time anchors) from the
  current `to_display_frame_number`
- finds a matching slot via `findDisplaySlotForFrame()` and sets `read_head`
- un-throttles stimulus, re-enables stimulus decode
- does NOT issue any camera seek
- does NOT issue any stimulus seek

This is the fast, smooth path. Camera decode just picks up from wherever the
decoder left off. Stimulus follows via the per-tick alignment lookup at
`red.cpp:1439-1448`. No SeekState machine involvement.

#### Path 2: Browsed-resume via full hard seek (the one that's rough)

Condition: `browsed_since_pause` (user browsed to a different buffered frame
after pausing, then pressed play).

Code: calls `seekToFrame(resume_frame, false, true)`.

The `false` for `prefer_buffer_when_paused` means the buffer-hit shortcut at
`seekToFrame:217` is skipped. The `true` for `force_inaccurate` means
`seek_accurate` is forced false.

What it actually does (the full generic seek path, `seekToFrame:262` onward):

- increments `seek_progress->seek_id`
- sets `SeekState::WaitingCameras`
- resets `to_display_frame_number`, `read_head`, `accumulated_play_time`,
  `slider_frame_number`, wall-time anchors
- computes stimulus alignment and sets `current_stimulus_frame`
- calls `initiate_camera_seeks()` — triggers decoder recreation and seek on
  every camera
- the SeekState machine then waits for all cameras to settle, then issues a
  stimulus seek with `use_seek = true`, waits for stimulus settle

This is the same path used for a 10,000-frame slider drag. It recreates
decoders, issues a full stimulus hard-seek, and runs the entire two-phase
settle state machine. That is why stimulus feels rough on resume from a browsed
buffered frame — the browsed frame is already in the ring buffer, but the code
treats it like a random seek to an arbitrary position.

#### Path 3: Buffer-hit shortcut during paused seek (not used for resume)

There is a third path inside `seekToFrame()` at line 217:

```cpp
if (prefer_buffer_when_paused && stepPausedFrameFromBuffer(clamped_frame)) {
    // ... stimulus seek only, skip camera seek
    return;
}
```

This is used by `stepFrames()` (comma/period keys during active playback are
paused-step, not buffer browsing) and by explicit paused seeks. It skips camera
decoder recreation and only issues a stimulus seek if alignment exists.

This path is NOT used for play-resume because `applyPlaybackToggle` calls
`seekToFrame(resume_frame, false, true)` — the `false` skips this branch.

### Which path is actually used for browsed-resume?

**Path 2.** Full hard seek. Every time.

That is the problem. The user browsed 15 frames backward in a buffer that
already contains the frame, and we respond by recreating every camera decoder
and issuing a full stimulus hard-seek through the two-phase settle machine.

## Per-Tick Stimulus Alignment

Every render tick in `red.cpp:1439-1448`:

```cpp
if (zarr_loaded && zarr_loader.hasStimulusAlignment()) {
    int stim_source_frame = ps.play_video ? current_frame_num
                                          : ps.to_display_frame_number;
    if (auto stim_frame = zarr_loader.getStimulusFrameForCameraFrame(stim_source_frame)) {
        ps.current_stimulus_frame = *stim_frame;
    } else {
        ps.current_stimulus_frame = -1;
    }
}
```

This means `ps.current_stimulus_frame` is recomputed every frame regardless of
whether a seek was issued. During playback, stimulus naturally follows the
camera frame through this lookup. During paused browsing, it also silently
updates — but since no stimulus seek is issued, the stimulus decoder does not
actually chase the new value. The stimulus display stays on whatever it last
decoded.

This is mostly fine, but it means `ps.current_stimulus_frame` drifts during
paused browsing even though the stimulus display does not move. An explicit
freeze (guard the lookup with `ps.play_video || !ps.pause_seeked`) would make
the intent clearer and prevent downstream code from accidentally consuming the
drifted value.

## Stimulus Is Not Actually Frozen During Paused Browsing

The design docs say "stimulus should not follow paused inspection clicks." But
tracing the actual code reveals stimulus is only **partially** frozen — and
during Path 2 resume, stimulus can briefly show a frame that does not match
what the camera is displaying.

### What happens during paused browsing

Three things interact:

1. **Per-tick alignment lookup** (`red.cpp:1439-1448`): every render frame,
   regardless of pause state, this runs:

   ```cpp
   int stim_source_frame = ps.play_video ? current_frame_num
                                         : ps.to_display_frame_number;
   if (auto stim_frame = zarr_loader.getStimulusFrameForCameraFrame(stim_source_frame)) {
       ps.current_stimulus_frame = *stim_frame;
   }
   ```

   When the user clicks a buffered frame, `to_display_frame_number` changes,
   so `current_stimulus_frame` updates to the aligned stimulus frame for the
   browsed camera frame. This happens immediately, every tick.

2. **Stimulus presenter** (`stimulus_playback_windows.cpp:57,219-229`): reads
   `target_stimulus_frame = playback_state.current_stimulus_frame` and, if the
   stimulus window is visible, checks:

   ```cpp
   if (mapping_available && target_stimulus_frame >= 0 &&
       target_stimulus_frame != stimulus_player.last_displayed_frame) {
       int buffer_index = findStimulusBuffer(stimulus_player, target_stimulus_frame);
       if (buffer_index != -1) {
           // ... uploads and displays the frame
       }
   }
   ```

   So if the aligned stimulus frame for the browsed camera frame happens to
   exist in the stimulus ring buffer, it gets uploaded and displayed. The
   stimulus display **does follow paused browsing** when the buffer contains
   the matching frame.

3. **Decoder re-enable on miss** (`stimulus_playback_windows.cpp:193-204`):
   even when `base_decode_request` is false (paused, pause_seeked), a fallback
   check re-enables the decoder if the target stimulus frame is not close to
   anything in the buffer:

   ```cpp
   if (!decoder_requested && target_stimulus_frame >= 0) {
       int candidate_index = findStimulusBuffer(stimulus_player, target_stimulus_frame);
       if (!isStimulusFrameClose(candidate_frame, target_stimulus_frame)) {
           decoder_requested = true;
       }
   }
   ```

### The result: inconsistent freeze

Paused browsing produces three different stimulus behaviors depending on buffer
state:

| Buffer state for aligned stimulus frame | What happens |
|----------------------------------------|--------------|
| Frame is in stimulus buffer | Stimulus display jumps to match browsed camera frame |
| Frame is NOT in buffer but nearby frame is | Stimulus stays on last displayed (but `current_stimulus_frame` has drifted) |
| Frame is NOT in buffer and nothing close | Stimulus decoder gets re-enabled, may decode and display it |

The user sees stimulus sometimes follow and sometimes not, depending on what
the stimulus decoder happened to have buffered. This is not the design intent.

### What happens during Path 2 resume specifically

When the user presses play from a browsed frame and Path 2 fires:

1. `applyPlaybackToggle` sets `play_video = true` (line 359)
2. Calls `seekToFrame(resume_frame, false, true)` (line 374)
3. Inside `seekToFrame`:
   - `read_head` is reset to 0 (line 279)
   - `current_stimulus_frame` is set to the alignment for `resume_frame` (line 295)
   - `SeekState::WaitingCameras` begins (line 268)
   - `initiate_camera_seeks` fires on all cameras (line 300)
4. Camera decoders begin recreating/seeking

During the settle window (WaitingCameras, then WaitingStimulus):

- `play_video = true`, so the per-tick lookup at `red.cpp:1430-1437` reads:

  ```cpp
  int live_frame = scene->cameras[0].display_buffer[ps.read_head % scene->size_of_buffer].frame_number;
  ```

  But `read_head` was just reset to 0, and the cameras are mid-seek. Slot 0
  may contain a stale frame from before the seek, or it may be invalid. So
  `current_frame_num` can be wrong.

- The per-tick alignment at `red.cpp:1440` then computes
  `current_stimulus_frame` from this potentially-wrong `current_frame_num`.

- Meanwhile, `pollSeekState` also writes `current_stimulus_frame` when cameras
  settle (line 459).

So during the settle window there are **two concurrent writers** of
`current_stimulus_frame`:

- The per-tick alignment lookup (writing from whatever frame happens to be in
  slot 0 or wherever `read_head` points)
- The seek settle logic (writing the correct value once cameras finish)

The stimulus presenter reads `current_stimulus_frame` each tick and may upload
whichever value it sees. This can produce:

- A brief flash of the wrong stimulus frame (from the stale slot 0 value)
- Then the correct stimulus frame once settle completes

Whether the user notices depends on how fast cameras settle. On fast machines
it may be one frame; on loaded systems it could be several.

### Bottom line for Jadewise's question

**No.** During Path 2 resume, the stimulus frame shown can briefly NOT match
the camera frame the eye sees, because:

- `read_head = 0` points at a stale camera slot during the settle window
- The per-tick alignment uses that stale camera frame to compute stimulus
- The seek settle logic overwrites it once cameras finish, but not atomically

And even before resume, during paused browsing itself, stimulus is not
reliably frozen — it follows browsing when the aligned frame happens to be in
the stimulus buffer, and stays put when it is not.

### What this means for the proposed fix

The `resumeFromBufferedFrame()` approach avoids this problem because:

- It does NOT reset `read_head` to 0 — it finds the actual matching slot
- It does NOT enter the SeekState machine — no settle window
- The per-tick alignment runs on the next tick with the correct camera frame
- There is no race between two writers of `current_stimulus_frame`

The optional stimulus freeze during paused browsing (guarding the lookup with
`ps.play_video || !ps.pause_seeked`) would additionally prevent the
inconsistent follow behavior during browsing itself.

## Proposed Change: resumeFromBufferedFrame()

Add a new method that sits between `syncPlaybackStartToCurrentFrame()` (too
simple — does not reset the target frame) and `seekToFrame()` (too heavy —
recreates decoders and hard-seeks stimulus).

### What it should do

1. Reset the playback clock to the browsed frame (same math as
   `syncPlaybackStartToCurrentFrame`, but from `resume_frame` instead of
   `to_display_frame_number`)
2. Set `to_display_frame_number` and `slider_frame_number` to `resume_frame`
3. Find a matching slot for the browsed frame via `findDisplaySlotForFrame()`
   and set `read_head` — the frame is already in the buffer, so this should
   succeed
4. Re-enable camera decode requests (so the decoder starts filling forward)
5. Un-throttle stimulus and re-enable stimulus decode — but do NOT issue a
   stimulus hard-seek
6. Do NOT call `initiate_camera_seeks()` — no decoder recreation
7. Do NOT enter the SeekState machine — no settle phase needed

### Why this is safe

- The browsed frame is already decoded and in the ring buffer (the user just
  looked at it while paused)
- The decoder was paused (decode requests disabled via `play_video` flag), so
  no eviction race — the ring buffer is frozen while paused
- Stimulus will naturally pick up the correct alignment on the next render tick
  via the per-tick lookup at `red.cpp:1439`
- Camera decode resumes from wherever it last stopped, which is fine — if the
  decoder is ahead of the browsed frame, new frames will fill in ahead; if
  behind, it will catch up

### When this is NOT sufficient

If the browsed frame has been evicted from the ring buffer by the time the user
presses play, the slot lookup will fail. In the current code this cannot happen
(decode is disabled while paused, so no eviction occurs), but a future
window-aware eviction policy could change that. The fallback should be: if
`findDisplaySlotForFrame` returns -1, fall through to the full `seekToFrame`
path.

### Change to applyPlaybackToggle

Replace:

```cpp
if (browsed_since_pause) {
    seekToFrame(resume_frame, false, true);
}
```

With:

```cpp
if (browsed_since_pause) {
    resumeFromBufferedFrame(resume_frame);
}
```

### Optional: freeze stimulus during paused browsing

Guard the per-tick stimulus alignment lookup so it does not run during paused
buffer browsing:

```cpp
if (zarr_loaded && zarr_loader.hasStimulusAlignment()) {
    if (ps.play_video || !ps.pause_seeked) {
        // ... existing alignment lookup ...
    }
    // else: paused browsing — keep stimulus frozen at pre-browse value
}
```

This prevents `ps.current_stimulus_frame` from silently drifting to match
browsed frames. The value would only update when playback is active or when a
real seek completes.

## Relationship to Larger Design

This change is compatible with the contiguous playback window design. When
staging windows are implemented:

- `resumeFromBufferedFrame()` would become the "soft resume inside current
  active window" path
- If the browsed frame falls outside the active window, the staging window path
  would be used instead
- Stimulus follow policy would be tied to `ActivePlaybackWindow.playhead_frame`
  rather than individual frame updates

But none of that is needed for this incremental step.

## Risk Assessment

| Change | Risk | Reason |
|--------|------|--------|
| `resumeFromBufferedFrame()` | Low | New isolated method, only called from one place |
| Wire it in `applyPlaybackToggle` | Low | Only changes the browsed-resume case |
| Freeze stimulus during paused browse | Low | Cosmetic guard on existing lookup |
| Normal pause/play path | None | Untouched |
| Slider seeks | None | Untouched |
| Full hard seek path | None | Untouched, still available as fallback |
