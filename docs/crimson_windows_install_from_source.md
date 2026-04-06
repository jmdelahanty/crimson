# Crimson Windows Install From Source

Purpose: give a shorter, user-facing path for building and running Crimson on a
Windows machine from source.

Date anchored: 2026-04-04.

This document is for the current Windows source-build path. It is not a GUI
installer guide.

Related docs:

- [docs/crimson_windows_first_validation_guide.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_first_validation_guide.md)
- [docs/crimson_windows_internal_dependency_share_setup.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_internal_dependency_share_setup.md)
- [docs/crimson_windows_run_only.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_run_only.md)
- [tools/set_windows_dependency_roots.ps1](/home/delahantyj@hhmi.org/gitrepos/crimson/tools/set_windows_dependency_roots.ps1)
- [tools/stage_windows_ffmpeg_nvidia.ps1](/home/delahantyj@hhmi.org/gitrepos/crimson/tools/stage_windows_ffmpeg_nvidia.ps1)

---

## Recommended Scope

For the first successful Windows bring-up, prefer the 2D-only preset:

- `windows-trt10-cuda12.4-no-sfm`

That is the path validated most heavily so far.

Use the full preset only if you specifically need the SFM-backed triangulation
path and you already have a compatible OpenCV SFM build.

---

## What You Need

Install or unpack these first:

- Git
- Visual Studio 2022 or Visual Studio 2022 Build Tools
- Desktop development with C++
- Windows SDK
- CMake
- Ninja
- current NVIDIA driver
- CUDA Toolkit `12.4`
- TensorRT `10.0.1.6`
- OpenCV `4.10.0`
- NVIDIA Video Codec SDK `13.0`
- FFmpeg staged in Crimson's expected Windows layout

Recommended Git setting:

```powershell
git config --global core.longpaths true
```

If the laptop uses hybrid graphics, also make sure you can force `redgui.exe`
to the NVIDIA GPU from Windows graphics settings.

---

## Recommended Directory Layout

Keep paths short:

```text
C:\src\crimson
C:\third_party\opencv-install-4.10.0-x64
C:\third_party\TensorRT-10.0.1.6
C:\third_party\Video_Codec_SDK_13.0
C:\third_party\ffmpeg-nvidia
```

---

## 1. Clone The Repo

```powershell
git clone --recursive <repo-url> C:\src\crimson
cd C:\src\crimson
git submodule update --init --recursive
```

If you are working from a non-default branch, switch to it and rerun the
submodule update.

---

## 2. Prepare FFmpeg

If you already have a Windows FFmpeg root laid out as:

```text
<ffmpeg-root>\
  bin\
  include\
  lib\
```

you can skip this section.

If you built FFmpeg with Media Autobuild Suite, Crimson includes a helper to
stage it into that shape:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\stage_windows_ffmpeg_nvidia.ps1 `
  -MediaAutobuildRoot C:\src\media-autobuild_suite\local64 `
  -OutputRoot C:\third_party\ffmpeg-nvidia
```

Run that from a Visual Studio developer shell so `lib.exe` is available.

---

## Optional: Stage Local Dependency Archives

If you want to make Windows setup easier by distributing prebuilt dependency
archives inside your own team, Crimson now includes a local archive staging
helper:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\stage_windows_dependency_archives.ps1 `
  -DownloadRoot C:\third_party\downloads `
  -RequireManifest `
  -CleanDestination
```

It is designed for local `.zip` files you already have, not for downloading
from vendor sites. For deterministic team use, put a manifest named
`crimson-windows-deps.manifest.json` in the download root and pin each archive
with an exact filename and SHA-256 checksum.

Crimson includes a template you can copy and fill in:

```text
tools\crimson-windows-deps.manifest.example.json
```

Typical flow:

1. copy the example manifest into `C:\third_party\downloads\crimson-windows-deps.manifest.json`
2. fill in the exact archive filenames and SHA-256 values
3. place the matching archives in `C:\third_party\downloads`
4. run the staging helper with `-RequireManifest`

Expected staged destinations:

- `C:\third_party\opencv-install-4.10.0-x64`
- `C:\third_party\TensorRT-10.0.1.6`
- `C:\third_party\Video_Codec_SDK_13.0`
- `C:\third_party\ffmpeg-nvidia`

Use this only with archives you are actually allowed to redistribute. The
script does not attempt to download vendor packages for you.

---

## 3. Open The Right Shell

Use one of:

- `x64 Native Tools Command Prompt for VS 2022`
- `Developer PowerShell for VS 2022`

Then go to the repo:

```powershell
cd C:\src\crimson
```

---

## 4. Run The Prereq Check

Crimson now includes a Windows prereq checker:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\check_windows_prereqs.ps1
```

It verifies:

- Visual Studio 2022 / Build Tools with the C++ workload, using `vswhere`
  when available and standard `vcvars` paths as a fallback
- core build tools such as `git`, `cmake`, `ninja`, `cl`, `nvcc`
- repo layout and required submodules
- CUDA, OpenCV, FFmpeg, Video Codec SDK, and TensorRT roots
- the staged FFmpeg `include/`, `lib/`, and `bin/` layout Crimson expects

If your local dependency roots differ from the defaults, pass them explicitly:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\check_windows_prereqs.ps1 `
  -CudaToolkitRoot "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.4" `
  -OpenCvDir "C:/third_party/opencv-install-4.10.0-x64" `
  -FfmpegRoot "C:/third_party/ffmpeg-nvidia" `
  -VideoCodecSdkRoot "C:/third_party/Video_Codec_SDK_13.0" `
  -TensorRtRoot "C:/third_party/TensorRT-10.0.1.6"
```

---

## 5. Load Dependency Roots Into The Session

The fastest path is the helper script:

```powershell
. .\tools\set_windows_dependency_roots.ps1
```

The leading `. ` matters. It loads the variables into the current PowerShell
session and also prepends common runtime DLL directories to `PATH`.

If your local paths differ from the defaults, override them explicitly:

```powershell
. .\tools\set_windows_dependency_roots.ps1 `
  -CudaToolkitRoot "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.4" `
  -OpenCvDir "C:/third_party/opencv-install-4.10.0-x64" `
  -FfmpegRoot "C:/third_party/ffmpeg-nvidia" `
  -VideoCodecSdkRoot "C:/third_party/Video_Codec_SDK_13.0" `
  -TensorRtRoot "C:/third_party/TensorRT-10.0.1.6"
```

Quick checks:

```powershell
cmake --list-presets
nvidia-smi
nvcc --version
```

---

## 6. Configure

Recommended first configure:

```powershell
cmake --preset windows-trt10-cuda12.4-no-sfm
```

If you do need the full preset:

```powershell
cmake --preset windows-trt10-cuda12.4
```

---

## 7. Build

Recommended first build:

```powershell
cmake --build --preset build-windows-trt10-cuda12.4-no-sfm-release
```

Full build:

```powershell
cmake --build --preset build-windows-trt10-cuda12.4-release
```

Optional one-command smoke path:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\smoke_test_windows_build.ps1 `
  -LoadDependencyRoots `
  -RunInstall
```

That wrapper can run:

- dependency-root loading
- prereq check
- configure
- build
- install
- executable existence verification

---

## 8. Stage An Install Tree

If you want the staged install layout under `dist/Crimson`:

```powershell
cmake --install build/windows-trt10-cuda12.4-no-sfm --config Release --prefix dist/Crimson
```

Or for the full preset:

```powershell
cmake --install build/windows-trt10-cuda12.4 --config Release --prefix dist/Crimson
```

---

## 9. Launch Crimson

You can launch either from the build output or from the staged install tree.

From the build output:

```powershell
& .\release\Release\redgui.exe
```

From the staged install tree:

```powershell
& .\dist\Crimson\bin\redgui.exe
```

If you are launching from a fresh shell, dot-source
`tools/set_windows_dependency_roots.ps1` again first so the runtime DLL paths
are available in that session.

---

## 10. Open A Recording

The current recommended Windows recording layout is:

```text
<recording-root>\
  zarr\
    <archive>.zarr\
  cams\
    <camera-video>.mp4
  raw\
    <stimulus-video>.mp4
```

You can launch directly into a recording:

```powershell
& .\release\Release\redgui.exe --recording "C:\path\to\recording-root"
```

---

## 11. Practical Windows Notes

- If you run from `release\Release`, rebuilding is enough.
- If you run from `dist\Crimson\bin`, rebuild and reinstall after pulling new
  changes.
- If `redgui.exe` starts and exits immediately, the most common cause is that
  the dependency-root helper was not loaded in the current shell.
- On the validated Windows RTX A1000 laptop, forcing the app to the discrete
  NVIDIA GPU was still worth doing, but it did not materially change the final
  playback ceiling for a very large `4512x4512 @ 60 fps` main-camera stream.

---

## 12. Known Playback Caveat For Low-End Laptop GPUs

For the validated RTX A1000 laptop, the best current playback path is:

- main camera: `GPU Decode`
- main buffer: `GPU Buffer`
- playback renderer: `Lightweight Playback Renderer`
- stimulus: `Software Decode` + `GPU Buffer`

Even with that setup, a full-fidelity `4512x4512 @ 60 fps HEVC` main-camera
stream may still stay somewhat below stable real-time playback on that class of
machine.

That should be treated as a hardware/workload ceiling, not as a sign that the
Windows build is fundamentally broken.
