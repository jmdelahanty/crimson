# Crimson Windows Internal Dependency Share Setup

Purpose: define the simplest internal setup path for Windows users who should
install Crimson from a shared folder of approved dependency archives.

Date anchored: 2026-04-05.

This is an internal-team workflow. It assumes:

- your team maintains a shared Windows dependency folder
- that folder contains the approved archives plus a pinned manifest
- users are building Crimson from source, not using a public installer

Related docs:

- [docs/crimson_windows_install_from_source.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_install_from_source.md)
- [tools/setup_windows_from_internal_share.ps1](/home/delahantyj@hhmi.org/gitrepos/crimson/tools/setup_windows_from_internal_share.ps1)
- [tools/crimson-windows-deps.manifest.example.json](/home/delahantyj@hhmi.org/gitrepos/crimson/tools/crimson-windows-deps.manifest.example.json)

---

## Shared Folder Shape

Recommended internal shared folder:

```text
\\YOUR-SERVER\crimson-windows-deps\
  crimson-windows-deps.manifest.json
  opencv-install-4.10.0-x64.zip
  TensorRT-10.0.1.6.Windows10.x86_64.cuda-12.4.zip
  Video_Codec_SDK_13.0.19.zip
  ffmpeg-nvidia.zip
```

The manifest should pin:

- exact filenames
- SHA-256 checksums
- destination directory names

The checked-in template for that manifest is:

- [tools/crimson-windows-deps.manifest.example.json](/home/delahantyj@hhmi.org/gitrepos/crimson/tools/crimson-windows-deps.manifest.example.json)

Copy that template into the shared folder as:

```text
crimson-windows-deps.manifest.json
```

and replace the placeholder hashes with the real SHA-256 values.

---

## Recommended User Flow

Open `Developer PowerShell for VS 2022` or `x64 Native Tools PowerShell for VS 2022`.

Then from the repo root:

```powershell
cd C:\src\crimson
. .\tools\setup_windows_from_internal_share.ps1 `
  -ShareRoot "\\YOUR-SERVER\crimson-windows-deps" `
  -CleanDestination `
  -LoadDependencyRoots
```

Why run it this way:

- it stages the archives into `C:\third_party\...`
- it validates them against the manifest
- it runs the prereq checker
- it loads the Crimson dependency roots into the current PowerShell session

The leading `. ` matters. Without dot-sourcing, the dependency roots will not
stay loaded in the caller's shell after the script exits.

After that:

```powershell
cmake --preset windows-trt10-cuda12.4-no-sfm
cmake --build --preset build-windows-trt10-cuda12.4-no-sfm-release
```

If you want the staged install tree too:

```powershell
cmake --install build/windows-trt10-cuda12.4-no-sfm --config Release --prefix dist/Crimson
```

---

## What The Wrapper Does

The wrapper script:

- calls [stage_windows_dependency_archives.ps1](/home/delahantyj@hhmi.org/gitrepos/crimson/tools/stage_windows_dependency_archives.ps1)
  with `-RequireManifest`
- stages the approved archives into:
  - `C:\third_party\opencv-install-4.10.0-x64`
  - `C:\third_party\TensorRT-10.0.1.6`
  - `C:\third_party\Video_Codec_SDK_13.0`
  - `C:\third_party\ffmpeg-nvidia`
- runs [check_windows_prereqs.ps1](/home/delahantyj@hhmi.org/gitrepos/crimson/tools/check_windows_prereqs.ps1)
- optionally dot-sources [set_windows_dependency_roots.ps1](/home/delahantyj@hhmi.org/gitrepos/crimson/tools/set_windows_dependency_roots.ps1)

---

## Recommended Team Practice

- Treat the shared folder as versioned infrastructure, not an informal dumping
  ground.
- Update the manifest whenever any archive changes.
- Prefer replacing archives with new versioned files rather than silently
  mutating files under the same name.
- Keep the share internal unless redistribution terms for every bundled vendor
  archive have been reviewed.

---

## Troubleshooting

If the wrapper fails:

- confirm the share path is reachable from Windows
- confirm the manifest filename is exactly
  `crimson-windows-deps.manifest.json`
- confirm the archive filenames match the manifest exactly
- confirm the SHA-256 values are correct
- confirm you are in a Visual Studio developer shell so `cl` and related tools
  are available

If the wrapper succeeds but build/launch still fails:

- rerun the prereq checker directly
- rerun `. .\tools\set_windows_dependency_roots.ps1`
- then follow the longer guide in
  [docs/crimson_windows_install_from_source.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_install_from_source.md)
