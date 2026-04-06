# Crimson Windows Internal Publish Workflow

Purpose: describe the publisher-side scripts for populating the internal
Windows dependency share and the staged app drop.

Date anchored: 2026-04-06.

This is for the person preparing internal Windows artifacts, not the end user
consuming them.

Related docs:

- [docs/crimson_windows_internal_dependency_share_setup.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_internal_dependency_share_setup.md)
- [docs/crimson_windows_install_from_source.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_install_from_source.md)
- [tools/publish_windows_dependency_share.ps1](/home/delahantyj@hhmi.org/gitrepos/crimson/tools/publish_windows_dependency_share.ps1)
- [tools/publish_windows_app_drop.ps1](/home/delahantyj@hhmi.org/gitrepos/crimson/tools/publish_windows_app_drop.ps1)

---

## Two Internal Deliverables

There are two different things to publish:

1. a dependency share for Windows users who will build Crimson from source
2. a staged app drop for Windows users who should just run the app

Keep these separate.

Recommended internal paths:

```text
\\YOUR-SERVER\crimson\windows-deps
\\YOUR-SERVER\crimson\windows-app
```

If your lab prefers mapped drives, the same locations might look like:

```text
Z:\crimson\windows-deps
Z:\crimson\windows-app
```

Your current dependency-share folder can map to:

```text
\\YOUR-SERVER\crimson\windows-deps
```

---

## Publish The Dependency Share

### First: Create The Local Archive Set

If your machine already has the unpacked dependency folders under
`C:\third_party\...`, create the archive inputs with `tar.exe`, not with
PowerShell `Compress-Archive`.

Reason:

- `Compress-Archive` can run out of memory on large trees such as TensorRT
- `tar.exe` is much more reliable for these large internal bundles

Recommended local staging folder:

```powershell
New-Item -ItemType Directory -Force -Path C:\third_party\downloads
```

Create the archives:

```powershell
tar -a -c -f C:\third_party\downloads\opencv-install-4.10.0-x64.zip -C C:\third_party opencv-install-4.10.0-x64
tar -a -c -f C:\third_party\downloads\TensorRT-10.0.1.6.Windows10.x86_64.cuda-12.4.zip -C C:\third_party TensorRT-10.0.1.6
tar -a -c -f C:\third_party\downloads\Video_Codec_SDK_13.0.19.zip -C C:\third_party Video_Codec_SDK_13.0
tar -a -c -f C:\third_party\downloads\ffmpeg-nvidia.zip -C C:\third_party ffmpeg-nvidia
```

Then verify:

```powershell
Get-ChildItem C:\third_party\downloads
```

### Then Publish The Dependency Share

From a machine that already has the approved dependency archives locally:

```powershell
cd C:\src\crimson
powershell -ExecutionPolicy Bypass -File .\tools\publish_windows_dependency_share.ps1 `
  -DownloadRoot C:\third_party\downloads `
  -ShareRoot "\\YOUR-SERVER\crimson\windows-deps" `
  -CleanShare
```

If you publish through a mapped drive instead:

```powershell
cd C:\src\crimson
powershell -ExecutionPolicy Bypass -File .\tools\publish_windows_dependency_share.ps1 `
  -DownloadRoot C:\third_party\downloads `
  -ShareRoot "Z:\crimson\windows-deps" `
  -CleanShare
```

What it does:

- copies the approved archives into the share
- computes SHA-256 for each archive
- writes `crimson-windows-deps.manifest.json` into the share

Expected result:

```text
\\YOUR-SERVER\crimson\windows-deps\
  crimson-windows-deps.manifest.json
  opencv-install-4.10.0-x64.zip
  TensorRT-10.0.1.6.Windows10.x86_64.cuda-12.4.zip
  Video_Codec_SDK_13.0.19.zip
  ffmpeg-nvidia.zip
```

That share is then consumed by:

- [tools/setup_windows_from_internal_share.ps1](/home/delahantyj@hhmi.org/gitrepos/crimson/tools/setup_windows_from_internal_share.ps1)

---

## Publish The Staged App Drop

First make sure the app is built and installed into the staged tree:

```powershell
cd C:\src\crimson
. .\tools\set_windows_dependency_roots.ps1
cmake --build --preset build-windows-trt10-cuda12.4-no-sfm-release
cmake --install build/windows-trt10-cuda12.4-no-sfm --config Release --prefix dist/Crimson
```

Then publish that staged tree:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\publish_windows_app_drop.ps1 `
  -StageRoot C:\src\crimson\dist\Crimson `
  -ShareRoot "\\YOUR-SERVER\crimson\windows-app" `
  -DropName current `
  -CleanDestination
```

Mapped-drive example:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\publish_windows_app_drop.ps1 `
  -StageRoot C:\src\crimson\dist\Crimson `
  -ShareRoot "Z:\crimson\windows-app" `
  -DropName current `
  -CleanDestination
```

What it does:

- validates the staged tree contains `bin\redgui.exe`
- validates `share\crimson\fonts` and `share\crimson\config`
- copies the whole staged tree to the target app-drop folder
- preserves the standard Windows app layout:
  - `bin\redgui.exe` plus runtime `.dll` files
  - `share\crimson\...` assets

That is the right thing to publish for run-only users. Do not publish random
files copied by hand from `build\` or `release\Release\`.

---

## What Should Be Moved

### For Builders

Publish:

- the approved dependency archives
- the generated manifest

Do not publish:

- expanded SDK folders
- `include\` / `lib\` trees copied by hand
- your local `C:\third_party` layout

### For Run-Only Users

Publish:

- the staged `dist\Crimson` tree

Do not publish:

- the raw build tree
- the repo checkout
- intermediate build outputs
- developer helper executables unless you intentionally enable and ship them

---

## Recommended Release Discipline

- treat the dependency share and the app drop as separate artifacts
- only publish from scripts, not from ad hoc Explorer copies
- if the dependency archives change, rerun the dependency-share publish script
- if the app changes, rerun the app-drop publish script
