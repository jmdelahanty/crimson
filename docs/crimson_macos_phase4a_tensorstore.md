# Crimson Phase 4A TensorStore Capability Checkpoint

Date: 2026-07-12

This checkpoint makes the pinned TensorStore C++ dependency available to the
native macOS build before Phase 4 introduces Zarr-aligned crop and stimulus
playback. It does not yet read a recording's analysis Zarr or migrate the
legacy `ZarrDetectionLoader` implementation.

## Dependency Contract

Both application backends now enter TensorStore through
`cmake/CrimsonTensorStore.cmake`. The existing NVIDIA build still supports its
prebuilt TensorStore import and falls back to the pinned `v0.1.64` source. The
macOS build fetches that same pinned source.

The macOS capability target links `crimson::tensorstore_zarr`, a narrow
interface containing:

- TensorStore core;
- the memory and POSIX file kvstores; and
- the Zarr v2 and Zarr v3 array drivers.

The POSIX file driver is the relevant path for Zarr stored on an SMB share that
macOS has already mounted. TensorStore sees ordinary filesystem paths such as
`/Volumes/johnsonlab/...`; it does not implement the SMB connection itself.

The narrow interface intentionally excludes GCS, HTTP, gRPC, and distributed
OCDBT drivers. `tensorstore::all_drivers` pulled those unrelated stacks into the
Apple build and exposed a vendored gRPC incompatibility with AppleClang 17.
They are not needed for Crimson's local or mounted-network Zarr workflow.

## Build Requirements

The macOS developer prerequisites are:

```bash
brew install cmake ninja glfw nasm
```

TensorStore uses Python during its CMake source generation and uses NASM while
building codec dependencies. These are build-time tools. Crimson does not use
the Python `zarr` package or Python TensorStore at runtime.

TensorStore 0.1.64's vendored Abseil CMake emits paired Apple architecture
flags for its Randen hardware-AES implementation. On an arm64-only build, CMake
de-duplication can leave an x86 SSE flag exposed to AppleClang. The shared
module normalizes those two Abseil targets to the native
`-march=armv8-a+crypto` option.

## Automated Capability Test

`apple_tensorstore_zarr_tests` creates and opens four tiny arrays:

- Zarr v2 in memory;
- Zarr v2 in a temporary filesystem directory;
- Zarr v3 in memory; and
- Zarr v3 in a temporary filesystem directory.

This verifies CMake linkage, static driver registration, metadata parsing,
array creation, and file access without requiring a lab recording or GUI.

Repeatable commands:

```bash
cmake --preset macos-arm64-release
cmake --build --preset build-macos-arm64-release
ctest --preset test-macos-arm64
```

## Validation Record

Apple Silicon macOS:

- the source-fetched TensorStore build completed with AppleClang 17;
- `apple_tensorstore_zarr_tests` passed all four driver combinations; and
- the complete native suite passed 6/6, including frame ownership, the Apple
  video provider, offscreen Metal pixels, headless Metal, and Cocoa shell
  smoke coverage.

Maintained NVIDIA host:

- CUDA 12.4 with architectures 80/86, TensorRT 10.0.1.6, OpenCV 4.10, NVIDIA
  FFmpeg, and the prebuilt TensorStore import configured successfully;
- the complete 133-step disposable build linked `redgui`, the decode smoke,
  and existing Zarr diagnostic tools; and
- CTest passed its portable frame-contract suite.

The NVIDIA build retained the pre-existing OpenCV/FFmpeg version-family linker
warnings. No new warning class or behavior change was observed.

## Phase Boundary

Phase 4A proves that macOS can build and register the TensorStore drivers needed
by Crimson. The next checkpoint should define a backend-neutral Zarr repository
facade and test it against a small fixture before wiring actual crop and
stimulus streams into the macOS viewer. A production Zarr recording is useful
for the later parity gate, but it is not required to design or unit-test that
repository contract.
