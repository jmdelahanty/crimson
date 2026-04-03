# Crimson Windows First Validation Guide

Purpose: provide a step-by-step guide for bringing Crimson up on a first Windows
test machine and collecting the evidence needed for the planned Windows stack.

Date anchored: 2026-04-02.

Scope:

- first Windows validation machine
- intended stack: CUDA `12.4`, TensorRT `10.0.1.6`, OpenCV `4.10.0`
- target preset: `windows-trt10-cuda12.4`
- 2D-only fallback preset: `windows-trt10-cuda12.4-no-sfm`

Related docs:

- [docs/crimson_windows_trt10_cuda12.4_validation_record.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_trt10_cuda12.4_validation_record.md)
- [docs/crimson_supported_dependency_stack_matrix.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_supported_dependency_stack_matrix.md)
- [docs/crimson_dependency_stack_promotion_process.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_dependency_stack_promotion_process.md)

---

## Recommended Test Machine

A Windows laptop with a dedicated NVIDIA GPU is a good first validation machine.

It is enough to:

- prove whether the planned Windows stack is viable
- surface Windows-specific toolchain issues
- begin the `planned` -> `baseline` promotion process

It is not enough by itself to prove broad Windows support, but it is the right
starting point.

---

## Step 0: Choose the Repo Ref to Validate

Do not start by cloning an arbitrary ref.

Clone the branch or revision that actually contains the Windows preset and docs
you intend to test.

If the current Windows setup work has not been merged yet:

1. push a branch from the source machine first
2. clone that branch on the Windows laptop

If it has already been merged:

1. clone the intended default branch

---

## Step 1: Prepare the Windows Machine

Install or verify these first:

- Git
- Visual Studio 2022 Build Tools or Visual Studio 2022
- C++ desktop toolchain / MSVC
- Windows SDK
- CMake
- Ninja
- current NVIDIA driver
- CUDA Toolkit `12.4`

Recommended Visual Studio components:

- Desktop development with C++
- MSVC v143 toolset
- Windows 10 or Windows 11 SDK
- C++ CMake tools for Windows

Recommended Git setting:

```powershell
git config --global core.longpaths true
```

Reason:

- Crimson pulls in deep dependency trees
- Windows path-length issues are avoidable and not worth tripping over

If the laptop uses hybrid graphics:

- make sure you can force apps to the NVIDIA GPU from Windows graphics settings
  or the NVIDIA control panel

---

## Step 2: Pick a Clean Directory Layout

Keep paths short and predictable.

Suggested layout:

```text
C:\src\crimson
C:\third_party\opencv-4.10.0
C:\third_party\TensorRT-10.0.1.6
C:\third_party\ffmpeg-nvidia
```

Suggested meanings:

- `C:\src\crimson`
  - repo checkout
- `C:\third_party\opencv-4.10.0`
  - OpenCV install or build output root
- `C:\third_party\TensorRT-10.0.1.6`
  - TensorRT unpacked root
- `C:\third_party\ffmpeg-nvidia`
  - FFmpeg root for the Windows validation stack

---

## Step 3: Clone the Repo with Submodules

Crimson uses git submodules. Clone recursively.

If you are cloning a published branch directly:

```powershell
git clone --recursive <repo-url> C:\src\crimson
cd C:\src\crimson
git fetch origin
git switch <branch-name>
git submodule update --init --recursive
```

If the branch is already the default clone target for your test, this is enough:

```powershell
git clone --recursive <repo-url> C:\src\crimson
cd C:\src\crimson
git submodule update --init --recursive
```

Verify:

- `third_party/imgui`
- `third_party/implot`
- `third_party/ImGuiFileDialog`
- `third_party/IconFontCppHeaders`

all exist after clone.

---

## Step 4: Install or Unpack the Dependency Stack

### CUDA Toolkit

Install:

- CUDA Toolkit `12.4`

Verify:

```powershell
nvcc --version
```

Expected:

- CUDA toolkit `12.4`

### TensorRT

Install or unpack:

- TensorRT `10.0.1.6` for Windows

Verify that these exist under the TensorRT root:

- `include\NvInfer.h`
- `include\NvInferVersion.h`
- `lib\` or `lib\x64\`

### OpenCV

Install or build:

- OpenCV `4.10.0`

Important:

- `CRIMSON_OPENCV_DIR` must point to the directory containing
  `OpenCVConfig.cmake`
- `CRIMSON_FFMPEG_ROOT` must point to the FFmpeg install root containing
  `include/`, `lib/`, and `bin/`
- `CRIMSON_VIDEO_CODEC_SDK_ROOT` must point to the NVIDIA Video Codec SDK root
  containing `Interface/` and `Lib/x64/`
- the OpenCV build should include the modules Crimson actually uses
- the full `windows-trt10-cuda12.4` preset expects the OpenCV SFM module for
  triangulation support
- the `windows-trt10-cuda12.4-no-sfm` preset disables the SFM-backed
  triangulation path and does not require `opencv_sfm`

### FFmpeg

Prepare:

- the FFmpeg root intended for the Windows validation stack

At minimum, the root should provide headers and libraries in a structure that
matches the CMake hints Crimson uses.

If you build FFmpeg with Media Autobuild Suite, stage it into Crimson's
expected layout by running this from a Visual Studio developer shell:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\stage_windows_ffmpeg_nvidia.ps1 `
  -MediaAutobuildRoot C:\src\media-autobuild_suite\local64 `
  -OutputRoot C:\third_party\ffmpeg-nvidia
```

That script:

- verifies that the FFmpeg build exposes CUDA/NVENC
- copies headers and runtime DLLs into `C:\third_party\ffmpeg-nvidia`
- generates MSVC import libraries from the suite's `.def` files using `lib.exe`

### Driver Check

Verify the NVIDIA driver is functioning:

```powershell
nvidia-smi
```

Remember:

- `nvidia-smi` reports driver-side CUDA capability
- `nvcc --version` reports the toolkit Crimson compiles against

See:

- [docs/crimson_cuda_driver_toolkit_and_presets.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_cuda_driver_toolkit_and_presets.md)

---

## Step 5: Open a Visual Studio Developer Shell

Do not use a plain shell unless you already know the MSVC toolchain environment
is active.

Use one of:

- Developer PowerShell for VS 2022
- x64 Native Tools Command Prompt for VS 2022

Then verify:

```powershell
cmake --version
ninja --version
cl
```

---

## Step 6: Set the Dependency Root Variables

Example:

```powershell
$env:CRIMSON_CUDA_TOOLKIT_ROOT="C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.4"
$env:CRIMSON_OPENCV_DIR="C:/third_party/opencv-4.10.0/install/lib/cmake/opencv4"
$env:CRIMSON_FFMPEG_ROOT="C:/third_party/ffmpeg-nvidia"
$env:CRIMSON_VIDEO_CODEC_SDK_ROOT="C:/third_party/Video_Codec_SDK_13.0"
$env:CRIMSON_TENSORRT_ROOT="C:/third_party/TensorRT-10.0.1.6"
```

Alternative:

- store machine-local overrides in `CMakeUserPresets.json`

That file is ignored by git.

---

## Step 7: Run Preflight Checks

From the repo root:

```powershell
cmake --list-presets
nvidia-smi
nvcc --version
```

Confirm:

- the `windows-trt10-cuda12.4` preset exists
- the `windows-trt10-cuda12.4-no-sfm` preset exists
- `nvidia-smi` works
- `nvcc --version` reports CUDA `12.4`

Also record the values in:

- [docs/crimson_windows_trt10_cuda12.4_validation_record.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_trt10_cuda12.4_validation_record.md)

---

## Step 8: Configure

Run one of these:

```powershell
cmake --preset windows-trt10-cuda12.4
```

or, for a first 2D-only Windows bring-up:

```powershell
cmake --preset windows-trt10-cuda12.4-no-sfm
```

Expected:

- exact version checks pass for CUDA, OpenCV, and TensorRT
- configure completes

Note:

- first configure may need internet access if third-party dependencies are
  fetched during configure

If configure fails:

- capture the first real blocking error
- classify it as one of:
  - toolchain setup
  - path discovery
  - dependency version mismatch
  - third-party dependency fetch
  - Windows-specific compile system issue

---

## Step 9: Build

Release:

```powershell
cmake --build --preset build-windows-trt10-cuda12.4-release
```

2D-only fallback:

```powershell
cmake --build --preset build-windows-trt10-cuda12.4-no-sfm-release
```

Optional debug:

```powershell
cmake --build --preset build-windows-trt10-cuda12.4-debug
```

Record:

- first failing target
- whether the failure is in Crimson code or dependency bring-up

---

## Step 10: Stage an Install Tree

For the Windows multi-config build, include `--config Release`.

```powershell
cmake --install build/windows-trt10-cuda12.4 --config Release --prefix dist/Crimson
```

If you used the no-SFM preset, install from:

```powershell
cmake --install build/windows-trt10-cuda12.4-no-sfm --config Release --prefix dist/Crimson
```

Expected:

- install completes
- staged layout is produced under `dist/Crimson`

At this stage, you are testing the packaging direction as well as the build.

---

## Step 11: Run Runtime Smoke Checks

First run from the build output if needed, then from the staged install tree.

Minimum checks:

- app launches
- fonts load
- config loads
- a representative dataset opens
- decode path is exercised
- TensorRT inference path is exercised, if available
- if using the no-SFM preset, confirm the app runs as a 2D-only build and the
  Triangulate action is disabled

Important Windows observations:

- whether the app is using the NVIDIA GPU
- whether launch fails due to missing DLLs
- whether fonts/config/resource discovery work outside the repo root

---

## Step 12: Fill Out the Validation Record

As you go, record the results in:

- [docs/crimson_windows_trt10_cuda12.4_validation_record.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_trt10_cuda12.4_validation_record.md)

At minimum, capture:

- machine metadata
- dependency roots used
- configure result
- build result
- runtime smoke result
- install/staging result
- promotion decision
- blockers

---

## Step 13: Decide the Status

Use the promotion process:

- [docs/crimson_dependency_stack_promotion_process.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_dependency_stack_promotion_process.md)

Possible outcomes:

- remain `planned`
- promote to `baseline`
- downgrade to `experimental`

Do not promote the stack to `baseline` unless the checklist in the promotion
process is actually satisfied.

---

## Practical Recommendation

For the very first pass on the laptop:

1. get configure working
2. get release build working
3. get the app to launch
4. stage an install tree
5. record blockers instead of trying to solve every issue at once

The first goal is not "perfect Windows support". The first goal is a credible,
well-documented validation result.
