# Crimson Decode/Seek Artifact Investigation TODO

Date anchored: 2026-02-09.

Lifecycle: **archived investigation record**. Its unclosed NVIDIA runtime gate
is tracked in `docs/crimson_playback_remaining_work.md`.

## Problem Summary

After seeks (especially review-frame jumps), visible frame artifacts persist temporarily:

- fish/object appears at an old location
- image appears to "fade in" to correct pixels over subsequent frames
- artifacts can happen both paused and during playback

## Current Repro (Observed)

1. Load archive + affiliated video in `redgui`.
2. Jump frames via:
   - review navigation buttons, or
   - scrub/seek.
3. Observe stale visual content in current frame that resolves over time.

## Constraints / Guardrails

- Treat `src/FFmpegDemuxer.cpp` and `src/FFmpegDemuxer.h` as vendor-adjacent code; avoid edits unless explicitly approved.
- Focus investigation/fixes in Crimson app layers:
  - `src/red.cpp`
  - `src/decoder.cpp`

## What We Tried So Far

### A. Slot-validity and seek stabilization in `src/red.cpp`

- Added review-frame index navigation with seek integration.
- Added paused seek stabilization and slot selection helpers:
  - prefer exact target frame when available
  - avoid rendering writable/invalid slots
- Added logic to freeze decode requests after paused settle.

Status: **insufficient**. Artifacts still observed.

### B. Buffer metadata reset on seek in `src/decoder.cpp`

- On seek, invalidate all slots:
  - `available_to_write = true`
  - `frame_number = -1`

Status: **insufficient**. Artifacts still observed.

### C. Hard memory clear on seek in `src/decoder.cpp`

- On seek, clear slot memory to zero:
  - CPU path: `memset`
  - GPU path: `cudaMemset`

Status: **in progress to validate**.

### D. Render-side hard clear fallback in `src/red.cpp`

- If no valid slot is available for display, clear PBO (`cudaMemset`) instead of reusing stale texture content.

Status: **in progress to validate**.

### E. Decode-buffer dump instrumentation in `src/red.cpp`

Added debug tooling to inspect actual buffer contents:

- `Dump Decode Buffers`
- `Random Seek + Dump`

Output:

- default: `/tmp/crimson_buffer_dumps`
- override via `CRIMSON_BUFFER_DUMP_DIR`
- per camera:
  - slot snapshot (`*_slots.txt`: slot index, frame number, writable flag, frame pointer present)
  - frame map text file (`*_frames.txt`: dump index, source frame number, slot index, png name)
  - per-frame image dump (`*_f<frame>_slot<slot>.png`) for direct inspection
  - optional video dump (`.avi` fallback `.mp4`)

Status: **implemented and hardened, pending validation**.

### F. Dump robustness update in `src/red.cpp`

To improve dump reliability and diagnostics:

- Dump now pauses playback while capturing.
- Decode requests are temporarily disabled and capture waits briefly for in-flight writes to settle.
- Frame copy validates slot metadata before and after copy to avoid mid-write captures.
- Status line now reports `written frames (candidate frames)` to explain low capture counts.
- Added unique ImGui IDs in "Frames in the buffer" list to avoid duplicate-ID debug UI warnings.

Status: **implemented, needs runtime validation on artifact repro case**.

### G. Decoder recreation toggle experiment in `src/decoder.cpp` (2026-02-10)

Experiment:

- Modified seek handling to avoid rebuilding `NvDecoder` on every seek.
- Kept demux flush + `CUVID_PKT_DISCONTINUITY` decode reset in place.

Observed result:

- stale/fading artifact returned when seeking (regression),
- consistent with residual decoder-internal state surviving seek when decoder is reused.

Current policy:

- default path is restored to **recreate decoder on seek** (known-good behavior),
- an experiment toggle remains:
  - `CRIMSON_RECREATE_DECODER_ON_SEEK=0` disables recreation,
  - unset (or any value except `0`) keeps recreation enabled.

Implication:

- decoder recreation is currently a required guardrail for artifact-free seeking in this pipeline until we find an equivalent flush/reset sequence that is proven safe.

## Notable Issue Encountered and Fixed

Encountered assert:

`NvDecoder.h:107: int NvDecoder::GetWidth(): Assertion 'm_nWidth' failed`

Cause:

- seek-reset path touched `NvDecoder::GetWidth()` before decoder init.

Fix:

- changed seek reset to use demuxer dimensions only.

## Most Likely Remaining Root Causes

1. Seek completion signal timing
- `seek_done` may be observed before a fully valid post-seek frame is safe to present.

2. Cross-thread visibility/race in slot lifecycle
- slot metadata and pixel payload may not be in a strictly synchronized "ready" state at render sampling time.

3. Decode reference-state behavior around keyframe seeks
- even with slot clears, first decoded outputs after seek might require stricter gating before presentation.

4. Decoder-internal surface/reference persistence when reusing `NvDecoder`
- discontinuity+flush alone may not fully reset internal state for this stream/cadence.

## Next Investigation Steps (Priority Order)

1. Re-run dump workflow using new PNG + slot outputs.
- Run `Random Seek + Dump` multiple times.
- Inspect `*_slots.txt`, `*_frames.txt`, and PNG frames.
- Determine whether artifact exists:
  - in dumped PNG payloads (decoder-side issue), or
  - only on live display path (presentation/race issue).

2. Tighten post-seek readiness gate in app layer (if PNG dumps are clean).
- Present frame only when target frame slot is explicitly valid for each visible camera.
- If not ready, keep showing cleared buffer or loading state; never nearest stale frame during settle.

3. Add optional debug counters/logs (behind flag).
- per-frame selected slot index / frame number / validity
- seek target frame vs displayed frame
- count of invalid-slot display fallbacks

4. If PNG dump confirms decoder payload corruption after seek:
- evaluate stricter decoder-side flush+prime sequence in `src/decoder.cpp` before releasing seek completion.
- keep vendor demuxer unchanged unless evidence shows wrapper logic alone cannot resolve it.

## Acceptance Criteria

Investigation complete when all are true:

1. No stale-object/fading artifact after repeated random seeks.
2. Behavior stable in both paused and playback modes.
3. Buffer dumps show frame payload consistency around seek boundaries.
4. No regressions in normal stepping/scrubbing performance.

## Debug Session Checklist

Use this checklist to generate reproducible evidence for one session.

1. Build and launch `redgui`.
- Build:
  - `cmake --build build --target redgui -j$(nproc)`
- Optional output root for dumps:
  - `export CRIMSON_BUFFER_DUMP_DIR=/tmp/crimson_buffer_dumps`
- Launch:
  - `./release/redgui`

2. Load data and confirm video is visible.
- Load the target `.zarr` archive.
- Confirm affiliated video opens and frames are rendering.

3. Trigger artifact and capture baseline.
- Perform several seeks (review jump, slider seek, or frame jumps).
- Note whether artifact appears in paused mode, playback mode, or both.

4. Use built-in dump tools in **Frame Debug** -> **Decode Debug**.
- Click `Dump Decode Buffers` to capture current buffer contents.
- Click `Random Seek + Dump` 3-5 times.
- Record the printed dump path from the status line.

5. Inspect outputs.
- Dump directory layout (per run):
  - `<dump_dir>/<camera>_slots.txt`
  - `<dump_dir>/<camera>_frames.txt`
  - `<dump_dir>/<camera>_f<frame>_slot<slot>.png`
  - optional `<dump_dir>/<camera>.avi` (or `.mp4`)
- Inspect slot snapshot and frame mapping to confirm expected slot occupancy.
- Use PNG frames as ground truth if video container output appears corrupted.

6. Classify issue source.
- If artifact appears in dumped PNG frames:
  - decoder payload issue likely.
- If PNGs look clean but live display artifacts remain:
  - presentation/timing/race issue likely.

7. Archive evidence for handoff.
- Save:
  - dump directory path(s)
  - short note: mode used (paused/playback), seek operation used, whether artifact reproduced.

## Notes on Dump Counts

- Low frame counts (for example, only 1-3 frames dumped) can be valid if few slots are occupied at capture time.
- Candidate count in status indicates how many occupied slots were available before copy.
- `*_slots.txt` is the primary record to explain why a run captured few frames.
