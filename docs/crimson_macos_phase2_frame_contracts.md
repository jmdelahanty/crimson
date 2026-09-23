# Crimson Phase 2 Frame and Presentation Contracts

Date: 2026-07-12

Phase 2 separates decoded-frame identity, storage, lifetime, selection, and
presentation handles from CUDA, OpenGL, Metal, FFmpeg, and AVFoundation types.
It does not add the production Apple viewer; that remains Phase 3.

## Portable Types

`src/frame_types.h` is the backend-neutral contract. It defines:

- recording/global and decoder/clip-local frame identity;
- stream identity, PTS, and an exact rational timebase;
- width, height, allocation size, primary pitch, and per-plane layout;
- explicit `NV12`, `RGBA8`, and `BGRA8` pixel formats;
- color matrix and limited/full/unspecified range metadata;
- CPU, NVIDIA CUDA, and Apple VideoToolbox surface backends;
- borrowed, slot-owned, and reference-counted ownership;
- external, read-lease, and reference-counted lifetimes;
- opaque `FrameSurface` and `PresentationTexture` interfaces.

The portable headers include only C++ standard-library headers. CUDA, NVDEC,
FFmpeg, OpenCV, OpenGL, and Metal types remain in backend-specific files.

NV12 uses two explicit planes. Plane 0 is full-resolution luma with one byte per
element. Plane 1 is interleaved UV at ceil(width/2) by ceil(height/2), with two
bytes per element. RGBA8 and BGRA8 use one four-byte plane. Layout validation
checks positive dimensions, plane count, row stride, element size, and required
allocation size.

## Surface Lifetime

`src/frame_slot.*` retains a backend surface when a producer publishes a frame.
An NVIDIA or CPU producer that does not supply an object receives an internal
adapter over the existing slot allocation. Future Apple decode can publish a
reference-counted `CVPixelBuffer` adapter through the same interface.

A read lease holds the surface alive. Releasing a slot for reuse removes the
slot's reference, but the backend resource is not destroyed while any reader
still holds a lease. Writers remain blocked until all readers release. The
presenter consumes `FrameSurface::nativeHandle()` rather than reading the legacy
buffer pointer directly.

`PictureBuffer` remains a trivial compatibility object because the existing
NVIDIA renderer allocates its ring with `malloc`. Its diagnostic fields mirror
the published portable metadata.

## Presentation Adapters

- NVIDIA exposes the current front OpenGL texture through
  `NvidiaOpenGlPresentationTexture`. The adapter is refreshed after allocation,
  resize, and front/staging swaps.
- macOS exposes real `id<MTLTexture>` objects through
  `AppleMetalPresentationTexture`. Both offscreen and drawable paths verify a
  Metal backend, BGRA8 format, dimensions, and nonzero opaque handle.

Native handle types do not appear in the shared interface.

## Shared Selection

`src/frame_selection.*` is the single portable policy engine used by:

- playing camera presentation;
- playback-clock exact and best-buffered-before selection;
- paused exact-frame stepping;
- paused nearest-buffer reuse.

It distinguishes exact, preferred, buffered-before, nearest, retained-previous,
and unavailable results. A minimum exclusive frame supports skipped-present
behavior without moving backward, while exact-only selection preserves paused
frame identity.

## Portable Tests

`frame_slot_tests` builds on both macOS and NVIDIA without CUDA, FFmpeg, OpenCV,
or Zarr dependencies. It covers:

- publish, read, cancellation, reuse, multiple readers, and threaded stress;
- complete stream/frame/PTS/timebase/color metadata;
- NV12 two-plane and RGBA8/BGRA8 single-plane layouts;
- CPU, CUDA, VideoToolbox, OpenGL, and Metal representations;
- exact, nearest-buffered, skipped-present, and paused selection;
- backend-resource destruction only after the final read lease releases.

The macOS headless preset includes both this portable test and the offscreen
Metal pixel test. GUI smoke remains limited to actual window/drawable behavior.

## Validation Record

macOS Apple Silicon:

- clean configure and build passed with the macOS preset;
- `test-macos-arm64-headless` passed 2/2;
- `test-macos-arm64` passed 3/3, including the Cocoa drawable smoke;
- the portable lease/selection suite passed under AddressSanitizer,
  UndefinedBehaviorSanitizer, and ThreadSanitizer;
- the Metal adapter represented both the BGRA8 offscreen texture and live
  drawable with nonzero opaque handles.

Maintained NVIDIA host:

- CUDA 12.4, TensorRT 10.0.1.6, OpenCV 4.10.0, NVDEC/CUVID,
  OpenGL/GLEW, FFmpeg, and prebuilt TensorStore configured successfully;
- the complete 125-step build passed, including `redgui` and all CUDA sources;
- `frame_slot_tests` passed under CTest;
- the authenticated June 14 playback smoke passed:

```text
[PlaybackSmoke] PASS start_frame=0 end_frame=300 presented_frame=300 presented_slot=0 view_idx=0 presented_count=350 elapsed_s=2.9942
```

The build retained the pre-existing FFmpeg, CUDA interop, TensorRT, TensorStore,
and OpenCV/FFmpeg version warnings recorded during Phase 0. No new warning class
or runtime failure was observed.

## Phase Boundary

Phase 2 establishes representation, ownership, selection, and presentation
interfaces. It does not add AVFoundation decoding, production Metal camera
rendering, Zarr workflow migration, overlays, editing, or inference changes.
