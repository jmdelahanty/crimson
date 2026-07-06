# Crimson Windows App Drop Reintroduction Plan

Purpose: document how to reintroduce the previous Windows run-only app-drop
workflow into the current Crimson monolith, and how to troubleshoot the older
Windows installer while that work is pending.

Date anchored: 2026-07-06.

Related docs:

- [docs/crimson_packaging_and_distribution_plan.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_packaging_and_distribution_plan.md)
- [docs/crimson_windows_first_validation_guide.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_windows_first_validation_guide.md)
- [docs/crimson_supported_dependency_stack_matrix.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_supported_dependency_stack_matrix.md)

## Initial Problem

The current monolith has Windows CMake presets and a basic install target, but
the initial inspection showed that it did not yet reproduce the older run-only
Windows app-drop workflow.

Initial monolith state found during the readonly comparison:

- `CMakePresets.json` defines Windows presets that read dependency roots from
  `CRIMSON_CUDA_TOOLKIT_ROOT`, `CRIMSON_OPENCV_DIR`,
  `CRIMSON_FFMPEG_ROOT`, `CRIMSON_TENSORRT_ROOT`, and
  `CRIMSON_VIDEO_CODEC_SDK_ROOT`.
- `CMakeLists.txt` installed `redgui` and shared resources, but on Windows it
  installed the executable at the install root instead of `bin/`.
- The install rules did not bundle runtime DLLs.
- The install rules did not install the old helper scripts:
  `install_crimson.ps1`, `check_crimson_runtime.ps1`, or
  `set_crimson_cuda_device.ps1`.
- The repo did not include the old Windows crash-dump helper.

The practical result was that the old Windows installer could install an app
folder, but Crimson did not have enough packaging infrastructure in this branch
to produce the next self-contained app drop.

## Port Status

Status on 2026-07-06:

- Windows CMake install layout now targets `bin/redgui.exe`.
- Windows install rules now install root helper scripts and `README.txt`.
- Windows install rules now run CMake runtime dependency scanning for `redgui`
  and install resolved DLLs beside the executable.
- Windows builds now link `Dbghelp`.
- `install_crimson.ps1/.cmd`, `check_crimson_runtime.ps1/.cmd`,
  `set_crimson_cuda_device.ps1/.cmd`, `windows_app_README.txt`, and
  `publish_windows_app_drop.ps1` have been added.
- Windows crash-dump support has been restored with dumps under
  `%LOCALAPPDATA%\Crimson\CrashDumps` by default.
- Crimson now reads `CRIMSON_CUDA_DEVICE_INDEX` and the saved
  `cuda_device.json` preference non-interactively at startup.

Not yet validated in this branch:

- Windows configure/build/install with `windows-trt10-cuda12.4-no-sfm`.
- Windows runtime dependency scan output on the actual dependency stack.
- Launching the staged app on the target Windows machine without manual `PATH`
  edits.
- The old interactive multi-GPU selection dialog; this slice only restores
  env/config based selection.

Shared-store observation from the readonly scan:

- Existing drops are under `/groups/ahrens/ahrenslab/crimson/windows-app/current`
  and `/groups/johnson/johnsonlab/jeremy/crimson/windows-app/current`.
- Those drops contain `install_crimson.ps1` but appear not to contain
  `check_crimson_runtime.ps1`; the checker is part of the new package surface.

## Important Distinction

There are two different Windows deliverables:

1. Dependency share for builders.
2. Run-only app drop for users.

The dependency share contains zip archives for OpenCV, TensorRT, FFmpeg, Video
Codec SDK, and related source-build inputs. Those archives are for building and
publishing Crimson. They are not the normal user install payload.

The run-only app drop should already contain:

```text
Crimson/
  bin/
    redgui.exe
    required runtime DLLs
  share/
    crimson/
      fonts/
      config/
  install_crimson.ps1
  check_crimson_runtime.ps1
  set_crimson_cuda_device.ps1
  README.txt
  release.json
```

For a run-only user, `install_crimson.ps1` should copy and validate this app
drop. It should not require Visual Studio, CUDA Toolkit, TensorRT, source-build
dependency archives, or `C:\third_party` if the app drop was published
correctly.

## Supported Matrix, Not Every Combination

Crimson should not try to produce or support a separate Windows build for every
possible mix of CUDA, TensorRT, OpenCV, NVIDIA driver, Windows release, and
Visual Studio toolchain. The practical model is a small supported matrix:

```text
Windows x64 + MSVC 2022 + CUDA 12.4 + TensorRT 10.0.1.6 + OpenCV 4.10.0
```

The build/publish machine owns that dependency stack. The staged app drop then
bundles the runtime DLLs that match that stack. A run-only user owns only the
stable machine prerequisites:

- supported Windows x64 release
- compatible NVIDIA GPU
- compatible NVIDIA display driver

This is why runtime DLL bundling is a release requirement. It moves dependency
precision to the publisher instead of asking each user machine to recreate the
source-build stack. NVIDIA driver compatibility still matters, but Crimson does
not need a build per driver version when the driver is new enough for the
packaged CUDA runtime.

## What The Old Installer Did

The older `install_crimson.ps1` was a copier and validator:

- It expected `SourceRoot\bin\redgui.exe`.
- It expected `SourceRoot\share\crimson\fonts`.
- It expected `SourceRoot\share\crimson\config`.
- It optionally ran `check_crimson_runtime.ps1` before and after copying.
- It copied the app drop to `%LOCALAPPDATA%\Crimson` by default.
- It optionally created a desktop shortcut and launched the app.
- It prompted for a preferred CUDA device and wrote
  `%LOCALAPPDATA%\Crimson\config\cuda_device.json`.

It did not download, install, or extract the third-party runtime dependencies.
If a copied install is missing DLLs, the app drop was incomplete or came from an
intermediate build folder instead of a staged install folder.

## Current Windows Failure Triage

Use these checks on the Windows machine before changing the app package.

### Confirm The Installed Layout

```powershell
$install = "$env:LOCALAPPDATA\Crimson"
Get-ChildItem $install
Get-ChildItem "$install\bin"
Test-Path "$install\bin\redgui.exe"
Test-Path "$install\share\crimson\fonts"
Test-Path "$install\share\crimson\config"
```

Expected:

- `redgui.exe` lives at `$env:LOCALAPPDATA\Crimson\bin\redgui.exe`.
- Runtime DLLs live beside it in `bin`.
- Fonts and config live under `share\crimson`.

If `redgui.exe` is at the install root, or if `bin` has few or no DLLs, the app
drop is not in the old expected run-only layout.

### Run The Runtime Checker

From PowerShell:

```powershell
$install = "$env:LOCALAPPDATA\Crimson"
powershell -ExecutionPolicy Bypass -File "$install\check_crimson_runtime.ps1"
```

If the script is missing, the app drop was not the old published run-only
payload. If it warns that `bin` has no DLLs, treat that as strong evidence that
runtime dependency bundling was skipped.

### Inspect Release And Install Metadata

```powershell
$install = "$env:LOCALAPPDATA\Crimson"
Get-Content "$install\release.json" -Raw
Get-Content "$install\install_metadata.json" -Raw
```

This should identify the source app drop and build timestamp. If the metadata is
missing, the installed folder may have been created from a manual copy or from a
partial package.

### Check The Crash-Dump Folder

The older crash handler created:

```text
%LOCALAPPDATA%\Crimson\CrashDumps
```

A folder with no `.dmp` file usually means the handler initialized, but no
normal unhandled Windows exception was captured. Common causes include:

- missing dependent DLL before normal app startup
- process abort or fast-fail path
- GPU driver/runtime initialization failure
- app launched from a partial install tree

That symptom does not prove the crash handler is broken.

### Check The GPU Driver

```powershell
nvidia-smi
```

If this fails, fix the driver/runtime environment first. Crimson may compile
against CUDA Toolkit `12.4`, but a run-only app primarily needs a compatible
NVIDIA driver and the DLLs bundled with the app drop.

Do not install the full CUDA Toolkit as a routine run-only troubleshooting
step. The toolkit is a build prerequisite because it provides `nvcc.exe`,
headers, import libraries, and development libraries. A published app drop
should carry the runtime DLLs it needs next to `redgui.exe`.

If a build-from-source machine does need CUDA Toolkit `12.4`, install it
without replacing the user's display driver unless the existing driver is too
old. In the graphical installer, use a custom install and deselect the driver.
In silent mode, list only toolkit subpackages and omit `Display.Driver`, for
example:

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

If the driver is too old for the target CUDA runtime, handle the driver update
as an explicit separate machine-maintenance step. NVIDIA documents
`Display.Driver` as a separate CUDA installer subpackage in the CUDA 12.4
Windows installation guide:
`https://docs.nvidia.com/cuda/archive/12.4.0/cuda-installation-guide-microsoft-windows/index.html`

### Identify Missing DLLs

From a Visual Studio Developer PowerShell:

```powershell
dumpbin /DEPENDENTS "$env:LOCALAPPDATA\Crimson\bin\redgui.exe"
```

If Visual Studio tools are not available, use a dependency-inspection tool on
`redgui.exe` and inspect unresolved imports.

The missing-DLL list is the useful evidence. Do not solve this permanently by
copying random DLLs into the install. The publisher should rebuild the staged
app drop so the required runtime DLLs are bundled next to `redgui.exe`.

### Temporary Diagnostic PATH Test

If the staging deployment area contains dependency zip archives, those archives
are most likely the builder dependency share. As a diagnostic only, extract them
to a short path such as `C:\third_party`, then launch Crimson from a PowerShell
session with the likely runtime directories prepended to `PATH`:

```powershell
$env:Path = "C:\third_party\ffmpeg-nvidia\bin;C:\third_party\TensorRT-10.0.1.6\lib;C:\third_party\opencv-4.10.0\x64\vc17\bin;$env:Path"
& "$env:LOCALAPPDATA\Crimson\bin\redgui.exe"
```

Adjust the OpenCV, TensorRT, and FFmpeg paths to the actual extracted archive
layout.

Interpretation:

- If Crimson starts only after this `PATH` change, the installed app drop is
  missing runtime DLLs.
- If Crimson still fails, collect the missing-DLL list, `nvidia-smi` result,
  runtime-check output, and any crash dump or stderr output.

## Reintroduction Plan

### 1. Restore The Run-Only Install Layout

Change Windows install rules so `redgui.exe` installs to `bin/`, matching the
older run-only app-drop contract.

Acceptance:

- `cmake --install` creates `Crimson\bin\redgui.exe`.
- `share\crimson\fonts` and `share\crimson\config` are present.
- Linux install layout is not regressed.

### 2. Bundle Runtime DLLs

Port the old Windows runtime-dependency install logic into the monolith.

Implementation direction:

- Use CMake runtime dependency scanning for `redgui`.
- Search dependency directories derived from the configured FFmpeg, TensorRT,
  OpenCV, CUDA, and vcpkg roots.
- Install resolved DLLs beside `redgui.exe` in `bin/`.
- Exclude known system DLLs.
- Fail packaging if required non-system DLLs cannot be resolved.

Acceptance:

- A staged install launches without manually editing `PATH`.
- `bin/` contains the required OpenCV, FFmpeg, TensorRT, CUDA-adjacent, and
  support DLLs used by direct links.
- Missing dependency failures happen at install/package time instead of on a
  user machine.

### 3. Restore Helper Scripts

Port or rewrite the old run-only helper scripts:

- `install_crimson.ps1`
- `install_crimson.cmd`
- `check_crimson_runtime.ps1`
- `check_crimson_runtime.cmd`
- `set_crimson_cuda_device.ps1`
- `set_crimson_cuda_device.cmd`
- run-only `README.txt`

Acceptance:

- The scripts are installed at the app-drop root.
- The installer copies the app drop to `%LOCALAPPDATA%\Crimson`.
- The runtime checker verifies `bin\redgui.exe`, bundled DLL presence, fonts,
  config, metadata, crash-dump directory, and optional NVIDIA driver status.
- The CUDA-device helper writes user config without modifying the install tree.

### 4. Restore Windows Crash Dumps

Port the old Windows crash-dump helper into the monolith.

Acceptance:

- Windows builds install an unhandled-exception filter at startup.
- Dumps default to `%LOCALAPPDATA%\Crimson\CrashDumps`.
- `CRIMSON_CRASH_DUMP_DIR` overrides the dump location.
- Startup creates the dump directory, and real unhandled exceptions produce
  `.dmp` and sidecar text files.

### 5. Restore Publish Workflow

Port the app-drop publishing scripts after the staged install tree is correct.

Needed scripts or equivalents:

- dependency-share publisher for builder archives
- dependency-share setup for source-build machines
- app-drop publisher that stages `dist\Crimson`, writes `release.json`, runs the
  runtime checker, and refreshes a `current` release folder

Acceptance:

- Builders can stage dependencies from the internal share.
- Publishers can create a run-only app drop from `cmake --install`.
- Users receive only the app drop, not the builder dependency archives.

### 6. Validate On Windows

Validation should cover both packaging and runtime behavior:

```powershell
cmake --preset windows-trt10-cuda12.4-no-sfm
cmake --build --preset build-windows-trt10-cuda12.4-no-sfm-release
cmake --install build\windows-trt10-cuda12.4-no-sfm --config Release --prefix dist\Crimson
powershell -ExecutionPolicy Bypass -File dist\Crimson\check_crimson_runtime.ps1
& dist\Crimson\bin\redgui.exe
```

Then repeat with the full `windows-trt10-cuda12.4` preset once the SFM-capable
OpenCV stack is validated.

Acceptance:

- App launches from `dist\Crimson` before installation.
- App launches after `install_crimson.ps1` copies it to `%LOCALAPPDATA%`.
- Fonts/config are found without changing the working directory.
- A representative Zarr opens.
- Video decode path is exercised.
- GPU selection and crash-dump behavior are validated.

## Near-Term Guidance

For the immediate Windows machine, use the old build if it is sufficient for the
user. If it fails to start, first determine whether the installed app drop is
missing runtime DLLs. The strongest evidence is:

- `bin\redgui.exe` exists but `bin` lacks expected DLLs.
- `check_crimson_runtime.ps1` warns about missing or sparse DLLs.
- dependency inspection reports unresolved OpenCV, FFmpeg, TensorRT, CUDA, or
  support DLLs.
- temporarily adding extracted dependency runtime folders to `PATH` lets the
  app start.

If those are true, the follow-up is to republish a complete run-only app drop,
not to make each user manually install the builder dependency archives.
