# Crimson Windows Installation Procedures

Purpose: collect the current Windows install, source-build, and publish
procedures in one operator-facing document.

Date anchored: 2026-07-06.

Related docs:

- [docs/crimson_windows_first_validation_guide.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_windows_first_validation_guide.md)
- [docs/crimson_windows_app_drop_reintroduction_plan.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_windows_app_drop_reintroduction_plan.md)
- [docs/crimson_supported_dependency_stack_matrix.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_supported_dependency_stack_matrix.md)
- [tools/windows_app_README.txt](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/tools/windows_app_README.txt)

## Current Model

Crimson has two Windows workflows:

1. Run-only app drop for users.
2. Source-build and publish workflow for developers/builders.

Do not mix them. A run-only user should not install Visual Studio, CUDA
Toolkit, TensorRT SDK, OpenCV, vcpkg, Python, or NASM just to launch Crimson.
Those are build-machine inputs. A published app drop should include
`redgui.exe`, helper scripts, resources, and runtime DLLs.

Current supported source-build stack:

```text
Windows x64 + MSVC 2022 + CUDA 12.4 + TensorRT 10.0.1.6 + OpenCV 4.10.0
```

The first Windows build target should normally use:

```text
windows-trt10-cuda12.4-no-sfm
```

That disables OpenCV SFM-backed triangulation while validating the rest of the
Windows stack.

## Run-Only User Install

Use this when someone receives a staged Crimson app drop.

Expected app-drop layout:

```text
Crimson\
  bin\
    redgui.exe
    required runtime DLLs
  share\
    crimson\
      fonts\
      config\
  install_crimson.ps1
  install_crimson.cmd
  check_crimson_runtime.ps1
  check_crimson_runtime.cmd
  set_crimson_cuda_device.ps1
  set_crimson_cuda_device.cmd
  README.txt
  release.json
```

Install:

```powershell
.\install_crimson.cmd
```

or:

```powershell
powershell -ExecutionPolicy Bypass -File .\install_crimson.ps1
```

Replace an existing install:

```powershell
powershell -ExecutionPolicy Bypass -File .\install_crimson.ps1 -ReplaceExisting
```

Optional desktop shortcut:

```powershell
powershell -ExecutionPolicy Bypass -File .\install_crimson.ps1 -CreateDesktopShortcut
```

Default install location:

```text
%LOCALAPPDATA%\Crimson
```

Runtime check:

```powershell
powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\Crimson\check_crimson_runtime.ps1"
```

Launch:

```powershell
& "$env:LOCALAPPDATA\Crimson\bin\redgui.exe"
```

Optional CUDA GPU preference:

```powershell
powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\Crimson\set_crimson_cuda_device.ps1"
```

Run-only machine prerequisites:

- supported Windows x64 machine
- compatible NVIDIA GPU
- compatible NVIDIA display driver

Run-only users should not need the CUDA Toolkit. Check the driver with:

```powershell
nvidia-smi
```

## Source-Build Machine Setup

Use this when building Crimson from source or publishing an app drop.

Recommended layout:

```text
C:\src\crimson
C:\src\vcpkg
C:\third_party\opencv-install-4.10.0-x64
C:\third_party\ffmpeg-nvidia
C:\third_party\Video_Codec_SDK_13.0.19
C:\third_party\TensorRT-10.0.1.6
```

Required tools:

- Git
- Visual Studio 2022 Build Tools or Visual Studio 2022
- MSVC v143 C++ toolset
- Windows 10 or Windows 11 SDK
- CMake
- Ninja
- Python 3
- NASM
- vcpkg
- current NVIDIA driver
- CUDA Toolkit `12.4`

Use an x64 developer shell:

```text
x64 Native Tools Command Prompt for VS 2022
```

or:

```text
Developer PowerShell for VS 2022
```

Verify:

```powershell
cl
cmake --version
ninja --version
nvcc --version
```

Python is needed by TensorStore during CMake configure. Verify either:

```powershell
python --version
```

or:

```powershell
py -3 --version
```

NASM is also needed by TensorStore during configure/build. Verify:

```powershell
nasm -v
```

If winget installed NASM under the user profile, this path may exist even when
`nasm` is not on `PATH`:

```powershell
Test-Path "$env:LOCALAPPDATA\bin\NASM\nasm.exe"
& "$env:LOCALAPPDATA\bin\NASM\nasm.exe" -v
```

The build helper probes this location directly.

## Build Dependencies

### CUDA Toolkit

Install CUDA Toolkit `12.4`. This is a build prerequisite because CMake needs
`nvcc.exe` and CUDA development libraries.

Do not silently replace a user's NVIDIA display driver. Use custom install and
deselect `Display.Driver`, or use a silent toolkit-only install that omits
`Display.Driver`.

Verify:

```powershell
nvcc --version
```

### Dependency Archives

OpenCV, FFmpeg, Video Codec SDK, and TensorRT should be unpacked under
`C:\third_party` using the supported stack versions.

The current helper defaults are:

```text
C:\third_party\opencv-install-4.10.0-x64
C:\third_party\ffmpeg-nvidia
C:\third_party\Video_Codec_SDK_13.0.19
C:\third_party\TensorRT-10.0.1.6
```

If local paths differ, pass overrides to `tools\build_windows_app_drop.ps1`.

For source builds, `C:\third_party\ffmpeg-nvidia` must be a development layout,
not only a runtime `bin` folder. It must contain:

```text
C:\third_party\ffmpeg-nvidia\include\libavformat\avformat.h
C:\third_party\ffmpeg-nvidia\lib\avformat.lib
C:\third_party\ffmpeg-nvidia\lib\avcodec.lib
C:\third_party\ffmpeg-nvidia\lib\avutil.lib
C:\third_party\ffmpeg-nvidia\lib\swscale.lib
C:\third_party\ffmpeg-nvidia\lib\swresample.lib
```

The FFmpeg root is the directory that directly contains `bin`, `include`, and
`lib`. Some dependency archives unzip with an extra nested directory. This is
wrong for the default helper path:

```text
C:\third_party\ffmpeg-nvidia\ffmpeg-nvidia\bin
C:\third_party\ffmpeg-nvidia\ffmpeg-nvidia\include
C:\third_party\ffmpeg-nvidia\ffmpeg-nvidia\lib
```

Flatten it to:

```text
C:\third_party\ffmpeg-nvidia\bin
C:\third_party\ffmpeg-nvidia\include
C:\third_party\ffmpeg-nvidia\lib
```

Alternatively, pass the nested root explicitly:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_windows_app_drop.ps1 `
  -FfmpegRoot "C:\third_party\ffmpeg-nvidia\ffmpeg-nvidia" `
  -CleanInstall
```

If using Media Autobuild Suite, stage that layout from an x64 Visual Studio
developer shell:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\stage_windows_ffmpeg_nvidia.ps1 `
  -MediaAutobuildRoot C:\src\media-autobuild_suite\local64 `
  -OutputRoot C:\third_party\ffmpeg-nvidia `
  -CleanOutput
```

Check the FFmpeg/NVIDIA codec build paths without running a full build:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\check_windows_build_dependency_paths.ps1
```

or:

```cmd
tools\check_windows_build_dependency_paths.cmd
```

To also check whether the Media Autobuild Suite source layout is available:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\check_windows_build_dependency_paths.ps1 `
  -CheckMediaAutobuild
```

### vcpkg

Install Crimson's current vcpkg package set:

```powershell
cd C:\src\crimson
powershell -ExecutionPolicy Bypass -File .\tools\setup_windows_vcpkg.ps1
```

This installs:

- `glew:x64-windows`
- `glfw3:x64-windows`
- `zlib:x64-windows`
- `hdf5[cpp]:x64-windows`

If vcpkg lives somewhere else:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\setup_windows_vcpkg.ps1 `
  -VcpkgRoot "D:\src\vcpkg"
```

## Build A Staged App Drop

From an x64 Visual Studio developer shell:

```powershell
cd C:\src\crimson
git pull

powershell -ExecutionPolicy Bypass -File .\tools\build_windows_app_drop.ps1 -CleanInstall
```

The helper:

- updates submodules
- sets dependency roots
- verifies CMake, Git, Python, and NASM
- validates required dependency roots
- validates FFmpeg headers/import libraries and NVIDIA codec import libraries
- passes vcpkg, Python, and NASM paths to CMake
- configures `windows-trt10-cuda12.4-no-sfm`
- uses a short default build tree, currently `build\w124n`, to avoid
  TensorStore-generated Windows object path limits
- builds Release
- installs to `dist\Crimson`
- runs `dist\Crimson\check_crimson_runtime.ps1`

Useful overrides:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_windows_app_drop.ps1 `
  -BuildDir "C:\src\crimson\build\w124n" `
  -ThirdPartyRoot "D:\third_party" `
  -VcpkgRoot "D:\src\vcpkg" `
  -Python3Executable "C:\Path\To\python.exe" `
  -NasmExecutable "C:\Path\To\nasm.exe" `
  -CleanInstall
```

If a configure failure used stale cache entries, clear the build directory:

```powershell
Remove-Item -Recurse -Force .\build\w124n -ErrorAction SilentlyContinue
```

If CMake fails during generation with `CMAKE_OBJECT_PATH_MAX` warnings from
TensorStore, protobuf, or `lpm-build`, the path to the build tree is too long.
Use a short build directory:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_windows_app_drop.ps1 `
  -BuildDir "C:\src\crimson\build\w124n" `
  -CleanInstall
```

Launch the staged app:

```powershell
& .\dist\Crimson\bin\redgui.exe
```

## Publish An App Drop

After a successful staged install:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\publish_windows_app_drop.ps1 `
  -StageRoot .\dist\Crimson `
  -ShareRoot "\\SERVER\crimson\windows-app" `
  -PublishCurrent
```

The publisher validates the app-drop layout, writes release metadata, and can
refresh the `current` drop. When `-ReleaseName` is omitted, the publisher uses
an automatic name like `YYYY-MM-DD_HHMMSS_<short-commit>` when Git metadata is
available, falling back to `YYYY-MM-DD_HHMMSS` otherwise.

To build, run the staged runtime check, and publish in one command:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_check_publish_windows_app_drop.ps1 `
  -ShareRoot "\\SERVER\crimson\windows-app" `
  -PublishCurrent `
  -ArchiveExistingCurrent `
  -RequireNvidiaSmi
```

The chain script forwards the common source-build options from
`build_windows_app_drop.ps1`, runs `check_crimson_runtime.ps1` against the
staged app, and then calls `publish_windows_app_drop.ps1`. Pass `-CleanInstall`
when you want to remove and recreate `dist\Crimson` before staging.
The staged runtime check may warn that `release.json` and `install_metadata.json`
are missing; `release.json` is created by the publish step, and
`install_metadata.json` is created by the user install step.

## Current Script Inventory

| Script | Audience | Purpose |
| --- | --- | --- |
| `tools\build_windows_app_drop.ps1` | builder | configure, build, install, and runtime-check a staged app drop |
| `tools\build_check_publish_windows_app_drop.ps1` | publisher | build, check, and publish a staged app drop in one command |
| `tools\check_windows_build_dependency_paths.ps1` | builder | check FFmpeg development files and NVIDIA codec import-library paths |
| `tools\setup_windows_vcpkg.ps1` | builder | clone/bootstrap vcpkg and install Crimson's vcpkg packages |
| `tools\set_windows_dependency_roots.ps1` | builder | set `CRIMSON_*` roots and prepend runtime DLL paths in the current shell |
| `tools\publish_windows_app_drop.ps1` | publisher | copy a staged app drop to a release/current share with metadata |
| `tools\install_crimson.ps1` | run-only user | copy an app drop to `%LOCALAPPDATA%\Crimson` |
| `tools\check_crimson_runtime.ps1` | user/builder | validate installed/staged app layout and runtime surface |
| `tools\set_crimson_cuda_device.ps1` | user | write or clear preferred CUDA GPU config |

## Troubleshooting Checklist

PowerShell script execution disabled:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
```

Wrong developer shell:

- use x64, not x86
- verify `cl` reports an x64 compiler

CUDA confusion:

- `nvidia-smi` reports driver-side CUDA capability
- `nvcc --version` reports the installed CUDA Toolkit
- Crimson's source build needs CUDA Toolkit `12.4`

Python Store alias:

- `python.exe` under `WindowsApps` may be only a Microsoft Store alias
- the helper skips non-working aliases and tries `py -3`
- explicit override: `-Python3Executable "C:\Path\To\python.exe"`

NASM not on `PATH` after winget:

- check `%LOCALAPPDATA%\bin\NASM\nasm.exe`
- the helper probes that location
- explicit override: `-NasmExecutable "C:\Path\To\nasm.exe"`

OpenCV config not found:

- `OpenCV_DIR` must contain `OpenCVConfig.cmake`
- the helper searches common nested archive layouts

FFmpeg headers or import libraries not found:

- `FFMPEG_ROOT` must directly contain `bin`, `include`, and `lib`
- avoid nested extraction such as `ffmpeg-nvidia\ffmpeg-nvidia\include`
- run `tools\check_windows_build_dependency_paths.cmd`

vcpkg package errors:

- rerun `tools\setup_windows_vcpkg.ps1`
- current package set includes GLEW, GLFW, zlib, and HDF5 C++
- clear the CMake build directory after package changes

Black window then close:

- run `check_crimson_runtime.ps1`
- inspect `%LOCALAPPDATA%\Crimson\CrashDumps`
- check for missing DLLs beside `bin\redgui.exe`

## Adequacy Assessment

The current procedures are documented enough for the active Windows source-build
bring-up and run-only app-drop model, provided this document is treated as the
entry point.

Remaining documentation or automation gaps:

- There is no monolith helper yet that stages all internal dependency archives
  from a share into `C:\third_party`.
- There is no standalone Windows prerequisite audit script in this branch; the
  build helper performs the most important checks inline.
- The first fully successful Windows build/install/publish run should update
  [docs/crimson_windows_trt10_cuda12.4_validation_record.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_windows_trt10_cuda12.4_validation_record.md)
  with exact evidence.
