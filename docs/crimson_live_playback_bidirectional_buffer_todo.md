# Crimson Live Playback Bi-Directional Buffer TODO

Date anchored: 2026-02-10.

## Problem Summary

Current live playback uses a forward-consumption ring buffer. As playback advances, old slots are immediately released, so users cannot reliably scrub or step a short distance backward without triggering a seek/decode refill.

Goal: support a stable asymmetric window around the current playback frame:

- history: 32 frames behind
- lookahead: 64 frames ahead

Target window at frame `T`: `[T - 32, T + 64]`.

## Desired User Behavior

During live playback:

1. Display remains smooth at current play speed.
2. Small backward steps (up to 32 frames) use in-memory data, no hard seek.
3. Small forward steps (up to 64 frames) usually use already-decoded data.
4. Buffer occupancy remains bounded and predictable.

## Current Architecture (Relevant)

Key files:

- `src/decoder.cpp`
- `src/red.cpp`
- `src/decoder.h`
- `src/render.h`

Current behavior:

- Decoder thread writes sequential frames into a ring via `buffer_head`.
- Playback loop advances `ps.read_head` and marks intermediate slots writable.
- Slot lifecycle is `available_to_write` + `frame_number`; no explicit slot state machine.
- Paused mode already has helpers that can choose nearest matching frame by frame number.

Main gap:

- Live playback release policy is forward-only and does not preserve a history/lookahead window.

## Scope

In scope:

- Camera playback path.
- Live playback window retention policy for decoded frames.

Out of scope (first pass):

- Stimulus path parity.
- Multi-resolution caching.
- Cross-process persistent cache.

## Proposed Design

### 1) Introduce Window Policy

Add explicit runtime policy (initial defaults):

- `history_frames = 32`
- `lookahead_frames = 64`

Effective keep range at display target `T`:

- `keep_min = max(0, T - history_frames)`
- `keep_max = T + lookahead_frames`

### 2) Replace Forward-Only Release with Windowed Eviction

During live playback, do not release slots purely by `frame_delta`.

Instead, evict slots only when:

- `slot.frame_number < keep_min` (too old), or
- `slot.frame_number > keep_max` (too far ahead, usually after discontinuities), or
- space is required and slot is farthest outside keep range.

### 3) Make Decoder Production Window-Aware

Decoder should continue sequential decode, but with high/low watermarks:

- decode-continue threshold: newest buffered frame `< keep_max`
- decode-throttle threshold: newest buffered frame `>= keep_max + margin`
- decode-resume threshold: newest buffered frame `<= keep_max - hysteresis`

This avoids overfilling while keeping lookahead available.

### 4) Add Slot State Safety

Current metadata is minimal (`available_to_write`, `frame_number`) and race-prone under tighter policies.

Introduce explicit slot states:

- `FREE`
- `WRITING`
- `READY`

Minimum requirement:

- writer sets `WRITING` before copy and `READY` after metadata publish
- reader only presents `READY` slots
- eviction only transitions `READY -> FREE`

### 5) Frame Lookup Index

Add per-camera `frame_number -> slot_index` index for O(1)-ish lookup.

Benefits:

- fast exact-frame access for display
- fast keep-range membership checks
- robust nearest-frame fallback without scanning every slot each frame

### 6) Playback Cursor Behavior

`ps.to_display_frame_number` remains the authoritative target.

Presentation priority:

1. exact frame in cache
2. nearest lower frame inside keep range
3. nearest absolute frame
4. clear frame fallback (never display stale random slot)

### 7) Seek Integration

On seek to `T`:

- decode start target should be `max(0, T - history_frames)` when practical
- seek complete only after `T` is available for display (current seek stabilization remains relevant)
- post-seek warmup can continue until `keep_max` coverage target is reached

## Memory Budget Notes

For 4512x4512 RGBA:

- one frame: ~77.66 MiB
- 64 frames: ~4.85 GiB
- 96 frames (`32 + 64`): ~7.28 GiB

Implications:

- single camera can support 96 slots on high-memory GPUs
- multi-camera requires dynamic policy scaling (reduce history/lookahead per camera)

## Rollout Plan

### Phase 0: Instrumentation

Add debug counters/log lines:

- keep range per frame (`keep_min`, `target`, `keep_max`)
- oldest/newest buffered frame
- slot count in/out of keep range
- cache hit type (exact/lower/nearest/none)

### Phase 1: Windowed Eviction in Live Playback

Replace current forward release loop with keep-range eviction.
Keep decoder write path mostly unchanged initially.

### Phase 2: Decoder Throttle by Keep Range

Switch decode request gating to use window watermarks rather than simple play/pause heuristics.

### Phase 3: Slot State + Index Hardening

Add explicit state machine and frame index map for robust high-rate operation.

### Phase 4: Seek Warmup Improvements

Optionally decode from `T - history_frames` after seeks to fill immediate backward context.

## Validation / Acceptance Criteria

1. During playback at 60 fps, backward step up to 32 frames does not trigger hard seek in common case.
2. Forward step up to 64 frames is typically cache-hit if decode is healthy.
3. No stale-frame artifacts from invalid slot reuse.
4. No increased flashing/black-frame incidence relative to current baseline.
5. Controlled memory behavior with explicit policy and logs.

## Open Questions

1. Should history/lookahead be fixed, or adaptive by available VRAM and number of visible cameras?
2. Should stimulus path share the same window manager abstraction?
3. Is per-slot locking needed, or are atomics + publish ordering sufficient for current threading model?
4. Do we keep decoder recreation-on-seek default while this work lands? (recommended: yes)

## Suggested Config Flags (Future)

- `CRIMSON_PLAYBACK_HISTORY_FRAMES=32`
- `CRIMSON_PLAYBACK_LOOKAHEAD_FRAMES=64`
- `CRIMSON_PLAYBACK_WINDOW_DEBUG=1`

These should remain optional until the new policy is fully validated.
