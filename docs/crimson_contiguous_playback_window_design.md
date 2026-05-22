# Crimson Contiguous Playback Window Design

Date anchored: 2026-04-07.

## Status Update

As of 2026-05-22, parts of this design have landed as incremental safeguards,
but the full active/staging playback window abstraction has not been extracted.

Implemented pieces:

- `PlaybackSessionController::resumeFromBufferedFrame()` supports soft resume
  when the selected frame is inside the newest contiguous buffered span.
- Resume from an older sparse island now uses a camera re-anchor path that can
  skip the hard stimulus seek.
- Paused-browse and seek-settle stimulus updates are guarded so stimulus does
  not chase transient browse/settle frames.
- Clipped collection playback has a frame-keyed target clamp and release path
  so it does not advance into missing ring slots after clipped seek/rebase.

Still pending:

- an explicit `ActivePlaybackWindow` / `PlaybackStagingWindow` runtime model
- a shared frame-keyed buffer helper used by both clipped and non-clipped
  playback
- windowed history/lookahead retention and decoder watermarks
- formal sparse-browse cache semantics

## Problem Summary

Camera playback still mostly uses a forward-consumption ring buffer. The paused
buffer browser makes the real occupancy visible: it often contains sparse
islands of decoded frames rather than one contiguous playable neighborhood.

That makes two things clear:

- sparse buffered frame browsing is useful for inspection
- sparse occupancy is not the same thing as a resumable playback window

Recent buffered-resume experiments also showed that treating a browsed slot as
playback state is unstable:

- if playback simply restarts from the shown frame without re-priming decode,
  it can stall at the old decoded tail
- if playback resumes through the generic hard-seek path, camera continuity may
  improve but stimulus smoothness can regress

The right next abstraction is not "another ring" or "simple double buffering."
It is a separation between:

- the active contiguous playback window
- a staging window used to build a new contiguous window after seeks/resume
- an optional sparse browse cache for paused inspection only

## Goal

Make playback behave like a playout system, not like an exposed decoder ring.

Desired user behavior:

1. During normal playback, the app maintains a contiguous playable span around
   the playhead.
2. Small backward and forward steps within that span do not trigger a hard
   seek.
3. Paused browsing of sparse buffered frames remains immediate, but it does not
   drag stimulus or redefine the active playback window.
4. Seek and resume transitions build a new contiguous window before switching
   playout state.

## What Online Players Do

Systems like YouTube and Netflix do not treat arbitrary cached frames as the
playback source of truth.

They instead maintain:

- an active playout queue that is contiguous and ready to present
- a staging/fill path that prepares the next queue after a seek or rebuffer
- separate browse aids such as thumbnails or scrub previews

Important consequences:

- scrubbing previews are not the same thing as resumable decode state
- a seek does not become "smooth" until enough contiguous data exists near the
  new target
- the player only switches to the new target after its staging path is ready

Crimson should adopt the same conceptual split even though it is a local
decoder, not an HTTP segment player.

## Current Crimson Model

Today Crimson mostly has:

- one forward-consumption decode ring
- paused browsing over currently occupied slots
- playback driven by a target camera frame and whatever slots happen to exist

This is good enough for smooth steady playback, but it leaves a gap between:

- sparse inspectability
- contiguous resumability

## Proposed Model

### 1) Active Playback Window

Introduce an explicit runtime concept:

```cpp
struct ActivePlaybackWindow {
    bool valid = false;
    int playhead_frame = -1;
    int contiguous_start = -1;
    int contiguous_end = -1;
    int history_frames = 32;
    int lookahead_frames = 64;
};
```

Meaning:

- `playhead_frame` is the authoritative current playback target
- `[contiguous_start, contiguous_end]` is the playable camera-frame span that
  can be advanced without a hard seek
- history/lookahead remain policy knobs, not promises

This window replaces the informal idea that "whatever happens to be in the ring
must be good enough to resume from."

### 2) Staging Window

Introduce a separate seek/resume preparation state:

```cpp
struct PlaybackStagingWindow {
    bool active = false;
    int target_frame = -1;
    int desired_start = -1;
    int desired_end = -1;
    int ready_start = -1;
    int ready_end = -1;
};
```

Meaning:

- after a seek or buffered resume, decode does not immediately redefine the
  active playout window
- it first fills a contiguous neighborhood around the target
- only after readiness criteria are met does Crimson switch the active window

This is the important "double buffer" idea that is actually useful:

- one active playback window
- one staging window

Just adding a second generic ring would not solve the real problem unless one
ring is explicitly the active playout window and the other is explicitly a
staging target.

### 3) Sparse Browse Cache

Keep sparse paused browsing as a distinct concept:

```cpp
struct SparseBrowseCacheView {
    int visible_camera_index = -1;
    int oldest_buffered_frame = -1;
    int newest_buffered_frame = -1;
    int span_count = 0;
};
```

This cache is:

- useful for paused inspection
- useful for choosing a candidate frame to resume from
- not authoritative playback state

Important rule:

- browsing sparse buffered frames must not redefine the active playback window
- browsing sparse buffered frames must not directly drive stimulus playback

## Stimulus Policy

This needs to be explicit.

Stimulus should follow the active playback window, not individual paused browse
events.

Recommended policy:

- while paused and browsing buffered frame islands, keep stimulus unchanged
- when playback resumes or a real seek completes, stimulus re-synchronizes from
  the canonical camera-frame target
- if staging is still active, stimulus should wait for the active playback
  window switch rather than chasing sparse intermediate frame picks

This matches user expectation better:

- paused browsing is visual inspection
- stimulus remains tied to actual playback state

## Window Semantics

### Active playback

During steady playback:

- decoder keeps the active window filled ahead of the playhead
- playback only frees frames that fall outside the retained history window
- presenter prefers exact or nearby frames inside the active window

### Paused sparse browsing

While paused:

- the buffer browser may show sparse spans
- selecting one of those frames only creates a candidate resume target
- it does not mean the app has a new contiguous window there

### Resume from paused browse

When the user presses play from a sparse browsed frame:

1. create a staging window centered on that camera frame
2. decode/fill until the target frame and minimal forward continuity exist
3. switch the active playback window to the staged region
4. then let stimulus follow from the canonical camera-frame target

The key change is:

- the app does not try to "continue from the shown slot"
- it stages a real playout neighborhood around the selected frame

## Readiness Criteria

Crimson does not need a perfect full window before playback can continue.

Suggested first-pass readiness:

- target frame is decoded and present
- at least `min_forward_resume_frames` are contiguous after target
- presenter has a valid frame to show without clearing

Example initial thresholds:

- `min_forward_resume_frames = 8`
- full target window still aims for history 32 / lookahead 64

That keeps resume responsive while still insisting on some real continuity.

## Decoder / Cache Implications

### Slot metadata is not enough by itself

`available_to_write` plus `frame_number` is adequate for the current simple
ring, but not ideal once Crimson distinguishes:

- active playout ownership
- staging fill ownership
- sparse browse occupancy

Longer-term, this likely wants:

- explicit slot states (`FREE`, `WRITING`, `READY`, maybe `PINNED`)
- a frame-number index
- window ownership metadata or pin counts

### Eviction policy should follow windows

Frames should no longer be released solely because playback stepped past them.

Instead:

- retain frames inside the active window history/lookahead policy
- allow staging to reserve or protect frames until switch-over
- treat sparse browse leftovers as lowest-priority cache occupants

## Multi-Camera Semantics

The canonical identity remains camera frame, not slot index.

For multi-camera playback:

- the active playback window is defined in shared camera-frame terms
- each camera may have different exact slot coverage underneath
- switch-over readiness should require enough visible cameras to satisfy the
  chosen presentation policy

This keeps the design compatible with one-camera and several-camera views.

## Implementation Phases

### Phase 1: Window state and UI visibility

- add `ActivePlaybackWindow` and `PlaybackStagingWindow`
- expose current contiguous playable span separately from sparse buffer spans
- keep behavior mostly unchanged at first

### Phase 2: Window-aware eviction

- stop freeing frames purely by forward traversal
- retain history/lookahead for the active playback window

### Phase 3: Seek/resume staging

- on hard seek or buffered resume, fill a staging window near the target
- only switch once readiness criteria are met

### Phase 4: Stimulus window-follow policy

- keep stimulus tied to the active playback window
- do not let paused sparse browsing drive stimulus state
- resume stimulus only after active window switch or explicit hard seek settle

### Phase 5: Optional sparse browse cache formalization

- if useful, formalize sparse browse frames as a low-priority cache view
- keep it separate from playback guarantees

## Acceptance Criteria

1. During normal playback, Crimson can report a real contiguous playable span.
2. Small backward/forward steps inside that span avoid hard seek in common
   cases.
3. Browsing sparse paused frames does not move stimulus playback.
4. Resuming from a browsed frame stages a contiguous region instead of trying
   to continue from arbitrary sparse slots.
5. Resume no longer stalls at the old decoded tail.
6. Resume avoids the earlier heavy generic hard-seek behavior that made
   stimulus feel rough.

## Recommendation

Do not solve this by:

- making sparse paused-buffer occupancy look more contiguous than it is
- treating a selected slot as a durable playback checkpoint
- adding a second generic ring without window semantics

Do solve it by:

- formalizing an active contiguous playback window
- adding a real staging window for seek/resume
- treating sparse paused browsing as inspection state only
- keeping stimulus aligned to actual playback state, not paused browse clicks

That is the direction most consistent with both the current playback behavior
and the user expectation that stimulus should not follow browsing of individual
buffered frames.
