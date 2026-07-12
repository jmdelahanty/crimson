# Crimson Phase 4B Stimulus Repository Contract

Date: 2026-07-12

Phase 4B introduces the first domain repository facade used by both the legacy
NVIDIA loader and the native macOS Zarr path. It covers stimulus frame identity
and alignment only. Crop video decoding, stimulus video decoding, and composite
Metal presentation remain later Phase 4 checkpoints.

## Public Contract

`src/zarr/stimulus_repository.h` is a standard C++ interface with no
TensorStore, CUDA, OpenGL, Metal, FFmpeg, or AVFoundation types. A lookup
returns a `StimulusFrameResolution` containing:

- `mapped`, `missing`, or `out_of_range` status;
- the camera frame;
- optional metadata and stimulus frame identities;
- the mapping source that won; and
- whether the selected stimulus identity was interpolated.

Missing is distinct from out of range. A frame inside the archive's mapping
extent can be intentionally unmapped, while a negative frame or frame beyond
all mapping arrays is outside the repository domain.

## Mapping Precedence

Corrected-preferred resolution follows the existing Crimson order:

1. `camera_to_stimulus_frame_corrected` direct lookup;
2. corrected camera-to-metadata plus corrected metadata-to-stimulus lookup;
3. legacy camera-to-metadata plus legacy metadata-to-stimulus lookup.

An invalid value at one level falls through to the next level for that camera
frame. `LegacyOnly` bypasses both corrected paths. Reverse lookup preserves the
same source precedence before choosing the first matching camera frame.

`camera_stimulus_frame_interpolated` directly describes corrected direct
lookups. The legacy `camera_interpolation_mask` uses the opposite convention:
one means original and zero means interpolated. The repository normalizes both
representations to the single `interpolated` result field.

## Archive Context

`src/zarr/archive_context.h` is a shared archive handle with a Pimpl boundary.
Its implementation owns the TensorStore context, opened file kvstore, and root
path. Public domain APIs therefore do not need to rediscover or expose storage
objects.

`src/zarr/tensorstore_stimulus_repository.cpp` reads only the stimulus run
pointer, frame-alignment attributes, mapping arrays, metadata frame arrays, and
interpolation masks. It supports an explicitly requested run or the Zarr group
`latest` attribute. The adapter uses the narrow local/file TensorStore driver
set established in Phase 4A.

The legacy `ZarrDetectionLoader` remains the NVIDIA backend adapter. Its public
stimulus lookup methods now delegate to the portable resolver, so both paths
share one precedence implementation while broader loader migration proceeds
incrementally.

## Fixture Test

`tests/fixtures/stimulus_alignment_repository_fixture.json` checks in the
scientific mapping values and expected results. The headless test uses
TensorStore to materialize those arrays as a temporary Zarr v3 archive, writes
the group metadata and latest-run pointer, opens a real `ArchiveContext`, and
then verifies:

- direct corrected precedence;
- corrected metadata fallback;
- legacy fallback and legacy-only mode;
- missing and out-of-range distinction;
- interpolation normalization;
- metadata identities;
- reverse stimulus-to-camera lookup; and
- first mapped camera and stimulus identities.

The temporary archive is deleted after each run. No GUI, network share, Python
Zarr, or production recording is required.

Repeatable commands:

```bash
cmake --build --preset build-macos-arm64-release
build/macos-arm64-release/stimulus_repository_tests
ctest --preset test-macos-arm64
```

## Validation Record

Apple Silicon macOS:

- the TensorStore fixture repository test passed;
- the complete native build passed; and
- CTest passed 7/7, including the existing frame, video-provider, Metal pixel,
  headless Metal, and Cocoa smoke tests.

Maintained NVIDIA host:

- CUDA 12.4, architectures 80/86, TensorRT 10.0.1.6, OpenCV 4.10, NVIDIA
  FFmpeg, and the prebuilt TensorStore import configured successfully;
- the delegated legacy loader compiled in `redgui` and the standalone clipped
  loader probe;
- both executables linked after the portable contract library was made an
  explicit dependency; and
- CTest passed its portable suite.

The NVIDIA build retained its known OpenCV/FFmpeg version-family linker
warnings. No new runtime dependency or warning class was introduced.

## Phase Boundary

This checkpoint proves the repository boundary and alignment semantics without
a production Zarr. The next checkpoint should open the representative analysis
Zarr through `ArchiveContext`, compare repository results against the legacy
loader at selected camera frames, and expose the resolved stimulus video path.
Only after that parity test should the macOS viewer add a second decoded stream
and composite presentation.
