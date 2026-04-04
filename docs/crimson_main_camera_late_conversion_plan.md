# Crimson Main-Camera Late-Conversion Plan

Date anchored: 2026-04-04.

## Why This Exists

Playback profiling on the Windows RTX A1000 laptop has now ruled out several
smaller hypotheses:

- stimulus playback is no longer the primary limiter after switching the
  stimulus path to software decode
- PCIe upload is no longer the primary limiter in the main-camera `GPU Buffer`
  path
- steady decoder write/copy cost is no longer the primary limiter in the
  direct-slot `GPU Buffer` path
- CPU-side camera overlay construction is not the main limiter

What remains, consistently, is:

- `gl_draw_ms` near the full `60 fps` frame budget
- persistent positive `camera_decode_gap_frames`

The current evidence says Crimson is still paying too much for the
`4512x4512` main camera image because it expands frames to full `RGBA` too
early and keeps that representation around for playback/display.

This doc is the follow-on design note after:

- [docs/crimson_playback_preview_scale_plan.md](./crimson_playback_preview_scale_plan.md)

And it now feeds into the next render-focused plan:

- [docs/crimson_main_camera_zoom_aware_render_plan.md](./crimson_main_camera_zoom_aware_render_plan.md)

## Key Evidence

### FFmpeg Camera Benchmark

For the representative main camera file:

- codec: `HEVC Main`
- size: `4512x4512`
- fps: `60`

Measured on the Windows laptop:

- `software-native`: about `59.79 fps`
- `software-rgba`: about `52.44 fps`
- `cuda-download-nv12`: about `71.37 fps`
- `cuda-download-rgba`: about `60.23 fps`

Interpretation:

- hardware decode has headroom when the camera frame stays in `NV12`
- that headroom mostly disappears once the path forces full-frame `RGBA`
- software main-camera decode is not the right next move

### Crimson Perf Findings

Recent main-camera `GPU Buffer` captures showed:

- `camera_upload_ms` negligible
- `camera_pbo_copy_ms = 0`
- `camera_decode_write_ms` negligible
- `camera_decode_convert_ms` negligible in steady-state timing
- but `gl_draw_ms p90` still near the full frame budget

That means the next high-impact move is not more upload tuning. It is to stop
materializing the main camera as full `RGBA` earlier than necessary.

## Current Status

As of 2026-04-04, the first late-conversion slice has now landed:

- main-camera `GPU Buffer` slots store compact `NV12`
- only the selected displayed frame is converted for presentation
- the perf log now records `camera_display_convert_ms`

That slice succeeded in its immediate goal:

- upload became negligible
- steady decoder write/materialization became negligible

But it also clarified the next bottleneck:

- `gl_draw_ms` remains near the frame budget on the Windows RTX A1000 laptop

So the late-conversion plan remains the right architectural direction, but its
first slice is no longer the next optimization frontier. The next frontier is a
zoom-aware playback render path for weaker GPUs.

## Problem Summary

Current main-camera playback still does too much work too early:

1. decode on GPU
2. convert decoded frame to full `RGBA`
3. buffer/store the frame in that expanded form
4. draw/sample from a full-resolution `RGBA` texture during playback

For a `4512x4512` stream, that is expensive in:

- memory footprint
- memory bandwidth
- texture sampling/render cost

The user requirement we must preserve is important:

- zoom during playback remains available
- overlays remain visible and aligned
- paused inspection remains full fidelity

So the target is not "simplify the UI by removing functionality." The target is
"keep full functionality while moving expensive conversion later."

## Primary Goal

Keep the main camera in a compact GPU format for buffered playback, and convert
only the currently displayed frame for rendering.

In short:

- store buffered main-camera frames as `NV12` (or equivalent compact GPU format)
- perform color conversion only for the frame actually being shown
- later, make that displayed-frame conversion ROI-aware when zoomed in

## Non-Negotiable Guardrails

- Keep zoom/pan behavior during playback.
- Keep bounding boxes and overlays visible and aligned.
- Keep paused inspection full fidelity.
- Keep the current Windows low-VRAM laptop usable.
- Do not regress stimulus playback improvements.
- Preserve a fallback path while the late-conversion path is being validated.

## Current Architecture (Relevant)

Key files:

- [src/decoder.cpp](../src/decoder.cpp)
- [src/render.h](../src/render.h)
- [src/red.cpp](../src/red.cpp)
- [src/gx_helper.h](../src/gx_helper.h)

Current high-level behavior:

- `decoder.cpp` uses `NvDecoder`
- decoded camera frames are converted with `Nv12ToColor32<RGBA32>(...)`
- buffered slots are stored in `RGBA`
- `red.cpp` uploads or presents those `RGBA` buffers for drawing
- the main camera is ultimately drawn through `ImPlot::PlotImage`

Main issue:

- conversion to `RGBA` happens before the app has chosen which frame will
  actually be shown

## Target Shape

The intended end state for the main camera is:

1. decode camera frames with NVDEC as today
2. keep buffered playback slots in `NV12` on GPU
3. when the UI chooses the displayed frame:
   - convert only that frame to a displayable color texture
4. when zoomed in:
   - convert only the visible ROI where possible

This should reduce the cost of:

- per-frame buffered representation
- unnecessary full-frame expansion
- rendering a huge `RGBA` source texture during playback

## Scope

In scope:

- main camera playback path
- main camera buffered representation
- main camera render path
- preserving playback zoom functionality

Out of scope for the first pass:

- stimulus path changes
- software main-camera decode backend
- cross-process cache
- packaging/distribution changes

## Proposed Design

### 1) Introduce A Main-Camera Compact Slot Representation

For the main camera `GPU Buffer` path, add a compact slot type that stores:

- the decoded frame in `NV12` (or a similar compact GPU-native format)
- frame number / slot metadata as today

Do not force immediate `RGBA` expansion for every buffered slot.

### 2) Separate "Buffered Frame Format" From "Displayed Frame Format"

Today those concepts are mostly collapsed together.

Make them distinct:

- buffered playback slots: compact decode-native format
- displayed frame: temporary display texture in `RGB/RGBA`

This is the core architectural shift.

### 3) Convert Only The Displayed Frame

When `red.cpp` chooses the frame to show:

- locate the selected buffered slot
- convert that slot to the display texture
- draw from the display texture

This means the app pays conversion cost for:

- one displayed frame

instead of:

- every buffered playback frame

### 4) Preserve Full Zoom Behavior

Do not replace the camera view with a non-interactive path.

Keep:

- ImPlot-based zoom/pan behavior
- overlay coordinate system
- playback interaction model

The first pass can still draw into the existing display texture, as long as the
display texture is populated from the chosen buffered slot late in the path.

### 5) Add ROI-Aware Conversion Later

Once late conversion is in place, add a second optimization:

- when zoomed out: convert the whole displayed frame at reduced detail if useful
- when zoomed in: convert only the visible ROI at full resolution

This should be a follow-on phase, not the first implementation.

Why:

- late conversion is the higher-impact architectural win
- ROI logic is valuable, but easier to reason about once buffered vs displayed
  frame formats are already separated

## Rollout Plan

### Phase 0: Keep The Existing Instrumentation

Keep the current perf log fields that already proved useful:

- `camera_decode_convert_ms`
- `camera_decode_wait_ms`
- `camera_decode_write_ms`
- `camera_decode_pipeline_ms`
- `camera_upload_ms`
- `gl_draw_ms`
- `camera_decode_gap_frames`

These are the baseline metrics for validating the architectural shift.

### Phase 1: Add A Compact Main-Camera Slot Type

- Add a GPU-buffered slot format for compact decoded frames.
- Keep the existing `RGBA` path available as fallback.
- Land this first without removing the old representation.

Acceptance:

- app still builds and runs
- slot lifecycle remains correct
- no playback regressions yet

### Phase 2: Populate A Display Texture Only For The Chosen Frame

- When the playback loop selects `ps.to_display_frame_number`, resolve the slot
  as today.
- Convert only that slot into the display texture.
- Keep the existing overlays and plot interaction on top.

Acceptance:

- app renders correctly
- overlays remain aligned
- zoom during playback still works

### Phase 3: Validate On The Windows Laptop

Use:

- main camera `GPU Buffer`
- small buffer size, initially `4-6`
- stimulus software decode + GPU buffer

Look for:

- reduced `gl_draw_ms`
- stable or improved `camera_decode_gap_frames`
- playback speed closer to `1.0x`

### Phase 4: Add ROI-Aware Conversion

Only after Phase 2 is validated:

- detect whether the current plot view is zoomed in
- convert only the visible ROI at full resolution
- retain a zoomed-out whole-frame path for broad navigation

This is the logical place for ROI work.

## Risks / Complications

### Decoder Surface Lifetime

If the buffered slot representation becomes closer to the decode-native output,
we need a clear answer for:

- who owns the lifetime of those surfaces
- whether they must be copied into Crimson-owned GPU memory

We should not rely on decode surfaces remaining valid beyond their intended
lifetime.

### YOLO / Secondary Consumers

The current YOLO path and any other consumers that assume `RGBA` camera buffers
may need a compatibility path.

The first pass should keep YOLO-compatible behavior explicit rather than
implicitly breaking it.

### Paused vs Playing Semantics

Paused inspection is allowed to be more expensive than live playback.

That suggests:

- late conversion path during playback
- optional full-fidelity fallback while paused

### Complexity

This is a real architectural change, not a cosmetic optimization.

That is acceptable because the evidence now supports it more strongly than
further tuning around the current `RGBA`-everywhere design.

## Open Questions

1. What exact compact format should Crimson own in buffered slots:
   - native `NV12`
   - split planes
   - another GPU-friendly intermediate?
2. Does the first displayed-frame conversion use:
   - CUDA conversion into the display texture path
   - shader-based YUV sampling/conversion
   - some hybrid?
3. Should paused mode stay on the same path, or be allowed to fall back to a
   more expensive full-fidelity path?
4. Do we want ROI-aware conversion immediately after late conversion, or only
   after we validate the whole-frame late-conversion win?

## Acceptance Criteria

The first architectural pass is successful if:

1. playback speed on the Windows RTX A1000 laptop moves materially closer to
   `1.0x`
2. `gl_draw_ms` decreases materially relative to the current `GPU Buffer`
   baseline
3. `camera_decode_gap_frames` improves or at least does not regress
4. zoom during playback still works
5. overlays remain aligned
6. stimulus behavior does not regress

## Recommended Order From Here

1. land the compact-slot representation
2. land late conversion for the displayed frame
3. validate on the Windows laptop with the existing perf log
4. after confirming render is still the bottleneck, move to the zoom-aware
   playback render plan in
   [docs/crimson_main_camera_zoom_aware_render_plan.md](./crimson_main_camera_zoom_aware_render_plan.md)
