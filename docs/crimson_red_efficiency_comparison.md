# Crimson vs RED Efficiency Comparison

Date anchored: 2026-04-06.

Purpose: capture what the `origin/rob_ui_overhaul` branch in
`~/gitrepos/red` appears to be doing more efficiently than Crimson, what is
platform-specific to macOS, and what that means for Linux/NVIDIA efficiency
work in Crimson.

This is not a benchmark report. It is a design-comparison note based on source
inspection.

Related Crimson docs:

- [docs/crimson_main_camera_playback_renderer_plan.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_main_camera_playback_renderer_plan.md)
- [docs/crimson_main_camera_late_conversion_plan.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_main_camera_late_conversion_plan.md)
- [docs/crimson_windows_trt10_cuda12.4_validation_record.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_trt10_cuda12.4_validation_record.md)

External comparison target:

- `~/gitrepos/red` at `origin/rob_ui_overhaul`

## Short Version

The most important efficiency difference is not a missed SIMD trick.

The RED branch has a more native media/render backend split:

- macOS uses VideoToolbox decode plus Metal rendering
- Linux uses NVDEC plus CUDA/OpenGL
- the render abstraction is explicitly platform-switched

Crimson is still mostly centered on one CUDA/OpenGL-oriented architecture.

At the same time, Crimson now has at least one important efficiency advantage
over RED's Linux/NVIDIA path:

- Crimson's main-camera `GPU Buffer` path stores buffered slots as compact
  `NV12` and converts only the displayed frame for presentation
- the inspected RED Linux decoder still converts every decoded frame to full
  `RGBA` before storing it in the ring

So the main lesson from RED is not "copy its Linux decode path."
The useful lesson is "treat decode/render as a platform backend and decouple
decode delivery from the UI/render loop more aggressively."

## What RED Appears To Do Better

### 1. Native Apple media/render integration

On macOS, the RED branch uses:

- FFmpeg demux
- asynchronous VideoToolbox decode
- `CVPixelBufferRef` output
- `CVMetalTextureCacheCreateTextureFromImage(...)`
- Metal-backed ImGui rendering

Relevant files in `red`:

- `src/vt_async_decoder.h`
- `src/vt_async_decoder.mm`
- `src/metal_context.h`
- `src/metal_context.mm`
- `src/render.h`
- `src/render.cpp`

This is a cleaner architecture than trying to preserve a CUDA/NVDEC/OpenGL path
on macOS. It leans on the native platform decoder and native graphics stack
instead of forcing one cross-platform GPU path everywhere.

### 2. More explicit render-backend abstraction

The RED branch's render abstraction is visibly backend-aware:

- on macOS it stores `ImTextureID` handles backed by Metal textures
- on Linux it stores GL textures and PBO/CUDA state

That split is in `red/src/render.h`.

Crimson currently has a much heavier assumption that presentation goes through
the OpenGL/CUDA path in [src/render.h](/home/delahantyj@hhmi.org/gitrepos/crimson/src/render.h)
and [src/red.cpp](/home/delahantyj@hhmi.org/gitrepos/crimson/src/red.cpp).

### 3. Asynchronous decoded-frame delivery on macOS

RED's VideoToolbox path is callback-driven:

- decode completion enqueues retained `CVPixelBufferRef` outputs
- the main thread later drains that queue and imports them into Metal

That is materially different from Crimson's current main decoder flow in
[src/decoder.cpp](/home/delahantyj@hhmi.org/gitrepos/crimson/src/decoder.cpp),
which still does:

- `Demux`
- `Decode`
- `GetFrame`
- ring-slot write / publish

in a single decoder-thread path.

This difference is important because Crimson's Windows laptop profiling showed a
real steady cost in `camera_decode_submit_ms`. A more strongly decoupled
decode-output queue is a plausible future improvement.

## What Crimson Already Does Better Than RED's Linux Path

### 1. Late conversion of buffered main-camera frames

Crimson's main-camera `GPU Buffer` path now stores buffered slots as `NV12`
in [src/render.h](/home/delahantyj@hhmi.org/gitrepos/crimson/src/render.h) and
writes those slots directly from the decoder in
[src/decoder.cpp](/home/delahantyj@hhmi.org/gitrepos/crimson/src/decoder.cpp).

The selected display frame is converted later in
[src/red.cpp](/home/delahantyj@hhmi.org/gitrepos/crimson/src/red.cpp).

By contrast, the inspected RED Linux decoder in `red/src/decoder.cpp` still:

- decodes with `NvDecoder`
- immediately runs `Nv12ToColor32<RGBA32>(...)`
- stores every buffered frame as `RGBA`

So Crimson's current Linux/Windows NVIDIA path is already more memory-efficient
than that older RED Linux path.

### 2. Better playback telemetry

Crimson now has detailed playback telemetry for:

- decode demux / submit / convert / write timing
- upload timing
- presentation timing
- UI window timing
- playback speed and decode gaps

That instrumentation made it possible to rule out several false hypotheses on
the Windows RTX A1000 laptop. The RED branch may be architecturally cleaner in
places, but Crimson is much easier to diagnose today.

## What Is Probably Not The Missing Trick

The inspected RED branch does not suggest that Crimson is missing:

- some obvious CPU SIMD optimization
- a tiny ImGui-only tweak
- a small shader tweak that would suddenly unlock `1.0x`

The relevant differences are backend and pipeline architecture decisions:

- native platform decode/render integration
- explicit render backend separation
- stronger decode/output decoupling

## Linux Efficiency Thoughts

Linux deserves a separate conclusion because RED's macOS design is not directly
portable to the NVIDIA/Linux path.

### The RED macOS path is not the Linux template

RED's strongest efficiency story is the macOS path:

- VideoToolbox gives BGRA `CVPixelBuffer` outputs
- those buffers are imported into Metal textures via `CVMetalTextureCache`

That is an Apple-specific advantage. It does not imply that Crimson's best
Linux path is to imitate Apple's exact flow.

### Crimson is already ahead of RED's inspected Linux decoder in one key way

On Linux/NVIDIA, the inspected RED decoder still expands every buffered frame to
full `RGBA`.

Crimson now avoids that in the main-camera `GPU Buffer` path by:

- keeping buffered slots in `NV12`
- converting only the presented frame

So for Linux/NVIDIA, Crimson should not regress toward the older RED Linux
model.

### The more relevant Linux lesson is backend separation

The useful Linux-facing lessons from RED are:

- keep the Linux/NVIDIA backend explicit instead of burying it in one giant
  shared render path
- make decode delivery less tightly coupled to immediate frame publication
- allow presentation/render code to differ by platform/backend without forcing
  the whole app through one path

### Likely Linux priorities for Crimson

For Linux/NVIDIA, the next higher-value ideas are still:

- preserve compact `NV12` buffering
- further decouple decode submit from frame publication
- reduce presentation/render synchronization costs
- keep backend-specific code paths explicit where they materially differ

The Windows laptop work also suggests one cross-platform caution:

- a lower-end GPU can be simultaneously limited by decode submit and final
  presentation
- so Linux work should also avoid assuming "decode" or "draw" is the only
  bottleneck without measurement

## Practical Implications For Crimson

### If the goal is a future macOS viewer

The RED branch suggests the realistic path is:

- Apple-native decode backend
- Apple-native render backend
- explicit platform split

not:

- trying to stretch the current CUDA/NVDEC/OpenGL architecture onto macOS

### If the goal is better Linux/Windows NVIDIA efficiency

The most transferable ideas are:

1. stronger backend separation
2. more decoupled decode-output delivery
3. avoiding unnecessary full-frame expansion

The least transferable idea is:

- RED's VideoToolbox + Metal import path itself, because that is Apple-native

## Current Recommendation

Treat RED's `rob_ui_overhaul` branch as evidence for these conclusions:

- platform-native media/render stacks can be materially cleaner and more
  efficient than forcing one GPU architecture everywhere
- Crimson should not copy RED's older Linux habit of buffering every frame as
  `RGBA`
- the most promising backend-level improvement Crimson still lacks is stronger
  decode/output decoupling, not a small CPU optimization

If this comparison turns into engineering work, the best follow-up doc would be
a backend refactor note describing:

- current Crimson NVIDIA backend responsibilities
- which responsibilities should move behind a real backend interface
- what a future Apple backend or future D3D/Windows backend would need to own

Follow-up:

- [docs/crimson_backend_refactor_plan.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_backend_refactor_plan.md)
