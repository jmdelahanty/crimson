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

- [docs/crimson_windows_installation_procedures.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_windows_installation_procedures.md)
- [docs/crimson_windows_trt10_cuda12.4_validation_record.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_windows_trt10_cuda12.4_validation_record.md)
- [docs/crimson_supported_dependency_stack_matrix.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_supported_dependency_stack_matrix.md)
- [docs/crimson_dependency_stack_promotion_process.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_dependency_stack_promotion_process.md)

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
- Python 3
- NASM
- vcpkg
- current NVIDIA driver
- CUDA Toolkit `12.4`

Recommended Visual Studio components:

- Desktop development with C++
- MSVC v143 toolset
- Windows 10 or Windows 11 SDK
- C++ CMake tools for Windows

Python is needed during configure because TensorStore's CMake path uses Python
while generating its build files. It is not a run-only Crimson application
dependency. If Python is missing, install it with one of:

```powershell
winget install --id Python.Python.3.12 -e
```

or:

```powershell
winget install --id Python.Python.3.11 -e
```

Then open a fresh x64 Developer PowerShell and verify:

```powershell
python --version
```

If `python` is not on `PATH`, the build helper also tries the Windows `py -3`
launcher. You can verify that path with:

```powershell
py -3 --version
```

If Python is installed in a non-standard location, pass it explicitly:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_windows_app_drop.ps1 `
  -Python3Executable "C:\Path\To\python.exe" `
  -CleanInstall
```

NASM is needed during configure/build because TensorStore pulls in generated
third-party code that enables CMake's `ASM_NASM` language. It is not a run-only
Crimson application dependency.

Install options:

```powershell
winget search NASM
winget install --id NASM.NASM -e
```

If the winget package ID is unavailable on the machine, download the Windows
64-bit installer from the official NASM release page:

```text
https://www.nasm.us/pub/nasm/releasebuilds/3.02/win64/
```

Then open a fresh x64 Developer PowerShell and verify:

```powershell
nasm -v
```

On some machines, the winget NASM installer places `nasm.exe` under the user
profile but does not add it to `PATH`. Check:

```powershell
Test-Path "$env:LOCALAPPDATA\bin\NASM\nasm.exe"
& "$env:LOCALAPPDATA\bin\NASM\nasm.exe" -v
```

The Crimson build helper probes this location directly. You can also add it to
your user `PATH`:

```powershell
$nasmDir = "$env:LOCALAPPDATA\bin\NASM"
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if (($userPath -split ';') -notcontains $nasmDir) {
  [Environment]::SetEnvironmentVariable("Path", "$userPath;$nasmDir", "User")
}
```

Open a new terminal after changing `PATH`.

If NASM is installed in a non-standard location, pass it explicitly:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_windows_app_drop.ps1 `
  -NasmExecutable "C:\Path\To\nasm.exe" `
  -CleanInstall
```

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

### Supported Matrix

Do not validate Windows by trying every possible dependency combination. Pick
the intended supported stack and make failures explicit against that stack.

Current target:

```text
Windows x64 + MSVC 2022 + CUDA 12.4 + TensorRT 10.0.1.6 + OpenCV 4.10.0
```

Source-build machines need the full build stack above. Run-only user machines
should only need a supported Windows release, a compatible NVIDIA GPU/driver,
and the published Crimson app drop with its runtime DLLs bundled.

### vcpkg Packages

Crimson's Windows source build uses vcpkg for non-NVIDIA C/C++ dependencies
such as GLEW, GLFW, zlib, and HDF5. A missing `GLEW` configure error usually
means vcpkg is absent, the packages have not been installed, or CMake was not
given the vcpkg toolchain file. A missing `ZLIB` error during HDF5 discovery
usually means `zlib:x64-windows` is absent from the same vcpkg triplet.

Default source-build layout:

```text
C:\src\vcpkg
C:\src\vcpkg\installed\x64-windows\bin
```

Manual first-time setup:

```powershell
cd C:\src
git clone https://github.com/microsoft/vcpkg.git
cd C:\src\vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg.exe install glew:x64-windows glfw3:x64-windows zlib:x64-windows 'hdf5[cpp]:x64-windows'
```

Or use Crimson's helper from the Crimson repo root:

```powershell
cd C:\src\crimson
powershell -ExecutionPolicy Bypass -File .\tools\setup_windows_vcpkg.ps1
```

The helper clones `https://github.com/microsoft/vcpkg.git` into
`C:\src\vcpkg` if needed, bootstraps `vcpkg.exe`, and installs:

- `glew:x64-windows`
- `glfw3:x64-windows`
- `zlib:x64-windows`
- `hdf5[cpp]:x64-windows`

If vcpkg is stored elsewhere:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\setup_windows_vcpkg.ps1 `
  -VcpkgRoot "D:\src\vcpkg"
```

The one-command Crimson build helper passes
`C:\src\vcpkg\scripts\buildsystems\vcpkg.cmake` to CMake automatically when it
exists. If vcpkg is installed somewhere else, pass `-VcpkgRoot` and
`-VcpkgTriplet` to `tools\build_windows_app_drop.ps1`.

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

Build-from-source machines need the CUDA Toolkit because CMake must find
`nvcc.exe` and the CUDA development libraries. This is separate from the
NVIDIA display driver. Do not use the default/express CUDA installer path on a
user workstation if the goal is only to add the build toolkit.

Driver-preserving install options:

- in the graphical installer, choose a custom installation and deselect the
  NVIDIA display driver / `Display.Driver` component
- in silent mode, install only the toolkit subpackages Crimson needs and omit
  `Display.Driver`

Example silent install shape for CUDA `12.4`:

```powershell
.\cuda_12.4.0_551.61_windows.exe -s `
  nvcc_12.4 `
  cudart_12.4 `
  npp_12.4 `
  npp_dev_12.4 `
  nvml_dev_12.4 `
  visual_studio_integration_12.4 `
  -n
```

The important constraint is that `Display.Driver` is not listed. If the
existing NVIDIA driver is too old, update it as an explicit separate machine
maintenance step rather than as a hidden side effect of Crimson dependency
setup.

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
  containing `Interface/` and either `Lib/x64/` or `Lib/win/x64/`
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
- the `CUDA Version` printed by `nvidia-smi` is not proof that the CUDA Toolkit
  is installed
- a run-only Crimson app drop should need a compatible NVIDIA driver and the
  DLLs bundled with the app, not a full CUDA Toolkit install

See:

- [docs/crimson_cuda_driver_toolkit_and_presets.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_cuda_driver_toolkit_and_presets.md)
- NVIDIA CUDA 12.4 Windows install guide:
  `https://docs.nvidia.com/cuda/archive/12.4.0/cuda-installation-guide-microsoft-windows/index.html`

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
$env:CRIMSON_OPENCV_DIR="C:/third_party/opencv-install-4.10.0-x64"
$env:CRIMSON_FFMPEG_ROOT="C:/third_party/ffmpeg-nvidia"
$env:CRIMSON_VIDEO_CODEC_SDK_ROOT="C:/third_party/Video_Codec_SDK_13.0"
$env:CRIMSON_TENSORRT_ROOT="C:/third_party/TensorRT-10.0.1.6"
```

If you want to reload the same paths in one step each session, dot-source the
helper script from the repo root:

```powershell
. .\tools\set_windows_dependency_roots.ps1
```

If PowerShell reports that running scripts is disabled on this system, allow
scripts for only the current shell and rerun the helper:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
. .\tools\set_windows_dependency_roots.ps1
```

For a persistent per-user setting, use:

```powershell
Set-ExecutionPolicy -Scope CurrentUser -ExecutionPolicy RemoteSigned
```

The helper script also prepends the common runtime DLL directories to `PATH`
for the current PowerShell session so `redgui.exe` can be launched from the
same shell without an extra manual `PATH` edit.

Important:

- use the leading `. ` so the variables are set in the current shell
- override any default path by passing a named argument, for example:

```powershell
. .\tools\set_windows_dependency_roots.ps1 `
  -OpenCvDir "C:/third_party/opencv-install-4.10.0-x64"
```

Alternative:

- store machine-local overrides in `CMakeUserPresets.json`

That file is ignored by git.

---

## Fast Path: Build A Staged App Drop

From a Visual Studio Developer PowerShell in the repo root, the helper script
can run the normal no-SFM bring-up path end to end:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_windows_app_drop.ps1 -CleanInstall
```

That script:

- updates submodules
- loads dependency roots through `tools/set_windows_dependency_roots.ps1`
- configures `windows-trt10-cuda12.4-no-sfm`
- builds `Release`
- installs to `dist\Crimson`
- runs `dist\Crimson\check_crimson_runtime.ps1`

To launch after a successful build:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_windows_app_drop.ps1 -SkipConfigure -SkipBuild -SkipInstall -Launch
```

Or build and launch in one pass:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_windows_app_drop.ps1 -CleanInstall -Launch
```

Useful overrides:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_windows_app_drop.ps1 `
  -ThirdPartyRoot "D:\third_party" `
  -InstallPrefix "D:\CrimsonStage\Crimson" `
  -CleanInstall
```

If the fast path fails, continue with the manual steps below and report the
first failing section header plus the first real error block.

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

- [docs/crimson_windows_trt10_cuda12.4_validation_record.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_windows_trt10_cuda12.4_validation_record.md)

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

Suggested staged launch command:

```powershell
& .\dist\Crimson\bin\redgui.exe
```

If you already dot-sourced `tools/set_windows_dependency_roots.ps1` in the same
PowerShell session, the helper should have already prepended the common runtime
DLL directories to `PATH`.

Current observed result on the first validated Windows laptop:

- the staged app launched from `dist/Crimson/bin/redgui.exe`

Follow-up packaging work:

- verify the flattened Windows install layout in a clean staging directory
- confirm whether a fresh configure/install moves `redgui.exe` to
  `dist/Crimson/redgui.exe` or whether stale install artifacts are masking the
  change
- if flattening remains unreliable, decide whether to keep the `bin/` layout on
  Windows for the first packaged release

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

### Playback Tuning Note for Low-VRAM Laptops

The first validated Windows laptop showed a clear split between the large raw
camera stream and the smaller high-FPS stimulus stream.

Important distinction:

- `Buffer Type` / `Stimulus Buffer Type` control where decoded RGBA frames are
  queued after decode
- `Stimulus Decode Backend` controls how the compressed stimulus MP4 is decoded
- final display still uses OpenGL textures/PBOs, so some GPU memory is still
  required even when both ring buffers are CPU-backed

Observed benchmark on the validation laptop for a `344x344 @ 120 fps`
`H.264 Main` stimulus MP4:

- software RGBA decode path: about `11048 fps`
- CUDA decode + download + RGBA conversion path: about `3699 fps`
- result: software decode was about `3x` faster for that stimulus stream

Recommended first-pass settings on similar laptops:

- main raw video: `CPU Buffer`
- main buffer size: `8-16`
- stimulus decode backend: `Stimulus Software Decode`
- stimulus buffer mode: start with `Stimulus GPU Buffer`
- stimulus buffer size: `8-12`

Why `Current Playback Speed` may decay below `1.0x` during play:

- Crimson advances the requested play clock from wall time and the selected
  playback multiplier
- the displayed speed metric is then computed from actual displayed camera
  frame progress over wall time
- displayed camera frames are still clamped to the slowest decoded camera
  stream that is currently needed for rendering
- VSync is enabled, so missed render budgets reduce steady-state playback
  throughput
- large raw-frame uploads, overlays, and visible windows all add pressure

In practice, if playback falls from `1.0x` and stabilizes near something like
`0.83x`, that usually means the current decode/render workload can only sustain
about `83%` of real-time on that machine with the current layout and visible
views.

---

## Step 12: Fill Out the Validation Record

As you go, record the results in:

- [docs/crimson_windows_trt10_cuda12.4_validation_record.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_windows_trt10_cuda12.4_validation_record.md)

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

- [docs/crimson_dependency_stack_promotion_process.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_dependency_stack_promotion_process.md)

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
