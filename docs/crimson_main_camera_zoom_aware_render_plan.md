# Crimson Main-Camera Zoom-Aware Playback Render Plan

Date anchored: 2026-04-04.

## Status Update

This doc remains useful background, but the latest zoomed-playback telemetry
changed the immediate priority.

Specifically:

- tight zoom during playback did **not** materially reduce `gl_draw_ms`
- the camera viewport stayed the same size on screen
- render time remained effectively flat between full-view and tight-zoom
  samples

That means ROI-aware rendering is no longer the highest-priority next
experiment by itself. The next plan has shifted to a cheaper playback-specific
camera renderer:

- [docs/crimson_main_camera_playback_renderer_plan.md](./crimson_main_camera_playback_renderer_plan.md)

## Why This Exists

The first late-conversion slice for the main camera is now in place:

- main-camera `GPU Buffer` slots are stored in compact `NV12`
- only the selected displayed frame is converted to `RGBA`
- upload and steady decoder-side materialization cost are now negligible in the
  relevant Windows laptop captures

That was an important architectural cleanup, but it did **not** solve the
remaining playback limit on the Windows RTX A1000 laptop.

The newest profiler results still show:

- `gl_draw_ms` sitting essentially at the `60 fps` frame budget
- playback speed still below real time
- `camera_decode_gap_frames` staying large as a downstream symptom

At the same time:

- `camera_display_convert_ms` is now tiny
- `camera_upload_ms` is tiny
- `camera_pbo_copy_ms = 0`
- steady decoder pipeline timing is tiny

So the next bottleneck is no longer upload or per-slot conversion. It is the
actual playback render path.

## Important Context

This remaining issue is now primarily a **machine-capacity problem** on the
Windows laptop, not a sign that Crimson is fundamentally broken.

That matches observed behavior:

- the low-VRAM RTX A1000 laptop still struggles with a `4512x4512 @ 60 fps`
  fully interactive main-camera playback path
- the user does **not** see this issue on a much stronger desktop with an
  NVIDIA RTX A6000

So this optimization is best understood as:

- a targeted low- to mid-tier GPU playback optimization
- not a universal requirement for high-end workstations

## Why The Current Preview Control Was Not Enough

The current playback preview control is too weak in the `GPU Buffer` path.

In the current code:

- `CPU Buffer` preview can create a genuinely smaller temporary preview image
- but `GPU Buffer` preview mostly changes texture sampling / mip behavior on the
  existing full-size display texture

Relevant code paths:

- preview activation and target shape selection in
  [src/red.cpp](../src/red.cpp)
- mip-based preview sampling setup in [src/red.cpp](../src/red.cpp)
- displayed-frame `NV12 -> RGBA` conversion in [src/red.cpp](../src/red.cpp)
- final OpenGL draw timing around `ImGui_ImplOpenGL3_RenderDrawData(...)` in
  [src/red.cpp](../src/red.cpp)

This means:

- the `GPU Buffer` path still produces a full-size displayed `RGBA` image
- the final render pass still shades the same on-screen camera view
- `1/2` and `1/4` preview settings do not materially reduce the actual draw
  cost

That is why the latest captures showed very similar `gl_draw_ms` even with
`1/4` preview.

## What Must Be Preserved

The user requirement is strict:

- zoom during playback remains available
- panning during playback remains available
- overlays remain visible and aligned
- paused inspection remains full fidelity

This rules out simply replacing the main camera view with a stripped-down
non-interactive image widget.

## Core Idea

Make playback rendering depend on the **visible view**, not always on the full
camera frame.

In practice:

- when zoomed out, render a reduced-resolution, view-sized playback image
- when zoomed in, convert and render only the visible ROI at full resolution

This is the next logical step after late conversion:

- current late conversion decides **which frame** to convert late
- the next step decides **how much of that frame** to convert and render

## Why This Was A Plausible Next Step

This was a reasonable next hypothesis because:

- further main-camera decode tuning
- a software main-camera decoder
- more preview LOD bias / mip sampling tweaks
- removing zoom support during playback

Why:

- the FFmpeg benchmark already showed software main-camera decode is not the
  right direction for `4512x4512 HEVC 60 fps`
- the profiler showed render cost, not upload cost, as the dominant steady
  limiter
- the user needs zoom during playback, so a simpler non-interactive renderer is
  not acceptable

But the newer zoomed-playback capture showed that zooming into a tiny visible
source-image fraction still did not lower draw time. That makes ROI-aware
rendering a weaker next optimization than a cheaper playback renderer.

## Target Design Context

### Zoomed-Out Playback Path

When the current camera view is effectively showing the whole frame:

- build a reduced-resolution playback image sized to the actual visible camera
  region
- render that image into the existing interactive camera view

This should reduce:

- displayed-frame conversion work
- texture size used for playback presentation
- final image shading cost

### Zoomed-In Playback Path

When the user zooms into a subregion of the main camera:

- compute the visible ROI in source-image coordinates
- convert only that ROI at native resolution
- present that ROI in the current camera view

This preserves:

- playback zoom behavior
- spatial fidelity in the zoomed region
- overlay alignment

### Paused Path

Paused inspection may remain more expensive than playback.

That is acceptable because:

- the primary complaint is live playback throughput
- paused inspection is the place where full-fidelity cost is most justified

## Preserving Current Interaction Semantics

The goal is **not** to discard the current ImPlot camera interaction model.

The safer approach is:

- keep the existing plot/view coordinate system
- keep current zoom/pan semantics
- change the source image that is being shown inside that view

This suggests a render-path change, not a wholesale camera-widget rewrite.

## Implementation Shape Context

### Phase 1: Instrument And Detect View State

Add explicit determination of:

- whether the camera is effectively zoomed out or zoomed in
- the visible source-image bounds for the current camera view
- the actual viewport size being drawn

Acceptance:

- ROI bounds are stable and correct
- no behavior change yet

### Phase 2: View-Sized Whole-Frame Playback Path

For zoomed-out playback:

- create a playback image sized to the visible camera region on screen
- populate it from the selected buffered frame
- keep paused mode on the existing full-fidelity path

Acceptance:

- playback still looks correct when fully zoomed out
- overlays remain aligned
- if this path is revisited later, `gl_draw_ms` should drop materially on the
  laptop

### Phase 3: ROI Full-Resolution Playback Path

For zoomed-in playback:

- compute the visible ROI in source-image coordinates
- convert only that ROI at full resolution
- render the ROI into the current view

Acceptance:

- zoomed playback remains crisp in the visible region
- overlays still align correctly
- no interaction regressions
- if revisited later, this phase should be justified by a renderer path where
  ROI can actually reduce work

### Phase 4: Tune Thresholds

Decide when to switch between:

- whole-frame reduced-resolution playback
- ROI full-resolution playback

This threshold should be driven by:

- visible fraction of the source image
- viewport size
- practical laptop performance

## Open Questions

1. Should the reduced-resolution playback image be generated via:
   - CUDA downscale into the display path
   - OpenGL shader/FBO path
   - another GPU path?
2. For ROI mode, is it better to:
   - crop in CUDA before conversion
   - convert full-frame and sample ROI
   - or use a shader-driven YUV render path?
3. What zoom threshold should switch from whole-frame reduced playback to
   ROI full-resolution playback?

## Risks

### Overlay Alignment

The largest functional risk is breaking overlay alignment if the view-to-source
mapping becomes ambiguous.

### Interaction Regressions

We must not lose:

- zoom during playback
- pan during playback
- current camera coordinate semantics

### Complexity

This is a more meaningful render-path change than the current preview-scale
control.

That is justified because:

- the cheaper earlier changes have already been tried
- the profiler now points clearly at the render path

## Acceptance Criteria

This phase is successful if, on the Windows RTX A1000 laptop:

1. measured playback speed moves materially closer to `1.0x`
2. `gl_draw_ms` drops materially relative to the current late-conversion
   baseline
3. zoom and pan still work during playback
4. overlays remain aligned
5. the A6000-class desktop path does not regress

## Relationship To Other Docs

This is the follow-on plan after:

- [docs/crimson_playback_preview_scale_plan.md](./crimson_playback_preview_scale_plan.md)
- [docs/crimson_main_camera_late_conversion_plan.md](./crimson_main_camera_late_conversion_plan.md)

The sequence is now:

1. playback preview experiments
2. late conversion of the displayed frame
3. zoom-aware playback render path for weaker GPUs
