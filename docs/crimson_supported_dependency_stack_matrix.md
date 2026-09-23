# Crimson Supported Dependency Stack Matrix

Purpose: define which CUDA, TensorRT, OpenCV, and related dependency stacks are
currently treated as baseline, planned, experimental, or legacy for Crimson.

Date anchored: 2026-04-02.

---

## Why This Exists

Crimson depends on a tightly coupled GPU software stack:

- CUDA toolkit
- TensorRT
- OpenCV
- FFmpeg/NVIDIA codec support

Because these components are version-sensitive, "supports CUDA" is not a useful
statement by itself. Crimson needs an explicit stack policy.

This document is the source of truth for that policy.

The promotion rules for moving stacks between statuses live in
[docs/crimson_dependency_stack_promotion_process.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_dependency_stack_promotion_process.md).

---

## Status Meanings

- `baseline`
  - pinned in shared presets and expected for routine development
- `planned`
  - the intended next target stack, but not yet validated end to end
- `experimental`
  - worth exploring, but not supported and not pinned in shared presets
- `legacy`
  - historical repo guidance may mention it, but it is not the maintained path

---

## Current Matrix

| Stack ID | OS | CUDA Toolkit | TensorRT | OpenCV | FFmpeg / Codec Path | Status | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `linux-trt10-cuda12.4-release` | Linux | `12.4` | `10.0.1.6` | `4.10.0` | custom FFmpeg root via `FFMPEG_ROOT` | `baseline` | current pinned Linux stack in shared presets |
| `linux-trt10-cuda12.4-debug` | Linux | `12.4` | `10.0.1.6` | `4.10.0` | custom FFmpeg root via `FFMPEG_ROOT` | `baseline` | same stack as release, debug build mode |
| `windows-trt10-cuda12.4` | Windows | `12.4` | `10.0.1.6` | `4.10.0` | Windows FFmpeg/NVIDIA codec stack still to be staged | `planned` | first intended Windows target stack; validation record in [docs/crimson_windows_trt10_cuda12.4_validation_record.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_trt10_cuda12.4_validation_record.md) |
| `cuda13.x-*` | Linux / Windows | `13.x` | `TBD` | `TBD` | `TBD` | `experimental` | do not infer support from `nvidia-smi` alone |
| `macos-viewer-*` | macOS | none | none | `TBD` | VideoToolbox/Metal/CPU TBD | `experimental` | not a port of the CUDA stack; first plausible target is viewer-only, see [docs/crimson_ubuntu_macos_platform_strategy.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_ubuntu_macos_platform_strategy.md) |
| historical README path | Linux | `12.0` | `8.6.1.6` | `4.8.0` | manual local install | `legacy` | older instructions exist, but this is not the current maintained preset stack |

---

## Current Policy

### Baseline Stack

The current baseline Crimson stack is:

- CUDA `12.4`
- TensorRT `10.0.1.6`
- OpenCV `4.10.0`

This is the stack family currently encoded in shared presets:

- [CMakePresets.json](/home/delahantyj@hhmi.org/gitrepos/crimson/CMakePresets.json)

And enforced by exact version checks in:

- [CMakeLists.txt](/home/delahantyj@hhmi.org/gitrepos/crimson/CMakeLists.txt#L38)

### Planned Windows Stack

The Windows port should target the same baseline dependency family first:

- CUDA `12.4`
- TensorRT `10.0.1.6`
- OpenCV `4.10.0`

Reason:

- it reduces simultaneous variables while the Windows bring-up is still
  incomplete
- it keeps Linux and Windows aligned on the same intended inference/runtime
  stack

The Windows preset expresses this intent, but it should not be treated as fully
validated yet.

The first validation record for that effort lives in:

- [docs/crimson_windows_trt10_cuda12.4_validation_record.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_trt10_cuda12.4_validation_record.md)

### Experimental Stacks

New toolkit lines such as CUDA `13.x` should be treated as experimental until
all of the following are verified:

- CMake configure succeeds
- CUDA compile succeeds
- TensorRT compatibility is confirmed
- OpenCV compatibility is confirmed
- Crimson runtime behavior is checked

Until then:

- do not call the stack supported
- do not rename the baseline presets
- do not infer support from driver capability alone

Promotion and downgrade decisions should follow:

- [docs/crimson_dependency_stack_promotion_process.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_dependency_stack_promotion_process.md)

macOS is also experimental, but for a different reason. It is not a CUDA stack
variant. A macOS build would need a no-CUDA viewer architecture first, then
separate Metal/VideoToolbox and non-TensorRT inference work. See:

- [docs/crimson_ubuntu_macos_platform_strategy.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_ubuntu_macos_platform_strategy.md)

### Legacy Stacks

Older manual setup notes may remain useful as migration history, but they should
not be treated as the current engineering target.

Specifically, the older README path built around:

- CUDA `12.0`
- TensorRT `8.6.1.6`
- OpenCV `4.8.0`

should be treated as historical guidance unless it is revalidated and added back
as an explicit stack.

---

## Rules for Shared Presets

Only add a stack to shared `CMakePresets.json` if it is one of:

- `baseline`
- `planned`

And use these rules:

1. `baseline` stacks should have exact version checks and should be the default
   engineering path.
2. `planned` stacks may exist before full runtime validation, but the docs must
   say that clearly.
3. `experimental` stacks should stay out of shared presets unless there is a
   concrete team decision to incubate them there.
4. machine-local path differences belong in `CMakeUserPresets.json` or
   environment variables, not in the shared preset names.

Promotion into or out of these statuses should follow the documented process:

- [docs/crimson_dependency_stack_promotion_process.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_dependency_stack_promotion_process.md)

---

## Rules for Communication

When talking about support, prefer this language:

- "Crimson baseline stack"
- "planned Windows target stack"
- "experimental CUDA 13.x stack"
- "legacy manual Linux stack"

Avoid vague statements like:

- "Crimson supports CUDA 13"
- "the machine has CUDA 13 so it should work"

Those statements collapse driver capability and build/runtime stack into one
number and cause confusion.

---

## Next Update Trigger

Update this matrix whenever any of these change:

- shared preset names
- pinned CUDA version
- pinned TensorRT version
- pinned OpenCV version
- Windows target stack status
- an experimental stack becomes baseline or planned
