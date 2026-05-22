# Buffered Resume Implementation Notes

Date anchored: 2026-04-07.

Working document for cross-agent coordination.

Related docs:

- `docs/crimson_buffered_frame_resume_design.md`
- `docs/crimson_contiguous_playback_window_design.md`
- `docs/crimson_live_playback_bidirectional_buffer_todo.md`

## Status Update

As of 2026-05-22, this document should be read as historical investigation plus
the design rationale for the current incremental implementation. Several "what
exists today" details below describe the April 2026 state before the buffered
resume and clipped playback sync fixes.

Current implementation:

- `applyPlaybackToggle()` no longer sends every browsed resume through the full
  generic camera+stimulus hard-seek path.
- If the browsed frame is inside the newest contiguous buffered span,
  `resumeFromBufferedFrame()` performs a soft resume by frame number.
- If the browsed frame is outside that span, Crimson uses a camera re-anchor
  seek with `skip_stimulus_hard_seek=true`.
- The per-tick stimulus alignment lookup is frozen during paused buffer
  browsing and seek settle, which removes the two-writer stimulus race described
  below.
- Clipped playback now has a frame-keyed target clamp and frame-number-based
  release path in `src/red.cpp`, so the clipped presenter does not advance into
  missing ring slots after clipped seek/rebase.

Remaining gap:

- the full shared active/staging playback window model is still not extracted
- non-clipped live playback still has older ring-offset advancement in the
  steady playback loop
- frame-keyed buffer lookup/release should move into a tested helper instead of
  staying inline in `src/red.cpp`

Line numbers in the historical sections below are April-era anchors and should
be rechecked before making code edits.

## April 2026 Baseline

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

### The original three resume paths in applyPlaybackToggle

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

#### Original Path 2: Browsed-resume via full hard seek

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

In the April baseline, this path was NOT used for play-resume because
`applyPlaybackToggle` called
`seekToFrame(resume_frame, false, true)` — the `false` skips this branch.

### Which path was originally used for browsed-resume?

Historical April 2026 answer: **Path 2.** Full hard seek. Every time.

Current May 2026 behavior:

- newest contiguous span: `resumeFromBufferedFrame(resume_frame)`
- older sparse island: `seekToFrame(resume_frame, false, true, true)`, which
  re-anchors cameras and skips the hard stimulus seek
- normal pause/play without browsing: `syncPlaybackStartToCurrentFrame()`

That was the problem. The user browsed 15 frames backward in a buffer that
already contained the frame, and the April code responded by recreating every
camera decoder and issuing a full stimulus hard-seek through the two-phase
settle machine.

## April Baseline Per-Tick Stimulus Alignment

In the April baseline, every render tick in `red.cpp:1439-1448` did this:

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

## Historical: Stimulus Was Not Actually Frozen During Paused Browsing

The design docs said "stimulus should not follow paused inspection clicks."
Tracing the April baseline revealed stimulus was only **partially** frozen, and
during original Path 2 resume, stimulus could briefly show a frame that did not
match what the camera was displaying.

### What happened during paused browsing

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

### What happened during original Path 2 resume specifically

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

### Historical bottom line for Jadewise's question

**No.** During Path 2 resume, the stimulus frame shown can briefly NOT match
the camera frame the eye sees, because:

- `read_head = 0` points at a stale camera slot during the settle window
- The per-tick alignment uses that stale camera frame to compute stimulus
- The seek settle logic overwrites it once cameras finish, but not atomically

And even before resume, during paused browsing itself, stimulus is not
reliably frozen — it follows browsing when the aligned frame happens to be in
the stimulus buffer, and stays put when it is not.

### What this meant for the proposed fix

The `resumeFromBufferedFrame()` approach avoids this problem because:

- It does NOT reset `read_head` to 0 — it finds the actual matching slot
- It does NOT enter the SeekState machine — no settle window
- The per-tick alignment runs on the next tick with the correct camera frame
- There is no race between two writers of `current_stimulus_frame`

The current code also freezes stimulus alignment during paused browsing and
seek settle, which prevents the inconsistent follow behavior during browsing
itself.

## Implemented Increment: resumeFromBufferedFrame()

The current code has a method that sits between
`syncPlaybackStartToCurrentFrame()` (too simple for browsed resume) and
`seekToFrame()` (too heavy when it recreates decoders and hard-seeks stimulus).

### What it does

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

The original proposed replacement was:

```cpp
if (browsed_since_pause) {
    seekToFrame(resume_frame, false, true);
}
```

with:

```cpp
if (browsed_since_pause) {
    resumeFromBufferedFrame(resume_frame);
}
```

### Implemented: freeze stimulus during paused browsing and seek settle

The per-tick stimulus alignment lookup now skips paused buffer browsing and
seek-settle windows:

```cpp
const bool freeze_stimulus_during_paused_browse =
    !ps.play_video && ps.pause_seeked && ps.buffer_browsed_since_pause;
const bool freeze_stimulus_during_seek =
    seek_progress.state == SeekState::WaitingCameras ||
    seek_progress.state == SeekState::WaitingStimulus;

if (zarr_loaded && zarr_loader.hasStimulusAlignment()) {
    if (!freeze_stimulus_during_paused_browse &&
        !freeze_stimulus_during_seek) {
        // ... alignment lookup ...
    }
}
```

This prevents `ps.current_stimulus_frame` from silently drifting to match
browsed frames or transient seek-settle frames. The value updates again when
playback is active or the seek has settled.

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

## Historical Update: Why Soft Resume Stalls at the Old Frontier

Date: 2026-04-07. After implementing `resumeFromBufferedFrame()`.

### The stall mechanism

The `resumeFromBufferedFrame()` fix removed jitter and the stimulus race. But
playback can still stop at the old decode frontier. Here is the exact chain:

1. **Decoder is a sequential producer.** In `decoder.cpp`, the decoder thread
   writes frames sequentially into the ring via `buffer_head`, incrementing
   `nFrame` after each frame (line ~675: `nFrame = assigned_frame_num + 1`).
   It advances `buffer_head = (buffer_head + 1) % size_of_buffer` (line ~676).
   It publishes `latest_decoded_frame[cam_name].store(assigned_frame_num)`.

2. **Decoder blocks when the ring is full.** At line ~618, the decoder spins
   waiting for `display_buffer[buffer_head].available_to_write`. If the slot
   is occupied, the decoder sleeps 1ms and retries.

3. **Playback releases slots only forward from read_head.** At `red.cpp:2626`,
   when `frame_delta > 0`, playback marks intermediate slots
   `available_to_write = true` and advances `read_head`. This is the ONLY
   place camera ring slots are freed during active playback.

4. **Playback is clamped to `min_decoded_frame`.** At `red.cpp:2604`:

   ```cpp
   frame_to_show = std::min(frame_to_show, min_decoded_frame);
   ```

   Playback cannot advance past what the decoder has produced.

5. **After `resumeFromBufferedFrame()`, the decoder's position has not
   changed.** The decoder was paused (no decode requests while `!play_video`).
   When decode resumes, it picks up from its last `nFrame` and `buffer_head`.
   Those are internal to the decoder thread — `resumeFromBufferedFrame()` has
   no access to them.

### The stall scenario

User pauses at frame 15588 (the decode frontier). Buffer contains sparse
islands: 15489..15492 and 15588. User browses to frame 15490. Presses play.

- `resumeFromBufferedFrame(15490)` sets:
  - `to_display_frame_number = 15490`
  - `accumulated_play_time` → corresponds to frame 15490
  - `read_head` → slot containing frame 15490

- Playback clock starts ticking from 15490. The clock quickly wants frame
  15491, 15492, then 15493...

- But `latest_decoded_frame` is still 15588. Decode resumes from `nFrame =
  15589`. The decoder writes frame 15589 into its `buffer_head` slot.

- At `red.cpp:2604`, `frame_to_show = min(clock_frame, 15589)`. For the first
  few ticks while clock is < 15589, this seems fine.

- **The problem:** playback at line 2626 requires `frame_delta > 0` to free
  slots. `frame_delta = frame_to_show - ps.to_display_frame_number`. When the
  clock reaches frame 15493 (the start of the gap), there is no frame 15493
  in the ring buffer. But `to_display_frame_number` can still advance if the
  ring slot at `read_head` happens to contain a frame (the playback loop
  reads from the ring at line 1432, not from a frame index).

  Actually, the real issue is simpler: `read_head` walks sequentially through
  ring slots. The frames 15489..15492 and 15588 are in non-contiguous slots.
  When `read_head` advances past the slot holding 15492 to the next slot, that
  slot might hold garbage, be empty, or hold 15588. If it holds 15588,
  playback jumps from 15492 to 15588 in one tick. If it is empty
  (`available_to_write = true`), `live_frame` at line 1432 reads -1, and
  `current_frame_num` falls back to `to_display_frame_number`.

  Meanwhile the decoder is at `buffer_head` pointing to some slot, writing
  frames starting from 15589. The decoder will eventually circle `buffer_head`
  around the ring, overwriting the old sparse islands. But until it wraps,
  playback's `read_head` is walking through stale or empty slots.

### The core issue

At the time of this April investigation, the ring buffer had no frame-indexed
lookup in the active playback advancement path. Playback advanced `read_head`
sequentially through slot indices, assuming the decoder filled them in order
from a nearby starting point. After a soft resume to a different position, that
assumption broke: `read_head` was positioned at a slot in a sparse island, but
the decoder was producing frames far ahead.

The current clipped playback path now has a frame-keyed guard for target
selection and release. The broader non-clipped/live playback path still needs
that logic extracted and unified.

Playback hits the end of the sparse island and either:
- jumps to whatever frame happens to be in the next slot (stutter)
- reads an empty slot and stalls

### Why full seekToFrame does not have this problem

`seekToFrame` calls `initiate_camera_seeks()` which triggers decoder
recreation. The decoder starts fresh from the seek target, writes sequentially
from frame N into `buffer_head = 0`, and playback's `read_head` (also reset
to 0) walks through freshly-decoded sequential frames. No sparse gap problem.

### What this tells us about the two-case split

Your proposed split is correct:

1. **Soft resume** — only valid when the browsed frame is in a span that the
   decoder can naturally continue from. The decoder is sitting at `nFrame`
   beyond the newest buffered frame. If the browsed frame is in the newest
   contiguous span (the one that ends at or near `latest_decoded_frame`),
   playback's `read_head` can walk through those sequential slots and the
   decoder will keep filling ahead of it.

2. **Camera re-anchor** — needed when the browsed frame is in an older sparse
   island that the decoder has moved past. The decoder needs to be told to
   produce frames from a new starting point.

## Answers to Your Three Questions

### Q1: Is "inside newest contiguous span" the right criterion, or a minimum forward-lookahead?

**"Inside newest contiguous span" is the right criterion, but define it
precisely: the span must be contiguous AND end at or near
`latest_decoded_frame`.**

Here is why forward-lookahead alone is not enough:

Consider buffer state: spans [15489..15492] and [15585..15588], with
`latest_decoded_frame = 15588`. If you use a forward-lookahead threshold of
say 8 frames, and the user browses to frame 15585, that passes (15588 - 15585
= 3 frames of lookahead). But it also passes for frame 15490 (15588 - 15490 =
98 frames of lookahead), even though 15490 is in a disconnected island.

The criterion should be:

```
soft_resume_ok =
    selected_frame is inside a contiguous span S, AND
    S.last_frame >= latest_decoded_frame - small_tolerance
```

Where `small_tolerance` accounts for the ring possibly having advanced
`buffer_head` a few frames past `latest_decoded_frame` without the value
being visible yet. A tolerance of 1-2 frames is enough.

This can be computed from the span data you already build in the paused buffer
browser UI (`paused_buffer_spans` in `red.cpp:1217-1232`). You already have
the spans sorted by frame number. Check whether the selected frame's span has
a `last_frame` that is at or near the `latest_decoded_frame`.

### Q2: Is "camera-only inaccurate seek, no hard stimulus seek" the right intermediate step?

**Yes. This is the right intermediate step.** Here is the reasoning:

The camera re-anchor needs `initiate_camera_seeks()` because the decoder must
start producing frames from a new position. That is unavoidable — only the
full camera seek path resets the decoder's internal `nFrame` and
`buffer_head`.

But stimulus does NOT need a hard seek for a buffered resume. The per-tick
alignment at `red.cpp:1443` will recompute `current_stimulus_frame` from the
new camera frame on the very next tick after cameras settle. The stimulus
decoder, if it is already near the right frame, will converge through normal
decode. The stimulus hard seek is only needed when the stimulus decoder is
very far from the target — and for a short buffer browse (typically tens to
low hundreds of camera frames), the corresponding stimulus frame is probably
close to what stimulus already has buffered.

**Implementation sketch for the camera re-anchor path:**

Use `seekToFrame` but with a modification: skip the stimulus hard seek. The
cleanest incremental approach is to add an option to `seekToFrame` that says
"cameras only, let stimulus follow naturally":

```cpp
// In seekToFrame, after cameras settle in pollSeekState, instead of
// entering SeekState::WaitingStimulus:
if (skip_stimulus_seek) {
    // Just update current_stimulus_frame from alignment
    // and go straight to SeekState::Ready
    context_.seek_progress->state = SeekState::Ready;
}
```

Or, since you might not want to add another parameter to `seekToFrame`, you
could set a flag on `seek_progress` like `skip_stimulus_hard_seek` that
`pollSeekState` checks when it would normally transition to
`WaitingStimulus`.

Either way the key is: the camera re-anchor path should go through the
SeekState machine for cameras (because it needs decoder recreation and settle
tracking), but skip the stimulus seek phase.

**One thing to watch for:** after the camera seek settles, the per-tick
stimulus lookup at `red.cpp:1443` needs to not be frozen. Your paused-browse
freeze guard uses `!ps.play_video && ps.pause_seeked && ps.buffer_browsed_since_pause`.
Since the re-anchor happens after `play_video = true`, the freeze guard will
not fire. Good — stimulus alignment will update naturally.

### Q3: Any cleaner way to express this using active-window vs sparse-browse semantics?

**Yes — and this is where the two-case split maps directly onto the
contiguous playback window design.**

What you are discovering empirically is the same distinction the design doc
makes between:

- **Active playback window** — the contiguous span that the decoder can
  naturally extend from its current position
- **Sparse browse cache** — everything else in the ring buffer

The soft resume / re-anchor split maps to:

| User browsed to... | Maps to... | Action |
|---------------------|------------|--------|
| Frame inside the active playback window (newest contiguous span touching the decode frontier) | Resume within active window | `resumeFromBufferedFrame()` — clock reset, slot hint, no seek |
| Frame in a sparse island outside the active window | Staging a new window | Camera re-anchor seek (no stimulus hard seek), then playback picks up from the newly staged position |

You do not need the full `ActivePlaybackWindow` / `PlaybackStagingWindow`
structs to express this. What you need right now is one predicate:

```cpp
bool isInsideActivePlaybackSpan(int frame, int latest_decoded) const;
```

That returns true if `frame` is in a contiguous buffered span whose highest
frame is at or near `latest_decoded`. Everything else is a sparse browse
frame that requires re-anchoring.

This predicate is the seed of the active playback window concept. Later, when
you add explicit window tracking with history/lookahead, this function becomes
`activeWindow.contains(frame)`. But for now it can be a simple scan of the
ring buffer, or you can pass the already-computed span data from the UI.

## Risk Assessment

| Change | Risk | Reason |
|--------|------|--------|
| `resumeFromBufferedFrame()` | Low | New isolated method, only called from one place |
| Wire it in `applyPlaybackToggle` | Low | Only changes the browsed-resume case |
| Freeze stimulus during paused browse | Low | Cosmetic guard on existing lookup |
| Normal pause/play path | None | Untouched |
| Slider seeks | None | Untouched |
| Full hard seek path | None | Untouched, still available as fallback |
