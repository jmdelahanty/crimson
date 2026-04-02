# Crimson CUDA, Driver, and Preset Versioning

Purpose: clarify the difference between NVIDIA driver-reported CUDA support,
the CUDA toolkit Crimson compiles against, and the dependency stack encoded in
CMake presets.

Date anchored: 2026-04-02.

---

## The Short Version

It is normal for these numbers to differ:

- `nvidia-smi`
  - reports the CUDA version level supported by the installed NVIDIA driver
- `nvcc --version`
  - reports the CUDA toolkit version used to compile code
- Crimson preset name
  - identifies the dependency stack Crimson is expected to build against

Example:

- `nvidia-smi` may show `CUDA Version: 13.0`
- `nvcc --version` may show `release 12.4`
- Crimson may use preset `linux-trt10-cuda12.4-release`

That is not a contradiction.

---

## Why This Happens

The NVIDIA driver and the CUDA toolkit are related, but they are not the same
thing.

### Driver Capability

`nvidia-smi` reflects the driver side.

In practice, that number answers a question like:

- "How new of a CUDA runtime interface can this driver support?"

It does **not** tell Crimson which compiler toolchain or headers it will use.

### Toolkit Used for Compilation

`nvcc --version` reflects the CUDA toolkit installed on disk and used during
build configuration and compilation.

That is the version that matters for:

- CMake's `find_package(CUDA ...)`
- CUDA headers
- `nvcc`
- compatibility with TensorRT and other build-time dependencies

### Crimson Dependency Stack

Crimson needs a coherent set of versions, not just "some CUDA is installed".

That stack currently includes at least:

- CUDA toolkit
- TensorRT
- OpenCV
- FFmpeg

The shared presets in the repo define which stack is expected.

---

## What Crimson Should Trust

For build and packaging work, trust these in order:

1. the CMake preset name
2. the exact dependency roots passed to CMake
3. the versions CMake actually detects from those roots

Do **not** name or validate the build stack from `nvidia-smi`.

Reason:

- TensorRT and OpenCV compatibility are tied to the toolkit and dependency
  versions Crimson builds against
- the driver can be newer than the toolkit without any problem

---

## Current Repo Convention

As of 2026-04-02, the shared preset stack in this repo is:

- CUDA `12.4`
- OpenCV `4.10.0`
- TensorRT `10.0.1.6`

That is why the preset names are:

- `linux-trt10-cuda12.4-release`
- `linux-trt10-cuda12.4-debug`
- `windows-trt10-cuda12.4`

The `cuda12.4` part refers to the compile-time toolkit stack, not to the
driver-reported maximum CUDA capability.

---

## How CMake Resolves the Stack

The flow is:

1. a preset selects the expected stack
2. machine-local paths are supplied through environment variables or
   `CMakeUserPresets.json`
3. CMake finds headers and libraries under those roots
4. Crimson validates that the discovered versions match the preset

In this repo:

- shared preset definitions live in [CMakePresets.json](/home/delahantyj@hhmi.org/gitrepos/crimson/CMakePresets.json)
- exact version checks live in [CMakeLists.txt](/home/delahantyj@hhmi.org/gitrepos/crimson/CMakeLists.txt#L38)

The main path variables are:

- `CRIMSON_CUDA_TOOLKIT_ROOT`
- `CRIMSON_OPENCV_DIR`
- `CRIMSON_FFMPEG_ROOT`
- `CRIMSON_TENSORRT_ROOT`

Those become:

- `CUDA_TOOLKIT_ROOT_DIR`
- `OpenCV_DIR`
- `FFMPEG_ROOT`
- `TENSORRT_ROOT`

inside CMake configuration.

---

## Why `/usr/local/cuda` Can Be Misleading

On Linux, `/usr/local/cuda` is often just a symlink.

That means all of these can be different at the same time:

- `/usr/local/cuda` points to `cuda-13.1`
- `nvcc` on `PATH` is from `cuda-12.4`
- Crimson preset pins `CUDA_TOOLKIT_ROOT_DIR=/usr/local/cuda-12.4`
- `nvidia-smi` reports driver support at `13.0`

This is exactly why Crimson should use explicit dependency roots and exact
version checks.

---

## Practical Checks

When someone says "I have CUDA 13", first determine which meaning they mean.

### Check Driver Capability

```bash
nvidia-smi
```

Useful for:

- confirming driver installation
- understanding the driver's CUDA compatibility level

Not sufficient for:

- choosing a Crimson preset
- proving TensorRT/OpenCV compatibility

### Check the Toolkit Compiler

```bash
nvcc --version
```

Useful for:

- verifying the CUDA toolkit version on `PATH`

### Check the Toolkit Path Crimson Uses

```bash
readlink -f /usr/local/cuda
cmake --preset linux-trt10-cuda12.4-release
```

Useful for:

- verifying the toolkit root Crimson is actually configured to use
- confirming the exact-version checks pass

---

## Runtime vs Build-Time

This distinction is also separate from runtime packaging.

Build-time:

- CMake finds headers, import libraries, and shared libraries to compile/link

Runtime:

- Linux uses runtime search paths such as RPATH
- Windows usually needs required DLLs staged beside `redgui.exe`

A machine can pass `nvidia-smi` and still fail to build if the toolkit root is
wrong.

---

## Recommended Team Rule

When discussing Crimson support, use this language:

- "driver supports CUDA 13.x"
- "Crimson builds against CUDA 12.4"
- "the supported Crimson stack is TensorRT 10.0.1.6 + OpenCV 4.10.0 + CUDA 12.4"

That keeps driver capability, toolkit version, and application stack from being
mixed together.

---

## Next Step for New Stacks

If Crimson needs to support another toolkit series later, add a separate preset
stack instead of mutating the existing one.

Example future shape:

- `linux-trt10-cuda12.4-release`
- `linux-trt10-cuda13.0-release`

Only add the new stack after verifying:

- CUDA toolkit compatibility
- TensorRT compatibility
- OpenCV compatibility
- successful configure/build
- runtime validation
