# Crimson Windows TRT10 / CUDA 12.4 Validation Record

Purpose: capture the first structured validation record for the planned Windows
stack:

- CUDA `12.4`
- TensorRT `10.0.1.6`
- OpenCV `4.10.0`

Date anchored: 2026-04-02.

Status at creation: `planned`.

Related policy:

- [docs/crimson_supported_dependency_stack_matrix.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_supported_dependency_stack_matrix.md)
- [docs/crimson_dependency_stack_promotion_process.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_dependency_stack_promotion_process.md)
- [docs/crimson_windows_first_validation_guide.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_first_validation_guide.md)

---

## Is a Windows Laptop with an NVIDIA GPU a Good Test Machine?

Yes. A Windows laptop with a real NVIDIA GPU is a good first validation machine
for this stack.

Why it is good:

- it exercises the real Windows toolchain and runtime path
- it tests NVIDIA driver/runtime behavior on an end-user style machine
- it is enough to produce the first `planned` -> `baseline` evidence record

Limits:

- one laptop is enough for the first validation pass, but not enough to prove
  broad Windows robustness by itself
- laptops can introduce hybrid-GPU issues, thermal throttling, and driver/vendor
  quirks that a desktop may not show

Recommended minimum characteristics:

- Windows 11 or a current Windows 10 build
- dedicated NVIDIA GPU
- current NVIDIA driver that works with the chosen CUDA/TensorRT stack
- enough free disk for CUDA, TensorRT, OpenCV, FFmpeg, and build artifacts
- Visual Studio 2022 Build Tools or equivalent MSVC environment
- ability to force the app onto the NVIDIA GPU if the laptop uses hybrid
  graphics

If the laptop is all you have right now, it is still the right place to start.

---

## Validation Scope

This record is intended to collect the evidence needed for:

- Gate B: `planned` -> `baseline`

for the Windows target stack, as defined in:

- [docs/crimson_dependency_stack_promotion_process.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_dependency_stack_promotion_process.md#L123)

---

## Validation Metadata

Fill this in on the Windows machine when you run the validation:

| Field | Value |
| --- | --- |
| Validation date | |
| Operator | |
| Machine name | |
| OS version | |
| Laptop model | |
| GPU model | |
| NVIDIA driver version | |
| `nvidia-smi` CUDA capability | |
| `nvcc --version` toolkit version | |
| Visual Studio / MSVC version | |
| CMake version | |
| Generator used | |
| Preset used | `windows-trt10-cuda12.4` |

If you are validating the first 2D-only Windows bring-up, record
`windows-trt10-cuda12.4-no-sfm` instead.

---

## Dependency Roots Used

Record the exact paths used during configure:

| Variable | Value |
| --- | --- |
| `CRIMSON_CUDA_TOOLKIT_ROOT` | |
| `CRIMSON_OPENCV_DIR` | |
| `CRIMSON_FFMPEG_ROOT` | |
| `CRIMSON_VIDEO_CODEC_SDK_ROOT` | |
| `CRIMSON_TENSORRT_ROOT` | |

If local overrides came from `CMakeUserPresets.json`, note that here too.

---

## Preflight Checklist

Check each item before trying the full build:

- [ ] `nvidia-smi` runs successfully
- [ ] `nvcc --version` reports CUDA `12.4`
- [ ] OpenCV `4.10.0` CMake package path is available
- [ ] if using the full preset, OpenCV SFM support is available
- [ ] TensorRT `10.0.1.6` headers and libs are available
- [ ] FFmpeg root for the intended Windows stack is available
- [ ] Visual Studio developer shell or equivalent MSVC environment is active
- [ ] `cmake --list-presets` works in the repo

---

## Configure Result

Command:

```powershell
cmake --preset windows-trt10-cuda12.4
```

Record:

- Result: `pass` / `fail`
- Notes:

Expected signals:

- exact version checks pass for CUDA, OpenCV, and TensorRT
- configure completes successfully

If it fails, capture:

- the first blocking error
- whether the issue is path discovery, compiler setup, dependency mismatch, or
  network/dependency-fetch behavior

---

## Build Result

Command:

```powershell
cmake --build --preset build-windows-trt10-cuda12.4-release
```

Optional debug build:

```powershell
cmake --build --preset build-windows-trt10-cuda12.4-debug
```

Record:

- Release build: `pass` / `fail`
- Debug build: `pass` / `fail` / `not attempted`
- Notes:

Capture:

- first failing target, if any
- whether failures are in Crimson code or third-party dependency bring-up
- any Windows-specific compile/link issues

---

## Runtime Smoke Result

Check the minimum runtime path:

- [ ] app launches
- [ ] fonts load
- [ ] config loads from expected path
- [ ] representative dataset opens
- [ ] video decode path is exercised
- [ ] TensorRT inference path is exercised, if available

Record:

- Runtime smoke: `pass` / `fail` / `partial`
- Notes:

Important Windows-specific observations:

- did the app use the NVIDIA GPU
- were any DLLs missing at launch
- if using `tools/set_windows_dependency_roots.ps1`, did launch work without any
  additional manual `PATH` edits
- did resource discovery work from the staged or build layout
- if using the no-SFM preset, was triangulation correctly disabled

---

## Install / Staging Result

Suggested command shape:

```powershell
cmake --install build/windows-trt10-cuda12.4 --config Release --prefix dist/Crimson
```

Check:

- [ ] install step completes
- [ ] staged app launches from `dist/Crimson/redgui.exe`
- [ ] runtime libraries are found from the staged layout

Record:

- Install/staging: `pass` / `fail` / `partial`
- Notes:

Suggested note for the first successful Windows staging pass:

- the first validated staged launch used `tools/set_windows_dependency_roots.ps1`
  in the same PowerShell session

---

## Promotion Decision

Choose one:

- [ ] remain `planned`
- [ ] promote to `baseline`
- [ ] downgrade to `experimental`

Reason:

---

## Remaining Blockers

List anything still preventing promotion:

- build blockers
- runtime blockers
- missing packaging pieces
- unsupported features
- machine-specific concerns

---

## Evidence Summary

Use this compact summary when copying the result into a PR or issue comment:

```text
Stack ID:
OS:
Machine:
Configure:
Build:
Runtime smoke:
Install/staging:
Promotion decision:
Remaining caveats:
Date:
```
