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

### Low-VRAM Playback Findings

Record any playback-specific observations here, especially on laptop GPUs.

Suggested fields:

- main raw-video buffer mode and size
- stimulus decode backend
- stimulus buffer mode and size
- whether `Current Playback Speed` stayed at `1.0x` or settled lower
- whether target stimulus frame progression outran latest decoded or last
  displayed stimulus frame

Observed evidence from the first validated Windows laptop:

- the large raw camera stream was most stable with main `CPU Buffer`
- a small, high-FPS `H.264 Main` stimulus MP4 decoded better with `Stimulus
  Software Decode` than with `Stimulus GPU Decode`
- benchmark evidence on that machine:
  - software RGBA decode path: about `11048 fps`
  - CUDA decode + download + RGBA conversion path: about `3699 fps`
  - software decode was about `3x` faster for that stimulus stream

Later playback profiling on the same Windows RTX A1000 laptop found:

- VSync/compositor pacing was part of the apparent draw cost, but turning
  VSync off did not by itself restore stable `1.0x` playback
- after adding deeper decoder timing, the main camera showed:
  - `camera_decode_demux_ms` small
  - `camera_decode_submit_ms` near or above the frame budget
- that means the laptop is paying both:
  - render cost near the frame budget
  - and hardware decode submit cost near the frame budget

Main-camera `GPU Buffer` size also proved to be a real tradeoff:

- a very large ring (`100`) gave the decoder much more runway and produced much
  better playback stability than a small ring
- a small ring (`8`) made playback worse on that laptop, with larger decode
  gaps and occasional decoder wait spikes

Interpretation:

- the large ring trades a lot of VRAM for decoder/runway stability
- the small ring reduces VRAM pressure but can expose the laptop's combined
  decode/render bottleneck
- on this specific machine, software main-camera decode became a justified
  follow-up experiment after these measurements

Outcome of that follow-up experiment:

- the software main-camera decode path was decisively worse than `NVDEC` on the
  Windows RTX A1000 laptop
- under comparable playback settings, the software-decode run fell to roughly
  `0.30x` playback speed, while the GPU-decode run stayed near `0.89x`
- the software-decode run also showed:
  - `camera_decode_submit_ms` around `44 ms`
  - `camera_decode_convert_ms` around `4.4 ms`
  - `camera_decode_write_ms` around `9-10 ms`
  - much larger decode gaps
- conclusion: keep main-camera `GPU Decode` as the recommended path on this
  machine and treat the software main-camera backend as a rejected experiment,
  not a new default

Discrete-GPU / OpenGL forcing follow-up:

- the app was also tested after explicitly forcing the Windows Graphics
  preference to `High performance` and setting the NVIDIA Control Panel's
  OpenGL rendering GPU to the discrete NVIDIA GPU
- this did not produce a material playback improvement in the comparable
  `GPU Buffer` / lightweight-renderer run
- observed result stayed roughly the same:
  - playback remained about `0.88x`
  - `gl_draw_ms` stayed near the frame budget
  - `camera_decode_submit_ms` stayed near the frame budget
- conclusion: discrete-GPU forcing was worth verifying, but it did not reveal a
  hidden "wrong GPU" issue or unlock stable `1.0x` playback on this machine

Large-main-camera playback conclusion:

- after the renderer, upload, stimulus, VSync, and decode-backend experiments,
  the best Windows RTX A1000 laptop runs still remained below stable `1.0x`
  playback for the `4512x4512 @ 60 fps` main camera stream
- the remaining comparable bottlenecks stayed near the frame budget:
  - `camera_decode_submit_ms`
  - `gl_draw_ms`
- that means the limiting factor on this machine is no longer one obvious app
  bug; it is the combined decode + presentation workload for a very large
  `HEVC` stream on a low-tier mobile workstation GPU
- practical conclusion: on this class of laptop, full-fidelity playback of
  this stream should be expected to remain somewhat slower than real time
  unless playback fidelity is reduced or the hardware is stronger

Codec / re-encode interpretation:

- re-encoding the same `4512x4512 @ 60 fps` source to a different codec may
  reduce decode pressure somewhat, but it is unlikely to remove the whole
  bottleneck by itself
- the app-side evidence says rendering/presentation is still near the frame
  budget even after upload and several decode-path costs were reduced
- the isolated FFmpeg benchmark also suggested the current GPU path already has
  roughly real-time decode throughput once the same large frame is involved
- so the most promising re-encode, if one is tried later, is not merely "same
  size, different codec"; it is a lower-fidelity playback representation such
  as a downscaled proxy

Interpretation note:

- `Stimulus Decode Backend` and `Stimulus Buffer Mode` are independent
- software decode plus GPU buffer is a valid combination
- final display still uses GPU textures/PBOs even if the queue is CPU-backed
- if the UI stabilizes below `1.0x`, record whether the likely bottleneck is
  raw-camera decode, render/upload cost, or stimulus catch-up behavior

---

## Install / Staging Result

Suggested command shape:

```powershell
cmake --install build/windows-trt10-cuda12.4 --config Release --prefix dist/Crimson
```

Check:

- [ ] install step completes
- [ ] staged app launches from the staged install tree
- [ ] runtime libraries are found from the staged layout

Record:

- Install/staging: `pass` / `fail` / `partial`
- Notes:

Suggested note for the first successful Windows staging pass:

- the first validated staged launch used `tools/set_windows_dependency_roots.ps1`
  in the same PowerShell session
- the first observed staged launch path was `dist/Crimson/bin/redgui.exe`

Suggested follow-up note for the flattening work:

- re-test the Windows install tree in a clean `dist/Crimson` directory
- verify whether `redgui.exe` is expected at the install root or under `bin/`
- if flattening does not validate cleanly, keep the staged-path expectation as
  `dist/Crimson/bin/redgui.exe` for the first Windows release candidate

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
