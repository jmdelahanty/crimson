# Crimson Playback Preview Scale Plan

## Why This Exists

Playback profiling on the Windows RTX A1000 laptop showed that the main camera
path is still the dominant bottleneck even after improving stimulus playback.

Recent profiler output indicated:

- `gl_draw_ms` was effectively at the full `60 fps` frame budget
- `camera_decode_gap_frames` remained persistently positive
- `camera_upload_ms` was negligible
- `swap_ms` was negligible

That means the app is primarily limited by:

1. main-camera render cost
2. main-camera decode lag

It is **not** primarily limited by:

- stimulus playback in the measured sample
- PCIe upload cost for the displayed camera frame
- `glfwSwapBuffers` / VSync wait

## Why A 4512x4512 Texture Can Be Drawn On A 1920x1200 Laptop

The screen resolution limits the number of final pixels shown on screen, but it
does **not** limit the size of textures that OpenGL can allocate and sample.

Crimson currently does this:

1. allocate an OpenGL texture at the camera's native frame size
2. upload the full decoded frame into that texture
3. draw that texture into a much smaller UI region

Relevant code paths:

- native-size texture allocation in [src/render.h](../src/render.h)
- frame upload into the PBO/texture in [src/red.cpp](../src/red.cpp)
- image draw via `ImPlot::PlotImage` in [src/red.cpp](../src/red.cpp)

So even on a `1920x1200` laptop screen, the GPU can hold a `4512x4512` RGBA
texture in VRAM and sample it down to the smaller visible camera window.

That is why playback can remain expensive even when the user is not viewing the
video at native 1:1 size.

## Safe Preview-Scale Plan

The goal is to reduce **display cost during playback** without reducing source
fidelity for paused inspection or downstream data.

### Scope

This is a **display-only** optimization.

For the larger follow-on architecture that stores compact main-camera frames and
converts only the displayed frame, see:

- [docs/crimson_main_camera_late_conversion_plan.md](./crimson_main_camera_late_conversion_plan.md)

It does **not**:

- change the source video
- change the decoded frame numbering
- change zarr overlays or their coordinate system
- change saved outputs

It only changes the temporary texture used for the live playback preview.

### First Implementation

Add an explicit `Playback Preview Scale` control for the main camera preview:

- `1x (Full Resolution)`
- `1/2`
- `1/4`

Behavior:

- active only while playback is running
- automatically returns to full-resolution preview when paused
- keeps overlay coordinates in full-image space
- keeps the decoded/source frame data full-resolution

The preview texture is still drawn across the full camera coordinate space, so
all overlays remain aligned. The user sees a lower-detail live preview, not a
different coordinate system.

### Conservative Constraints For The First Pass

The first pass should stay conservative:

- apply only to the **main camera** playback view
- keep stimulus behavior unchanged
- prefer enabling the scaled preview only when the main stream is using
  `CPU Buffer`
- fall back to full-resolution preview when YOLO inference is active, because
  the current YOLO path reads from the display PBO

These constraints keep the feature low-risk while still testing whether
playback-only downscaled preview helps on low-VRAM laptops.

## Why This Should Help

The user is already usually seeing the camera image shrunk to a much smaller UI
window. Right now Crimson still uploads and renders a native-size texture before
that shrink happens.

A playback-only preview scale can reduce:

- texture memory pressure for the drawn preview
- texture sampling cost during playback
- some downstream render cost in the main scene view

while preserving full-resolution source data for paused work.

## What It Will Not Fix By Itself

Preview scaling is not expected to solve all playback issues alone.

The profiler also shows persistent camera decode lag, so additional work is
likely still needed after this:

- deeper camera decode instrumentation
- lighter play-mode overlays
- possibly a simpler play-mode camera renderer

## Success Criteria

The first pass is successful if:

- measured playback speed moves closer to `1.0x`
- `gl_draw_ms` decreases materially in the perf log
- the camera preview remains aligned with overlays
- paused inspection still shows the full-resolution camera image
