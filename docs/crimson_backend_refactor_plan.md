# Crimson Backend Refactor Plan

Date anchored: 2026-04-06.

Purpose: outline what a more backend-agnostic Crimson architecture should look
like after the recent Windows playback investigation and the comparison against
`red`'s `origin/rob_ui_overhaul` branch.

This is not a commitment to implement every backend below. It is a design note
for separating responsibilities that are currently too entangled in the
CUDA/OpenGL/NVDEC path.

Related docs:

- [docs/crimson_red_efficiency_comparison.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_red_efficiency_comparison.md)
- [docs/crimson_main_camera_playback_renderer_plan.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_main_camera_playback_renderer_plan.md)
- [docs/crimson_main_camera_late_conversion_plan.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_main_camera_late_conversion_plan.md)

## Why This Exists

The current Crimson code path still mixes too many concerns together:

- demux/decode
- buffered-slot ownership
- presentation conversion
- texture upload
- OpenGL presentation details
- playback pacing and frame selection

That coupling lives primarily in:

- [src/decoder.cpp](/home/delahantyj@hhmi.org/gitrepos/crimson/src/decoder.cpp)
- [src/render.h](/home/delahantyj@hhmi.org/gitrepos/crimson/src/render.h)
- [src/red.cpp](/home/delahantyj@hhmi.org/gitrepos/crimson/src/red.cpp)
- [src/gx_helper.h](/home/delahantyj@hhmi.org/gitrepos/crimson/src/gx_helper.h)

The recent Windows laptop work ruled out several smaller hypotheses and
clarified that the remaining problems are structural:

- decode submit can be expensive on weaker GPUs
- presentation/render can be expensive on weaker GPUs
- improving one stage often just moves the stall into another because the
  backend boundaries are not clean

## Current Pain Points

### 1. Decoder and frame publication are tightly coupled

The main decoder thread in
[src/decoder.cpp](/home/delahantyj@hhmi.org/gitrepos/crimson/src/decoder.cpp)
currently does all of the following in one loop:

- demux packet
- submit packet to `NvDecoder`
- get decoded frame
- possibly convert to `RGBA`
- write the selected ring slot
- publish frame number / availability

That makes it hard to:

- swap decode backends cleanly
- decouple decode submit from buffered-frame delivery
- reason about where the real bottleneck is

### 2. Render allocation and backend state are mixed together

[src/render.h](/home/delahantyj@hhmi.org/gitrepos/crimson/src/render.h)
currently owns:

- camera dimensions
- GL textures
- CUDA-registered GL PBOs
- playback staging PBOs
- buffered slot memory
- seek state
- upload bookkeeping

That is too much backend-specific state for a single "scene memory" structure.

### 3. `red.cpp` is doing backend policy and backend execution

[src/red.cpp](/home/delahantyj@hhmi.org/gitrepos/crimson/src/red.cpp) now
contains a large amount of backend execution logic, including:

- CPU-buffer vs GPU-buffer branching
- NV12 vs RGBA slot branching
- display-time conversion branching
- playback staging / swap behavior
- OpenGL upload details
- playback renderer mode behavior

That makes the UI loop a backend implementation file instead of primarily an
application flow/controller file.

## Refactor Goal

Split Crimson into explicit backend-owned layers so that:

1. app/playback logic chooses *what frame should be shown*
2. a decode backend decides *how frames are produced and buffered*
3. a presentation backend decides *how the selected frame becomes an on-screen
   texture/image*
4. the UI layer just draws the resulting camera view and overlays

In short:

- app logic should not know about CUDA PBO mapping details
- decoder logic should not care about ImGui/OpenGL presentation details
- presentation logic should not own playback policy

## Proposed Backend Boundaries

### A. Decode Backend

Responsibilities:

- open stream / media source
- demux packets or source frames
- decode frames
- publish decoded-frame handles into backend-owned buffered slots
- expose timing / queue depth / published-frame telemetry

Examples of future implementations:

- `NvdecDecodeBackend`
- `SoftwareDecodeBackend`
- hypothetical Apple `VideoToolboxDecodeBackend`

The decode backend should return an abstract slot handle or frame handle, not
"a raw pointer the UI must interpret."

### B. Buffered Frame Store

Responsibilities:

- own the ring of buffered slots
- manage slot availability, published frame number, format metadata, and seek
  invalidation
- expose a query like:
  - `find_best_slot_for_frame(frame_number)`
  - `latest_published_frame()`

This should be backend-neutral at the interface level, even if concrete
implementations differ in storage type:

- CPU RGBA buffer
- GPU NV12 buffer
- platform-native decoded surface handle

Today this logic is spread across:

- [src/decoder.h](/home/delahantyj@hhmi.org/gitrepos/crimson/src/decoder.h)
- [src/render.h](/home/delahantyj@hhmi.org/gitrepos/crimson/src/render.h)
- [src/red.cpp](/home/delahantyj@hhmi.org/gitrepos/crimson/src/red.cpp)

### C. Presentation Backend

Responsibilities:

- take a selected decoded/buffered frame
- make it presentable to the UI/render backend
- own conversion/upload/swap/present details
- expose an `ImTextureID`-like or renderable handle to the UI

Examples:

- `OpenGlCudaPresentationBackend`
- hypothetical `MetalPresentationBackend`
- hypothetical `D3D11PresentationBackend`

This is the layer that should own things like:

- GL texture lifetime
- GL PBO lifetime
- CUDA/GL interop mapping
- display-time NV12->RGBA conversion
- staging surfaces

Today those details are split between:

- [src/render.h](/home/delahantyj@hhmi.org/gitrepos/crimson/src/render.h)
- [src/red.cpp](/home/delahantyj@hhmi.org/gitrepos/crimson/src/red.cpp)
- [src/gx_helper.h](/home/delahantyj@hhmi.org/gitrepos/crimson/src/gx_helper.h)

### D. Render UI Adapter

Responsibilities:

- expose the already-presentable image to ImGui / ImPlot / future custom widget
- preserve camera view transform semantics
- draw overlays in camera coordinates

This layer should *not* own decode or upload policy.

It should consume something like:

- `PresentedCameraFrame`
  - texture/view handle
  - image dimensions
  - source frame number
  - coordinate transform metadata

### E. Playback Controller

Responsibilities:

- requested playback speed
- requested frame number
- slider / seek / pause / step semantics
- clamping to available decoded frames
- deciding which streams need decode right now

This layer is application policy, not backend implementation.

Today this logic is mixed into the main loop in
[src/red.cpp](/home/delahantyj@hhmi.org/gitrepos/crimson/src/red.cpp) along
with backend execution details.

## Suggested Interface Shape

The exact C++ interface names can change, but the shape should be close to:

### `IDecodeBackend`

- `open_stream(...)`
- `start()`
- `stop()`
- `request_seek(frame_number, accurate)`
- `latest_decoded_frame()`
- `frame_store()`
- `perf_counters()`

### `IFrameStore`

- `find_slot_for_frame(frame_number)`
- `latest_published_frame()`
- `slot_format(slot_id)`
- `slot_dimensions(slot_id)`
- `slot_frame_number(slot_id)`

### `IPresentationBackend`

- `initialize(window, device_selection, resource_root)`
- `present_slot(slot_handle, present_request)`
- `current_texture_handle(camera_id)`
- `begin_frame()`
- `end_frame()`
- `perf_counters()`

### `CameraPresentRequest`

- desired frame number
- view bounds / zoom state
- playback vs paused mode
- desired presentation fidelity
- contrast/brightness params

The point is not to make everything virtual for its own sake. The point is to
stop routing all backend decisions through `red.cpp`.

## Immediate Practical Win From This Refactor

Even before adding any new backend, this would improve the current NVIDIA path:

- `decoder.cpp` could become a true decoder backend instead of a direct
  publisher into UI-owned memory
- `render.h` could stop being a mixed allocation + backend-implementation file
- `red.cpp` could become mostly playback/UI policy

That would make future experiments much cheaper to run:

- different presentation strategies
- different decode submission strategies
- backend-specific tuning for weak GPUs

## What "Backend-Agnostic" Actually Means Here

It does **not** mean pretending all platforms are identical.

It means:

- backend differences live behind intentional boundaries
- application logic depends on common concepts
- platform-specific fast paths remain allowed and expected

Examples:

- macOS backend can use VideoToolbox + Metal
- Linux/Windows NVIDIA backend can use NVDEC + CUDA + OpenGL
- a future Windows-native backend could use D3D11/12 or Media Foundation

The app logic should not care how the presented texture was produced.

## Linux Thoughts

Linux should not be treated as "the generic baseline" if that forces the wrong
architecture everywhere else.

For Linux/NVIDIA specifically:

- preserve compact `NV12` buffering
- do not regress toward buffering every frame as `RGBA`
- keep CUDA/NVDEC-specific fast paths where they help
- decouple decode publication from UI timing more strongly

So the Linux backend should remain explicit, not hidden behind fake
"cross-platform simplicity."

## Windows Thoughts

The Windows RTX A1000 laptop findings suggest:

- weak NVIDIA GPUs can be limited by both decode submit and presentation
- backend tuning matters
- control-panel / VSync tweaks are not enough

So Windows should probably keep using the NVIDIA backend for now, but behind a
cleaner backend boundary.

That gives room for future options like:

- different presentation implementations on Windows
- different decode queueing behavior
- a non-NVIDIA backend if that ever becomes worth it

## macOS Thoughts

This refactor does not mean "macOS support arrives automatically."

It just creates a shape where a future macOS backend would have somewhere
coherent to live:

- Apple decode backend
- Apple presentation backend
- Apple inference backend if needed

That is much better than trying to wedge macOS behavior into the current
OpenGL/CUDA-oriented implementation.

## Incremental Rollout Plan

### Phase 1: Backend boundary extraction

- define decode-store-presentation boundaries in code
- move current NVIDIA/OpenGL logic behind those boundaries
- keep behavior unchanged

Success condition:

- current Linux/Windows behavior still matches
- profiler output remains available

### Phase 2: Presentation backend cleanup

- move upload/conversion/staging logic out of `red.cpp`
- give `red.cpp` a simple present-frame call

Success condition:

- playback experiments stop requiring large edits to the UI loop

### Phase 3: Decode backend cleanup

- move slot publication and decoder perf ownership into a backend-owned object
- make decode submit and frame publication more decoupled

Success condition:

- the decoder is no longer a direct writer into ad hoc UI-owned structures

### Phase 4: Optional future backends

- future Apple backend
- future Windows-native presentation backend
- future alternate inference backend split if needed

## Current Recommendation

Do not start by trying to add a new platform backend immediately.

Start by extracting the current NVIDIA/OpenGL path into explicit decode and
presentation backend boundaries.

That is the highest-leverage way to:

- reduce backend complexity in `red.cpp`
- make future experiments cheaper
- prepare both Linux/Windows optimization work and any future macOS effort
