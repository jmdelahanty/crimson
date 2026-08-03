# Crimson Clipped Playback Sync Debugging

Date anchored: May 2026 clipped Palette smoke debugging.

Lifecycle: **archived debugging record**. Its remaining shared playback-policy
work is tracked in `docs/crimson_playback_remaining_work.md`.

This note records the clipped-video bounding-box synchronization investigation:
what failed, what logs ruled out Palette data issues, what logs exposed the
Crimson playback bug, what changed in Crimson, and what should happen next.

Related docs:

- `docs/palette_clipped_detection_surface_recommendations.md`
- `docs/archive/testing/crimson_gui_smoke_testing_todo.md`
- `docs/archive/playback/crimson_buffered_resume_implementation_notes.md`
- `docs/archive/playback/crimson_contiguous_playback_window_design.md`

## Dataset And Symptom

Primary smoke archive:

```bash
/groups/johnson/johnsonlab/jeremy/palette_smoke/sleepyfish_2026_05_05_17_45_30_cam2010093/zarr/sleepyfish_2026_05_05_17_45_30_cam2010093_analysis.zarr
```

Relevant clipped collection:

```text
sleepyfish_cam2010093_allclips_pynvvc_fixed_20260518_01
```

The user-visible symptom was:

- non-clipped Palette recordings showed bbox overlays correctly
- clipped Palette recordings loaded and drew bboxes, but during playback the
  bbox motion did not appear to move with the fish
- on initial open, the canvas could briefly be black while a bbox was visible
- manually stepping a few frames and then pressing play could reproduce bad
  motion
- clicking the timeline, then playing again, made the motion look correct

That last observation was the strongest behavioral clue. A timeline click
forces Crimson through a seek/rebase path, while initial play/resume can reuse
the already-decoded buffer. So the investigation shifted from Palette mapping
to Crimson playback/buffer state.

## Early Palette-Side Finding

Before the playback issue was isolated, Palette found and repaired a separate
physical chunking problem in bbox arrays:

- logical bbox shape was correct: `(N, 4)`
- some Zarr arrays were physically chunked like `(26664, 2)`
- that is valid Zarr, but it splits bbox columns across chunks
- any Crimson reader that assumed a full row was contiguous in raw chunk memory
  could read bad boxes

Palette changed refined-detect bbox arrays to write chunks as:

```text
(min(N, 65536), 4)
```

for:

- `instances/bbox_img_xyxy`
- `instances/bbox_norm_coords`
- `source_detections/bbox_img_xyxy`
- `source_detections/bbox_norm_coords`

Crimson should still read Zarr arrays by logical indices. The safer chunking
only protects new and repaired archives; older archives can legally contain
split-column chunks.

This chunking issue was real, but it did not explain the later motion mismatch
after Palette repaired the smoke collection.

## Instrumentation Added

All clipped sync instrumentation is opt-in.

Frame trace:

```bash
CRIMSON_CLIPPED_FRAME_TRACE=1
CRIMSON_CLIPPED_FRAME_TRACE_PATH=/tmp/crimson_clipped_frame_trace.jsonl
```

Bound texture dump:

```bash
CRIMSON_CLIPPED_TEXTURE_DUMP_FRAME=1153
CRIMSON_CLIPPED_TEXTURE_DUMP_PATH=/tmp/crimson_bound_texture_1153.png
```

Diagnostic rebase before play:

```bash
CRIMSON_CLIPPED_REBASE_BEFORE_PLAY=1
```

Example run:

```bash
CRIMSON_CLIPPED_FRAME_TRACE=1 \
CRIMSON_CLIPPED_FRAME_TRACE_PATH=/tmp/crimson_clipped_frame_trace.jsonl \
./release/redgui \
  --zarr /groups/johnson/johnsonlab/jeremy/palette_smoke/sleepyfish_2026_05_05_17_45_30_cam2010093/zarr/sleepyfish_2026_05_05_17_45_30_cam2010093_analysis.zarr \
  --swap-interval 0 \
  --frame-cap-fps 120
```

Texture dump example:

```bash
CRIMSON_CLIPPED_TEXTURE_DUMP_FRAME=1153 \
CRIMSON_CLIPPED_TEXTURE_DUMP_PATH=/tmp/crimson_bound_texture_1153.png \
CRIMSON_CLIPPED_FRAME_TRACE=1 \
CRIMSON_CLIPPED_FRAME_TRACE_PATH=/tmp/crimson_clipped_frame_trace.jsonl \
./release/redgui \
  --zarr /groups/johnson/johnsonlab/jeremy/palette_smoke/sleepyfish_2026_05_05_17_45_30_cam2010093/zarr/sleepyfish_2026_05_05_17_45_30_cam2010093_analysis.zarr \
  --swap-interval 0 \
  --frame-cap-fps 120
```

The texture dump writes:

- raw PNG
- `_flip_y` PNG
- JSON metadata beside the PNG
- a `clipped_texture_dump` event in the JSONL trace

## Trace Events

### `clipped_frame`

One compact JSON event per rendered clipped frame. Important groups:

- `ui_parent_timeline`: current/requested parent frame, playback running flag,
  playback speed
- `playback_state`: selected frame, play/pause state, resume state, playback
  clock, and decoded buffer summary
- `resolver`: Palette clipped resolver result for the presented/current frame
- `requested_resolver`: Palette clipped resolver result for the requested frame
- `video`: active clip/video path, decoder local frame, front texture metadata,
  PTS/timebase, presentation source, texture draw metadata
- `bbox`: bbox query parent/local frame, row count, first source bbox, first
  display bbox, source/manual metadata
- `sanity`: direct boolean checks
- `deltas`: frame-index deltas that are easy to aggregate

Key deltas:

```text
decoder_presented_local_minus_clip_local
bbox_query_parent_minus_current_parent
bbox_query_local_minus_clip_local
```

### `clipped_texture_draw`

This event checks that the OpenGL texture actually bound when ImGui/ImPlot draws
the image is the same texture Crimson thinks it queued.

It logs:

- queued texture id
- front texture id
- staging texture id
- front/staging parent and local frame metadata
- OpenGL callback-observed bound texture id
- `bound_matches_queued`

### `clipped_frame_trace_summary`

Periodic summary, roughly every 300 traced frames:

- total frames traced
- mismatch counts for the sanity booleans
- min/max/most-common deltas for decoder local, bbox parent, and bbox local
- texture draw callback missing/bound mismatch counts

### `clipped_playback_state`

Event markers around play toggles and buffer-related interactions. This logs
the same buffer summary used by `clipped_frame`, but at state-transition
boundaries.

### `clipped_rebase_before_play`

Only emitted when `CRIMSON_CLIPPED_REBASE_BEFORE_PLAY=1`. This records the
target frame Crimson chooses before play, then the state immediately after the
diagnostic seek/rebase.

## Code Map

Line numbers are current at the time this note was written.

| Area | Location |
| --- | --- |
| Env flag setup for clipped trace, texture dump, and rebase-before-play | `src/red.cpp:1120`, `src/red.cpp:1162`, `src/red.cpp:1196` |
| Rebase-before-play diagnostic event and seek | `src/red.cpp:1507` |
| Per-frame clipped JSON trace assembly | `src/red.cpp:4561`, `src/red.cpp:4616`, `src/red.cpp:4682`, `src/red.cpp:4935` |
| Trace summary counter and delta updates | `src/red.cpp:4853`, `src/red.cpp:4888` |
| Texture draw event and texture dump write | `src/red.cpp:5792`, `src/red.cpp:5865` |
| GL callback that observes the actual bound texture | `src/gui/camera_view_window.cpp:502` |
| Queueing texture id and frame metadata before `PlotImage` | `src/gui/camera_view_window.cpp:525`, `src/gui/camera_view_window.cpp:662` |
| Texture trace metadata struct | `src/render.h:16` |
| Clipped playback frame-keyed clamp/release fix | `src/red.cpp:6045` |

Field source map:

| Trace field | Source |
| --- | --- |
| `ui_parent_timeline.current_parent_frame_index` | presenter/current frame chosen for the camera view in `src/red.cpp` trace block |
| `ui_parent_timeline.requested_parent_frame_index` | requested parent frame before presenter resolution |
| `playback_state.to_display_frame_number` | `PlaybackState::to_display_frame_number` |
| `playback_state.clock_frame` | `ceil(ps.accumulated_play_time * video_fps)` |
| `playback_state.buffer.target_slot` | search of visible camera `display_buffer[*].frame_number == requested_parent_frame_index` |
| `playback_state.buffer.read_head_frame` | frame number in `display_buffer[ps.read_head % size_of_buffer]` |
| `resolver.*` | `zarr_loader.resolveClippedFrame(current_parent_frame_index)` result |
| `requested_resolver.*` | `zarr_loader.resolveClippedFrame(requested_parent_frame_index)` result |
| `video.decoder_presented_local_frame` | local-frame metadata attached to the presented camera frame |
| `video.front_texture_local_frame_*` | `CameraResources::last_uploaded_local_frame` before/after draw |
| `video.draw_texture.*` | `CameraTextureDrawTrace` queued in camera view and completed by the GL callback |
| `bbox.bbox_query_parent_frame_index` | parent frame used for the Zarr bbox lookup |
| `bbox.bbox_query_clip_local_frame_index` | clipped local frame resolved for the bbox row |
| `bbox.first_bbox_source_image` | first loaded bbox before display transform |
| `bbox.first_bbox_display` | first bbox after Crimson display transform |

## What The Logs Ruled Out

The first important trace result was that the logical frame relationships were
clean. For representative frames, including parent frame `1153`, the trace
showed:

- resolver parent/local frame matched the displayed parent/local frame
- bbox query parent matched the displayed parent frame
- bbox query local matched the resolver local frame
- decoder presented local matched the resolver local frame
- front texture local matched the resolver local frame
- all key deltas were zero

The periodic summaries showed the same pattern:

```text
decoder_presented_local_minus_clip_local: min 0, max 0, most_common 0
bbox_query_parent_minus_current_parent:  min 0, max 0, most_common 0
bbox_query_local_minus_clip_local:       min 0, max 0, most_common 0
texture_draw_bound_mismatches:           0
```

This ruled out:

- Palette clipped frame-index construction as the primary cause
- Crimson querying bboxes for the wrong parent frame
- Crimson querying bboxes for the wrong clip-local frame
- the decoder presenting a different local frame than the resolver expected
- ImGui/ImPlot drawing a different texture id than the one Crimson queued

The frame `1153` texture dump was decisive for the static case:

- Palette and Crimson agreed on the bbox source coordinates
- web UI scaling from the proxy frame to source coordinates matched Crimson's
  source bbox
- the dumped bound texture placed the fish inside the bbox

So the remaining issue was not a static coordinate transform. It had to be in
the playback/buffer path.

## Decisive Playback Evidence

The playback-state logs exposed a different mismatch from the Palette sanity
deltas:

```text
current_parent_frame_index != playback_state.to_display_frame_number
```

In the diagnostic run before the clipped playback fix:

- `1585` `clipped_frame` rows had current/displayed frame different from
  `to_display_frame_number`
- every one of those rows had `playback_state.buffer.target_slot == -1`
- the first clear bad row was around log line `950`

That first bad row looked like:

```text
playback running: true
current_parent_frame_index: 0
requested_parent_frame_index: 2
to_display_frame_number: 2
clock_frame: 2
current_frame_source: presenter_exact_frame_search
read_head: 3
read_head_frame: 3
target_slot: -1
front_parent_frame: 0
oldest_frame: 0
newest_frame: 3
latest_decoded_parent_frame: 3
```

The important interpretation:

- the playback clock requested frame `2`
- the decoded buffer did not contain frame `2`
- `target_slot` was `-1`
- the ring read head had already advanced to frame `3`
- the image presenter fell back to a different available frame
- bbox lookup followed the actually displayed frame, so the Palette/bbox sanity
  checks stayed green

That explains the confusing visual report. It could look like bbox motion and
fish motion disagreed, even while the direct bbox-vs-presented-frame trace said
they matched. The real problem was that Crimson's requested playback frame and
presented buffer frame were not always the same during clipped playback.

The user observation that a timeline click fixed playback fits this exactly:
the click forced Crimson to seek/rebase the decoded buffer around the selected
frame. After that, buffer slots and timeline target were aligned again.

## Root Cause

The shared playback advancement path assumed ring-buffer offset order was frame
order:

```text
next displayed frame = read_head + frame_delta
old consumed frames = slots [read_head, read_head + frame_delta)
```

That can work for simple non-clipped contiguous playback when decode writes and
playback consumes the same monotonically advancing stream.

Clipped playback is more complicated:

- the parent timeline maps through a clipped resolver to clip-local frames
- changing clip/media can reset or rebase decoder state
- prefill/prefetch can make `latest_decoded_parent_frame` run ahead
- after a seek/rebase, ring slot order may not match parent-frame order
- an exact target frame can be absent even when a later frame is present

The old offset-based release path could free the slot that playback was about
to request. Then the presenter had to show whichever frame was actually
available, while the playback clock kept moving.

This was a Crimson buffer-state bug, not a Palette data bug.

## Fix Implemented

The fix is intentionally clipped-specific for now.

In `src/red.cpp`, clipped playback now:

1. Detects clipped playback with:

   ```cpp
   zarr_loaded && zarr_loader.hasClippedCollection()
   ```

2. Finds the exact buffered slot for the requested parent frame by frame number,
   not by ring offset.

3. If the exact frame is absent, clamps the selected target to the best decoded
   frame at or before the requested frame and after the previous displayed
   target.

4. If no forward decoded frame is available, holds the previous target instead
   of advancing the clock into a missing frame.

5. Emits a `clipped_playback_state` event named
   `playback_target_clamped_to_buffer` whenever it clamps.

6. Releases old slots by frame number:

   ```text
   slot.frame_number < frame_to_show
   ```

   instead of releasing slots by ring offset.

7. Sets `ps.read_head` to the exact slot for the selected frame when one exists.

The non-clipped path still uses the older ring-offset release behavior. That
was a risk-control choice: the reported regression was clipped-specific, and
the non-clipped canary already behaved correctly.

## Validation Commands

After building:

```bash
cmake --build build --target redgui -j8
```

Run the clipped smoke with tracing:

```bash
CRIMSON_CLIPPED_FRAME_TRACE=1 \
CRIMSON_CLIPPED_FRAME_TRACE_PATH=/tmp/crimson_clipped_after_fix.jsonl \
./release/redgui \
  --zarr /groups/johnson/johnsonlab/jeremy/palette_smoke/sleepyfish_2026_05_05_17_45_30_cam2010093/zarr/sleepyfish_2026_05_05_17_45_30_cam2010093_analysis.zarr \
  --swap-interval 0 \
  --frame-cap-fps 120
```

Useful trace queries:

```bash
jq -sr 'map(.event // "missing") | group_by(.) | map({event: .[0], count: length})' \
  /tmp/crimson_clipped_after_fix.jsonl
```

```bash
jq -c 'select(.event == "clipped_frame_trace_summary") | {reason, summary}' \
  /tmp/crimson_clipped_after_fix.jsonl | tail -n 5
```

```bash
jq -sr '
  [.[] | select(.event == "clipped_frame")
       | select(.playback_state.play_video == true)
       | select(.ui_parent_timeline.current_parent_frame_index !=
                .playback_state.to_display_frame_number)]
  | {
      mismatch_count: length,
      target_slot_counts:
        (map(.playback_state.buffer.target_slot | tostring)
         | group_by(.)
         | map({target_slot: .[0], count: length})),
      sample:
        (.[0:5] | map({
          current: .ui_parent_timeline.current_parent_frame_index,
          requested: .ui_parent_timeline.requested_parent_frame_index,
          to_display: .playback_state.to_display_frame_number,
          clock: .playback_state.clock_frame,
          source: .playback_state.buffer.current_frame_source,
          read_head: .playback_state.buffer.read_head,
          read_head_frame: .playback_state.buffer.read_head_frame,
          target_slot: .playback_state.buffer.target_slot,
          valid_slots: .playback_state.buffer.valid_slots,
          oldest: .playback_state.buffer.oldest_frame,
          newest: .playback_state.buffer.newest_frame
        }))
    }' \
  /tmp/crimson_clipped_after_fix.jsonl
```

Expected post-fix behavior:

- user-visible clipped playback motion stays coherent
- trace summaries keep all Palette/bbox/decoder deltas at zero
- texture draw bound mismatches stay zero
- `target_slot == -1` should not be a common steady-playback state
- any remaining current-vs-target mismatches should be explainable by seek
  settle, buffer-empty transitions, or shutdown rather than continuous play

Then run without the diagnostic rebase flag:

```bash
CRIMSON_CLIPPED_FRAME_TRACE=1 \
CRIMSON_CLIPPED_FRAME_TRACE_PATH=/tmp/crimson_clipped_after_fix_no_rebase.jsonl \
./release/redgui \
  --zarr /groups/johnson/johnsonlab/jeremy/palette_smoke/sleepyfish_2026_05_05_17_45_30_cam2010093/zarr/sleepyfish_2026_05_05_17_45_30_cam2010093_analysis.zarr \
  --swap-interval 0 \
  --frame-cap-fps 120
```

The fix should not require `CRIMSON_CLIPPED_REBASE_BEFORE_PLAY=1` for normal
playback.

## Should Rebase Before Play Become Default?

Not as a blanket default yet.

`CRIMSON_CLIPPED_REBASE_BEFORE_PLAY=1` was useful because it proved that
forcing the decoded buffer to re-anchor before play made the visual problem go
away. It is best treated as a diagnostic and temporary safety valve, not the
primary fix.

Reasons not to make it unconditional immediately:

- it can hide playback-window bugs by always taking a seek/rebase path
- it may add latency to play/resume
- it may recreate decoder/seek behavior even when the frame is already in a
  good contiguous playback window
- it does not solve the deeper shared abstraction issue

A narrower default could make sense after more validation:

- for clipped playback only
- only when resuming from an explicit paused selection or timeline seek
- only when the target frame is not in the active contiguous playback window
- implemented as staging/rebase to a validated window, not as an unconditional
  hard seek on every play toggle

So the recommendation is:

1. Keep the env flag for debug.
2. Do not make unconditional rebase-before-play the default.
3. Make frame-keyed playback selection the durable behavior.
4. Add a narrow staged-rebase default only when Crimson cannot prove the target
   is present in a contiguous playable window.

## Should We Start Unification?

Yes, but the next slice should reduce the monolith rather than make
`src/red.cpp` bigger.

The clipped fix creates a useful boundary:

- non-clipped playback still has the old ring-offset advancement
- clipped playback now has frame-keyed target selection and frame-keyed release

That difference should become a shared playback helper rather than staying
inline in the application loop.

Recommended next slice:

1. Extract a small frame-keyed buffer helper, for example:

   ```text
   src/playback/frame_buffer_index.h
   src/playback/frame_buffer_index.cpp
   ```

2. Move these operations out of `src/red.cpp`:

   - find exact buffered frame slot
   - find best buffered frame at or before target
   - compute newest contiguous span
   - release slots older than a frame number
   - set read head from exact frame slot

3. Add focused tests for sparse slot layouts:

   - exact target present
   - target missing but later frame present
   - target missing but earlier forward frame present
   - ring slot order differs from frame order
   - release by frame number does not free the target slot

4. Keep clipped playback using the helper first.

5. After clipped stays stable, migrate non-clipped advancement to the same
   frame-keyed helper behind tests.

6. Then implement the larger active/staging playback window design from
   `docs/archive/playback/crimson_contiguous_playback_window_design.md`.

The long-term target is one playback surface for clipped and non-clipped media:

- selected parent frame is the timeline source of truth
- exact frame-keyed buffer lookup decides what can be shown
- an active contiguous playback window decides when play can advance
- seek/rebase/staging is used only when the selected target is outside that
  active window
- bbox/stimulus overlays follow the presented parent frame, not stale clock or
  ring slot assumptions

That direction hardens clipped playback and also supports the broader
monolith-refactor goal: move playback mechanics into testable modules instead
of adding more policy to `red.cpp`.
