# Crimson Windows Run-Only Install

Purpose: give internal Windows users a short path for installing and running
Crimson from a published app drop, without building from source.

Date anchored: 2026-04-06.

This document is for run-only users. It is not a source-build guide.

The published app drop now also includes a root-level `README.txt` with the
same basic install and launch instructions.

When installed from the published app drop, Crimson also writes a local
`install_metadata.json` and can show an in-app "update available" notice when
the share publishes a newer `latest.json`.

The published app drop also includes a cheap runtime verification script:
- [tools/check_crimson_runtime.ps1](/home/delahantyj@hhmi.org/gitrepos/crimson/tools/check_crimson_runtime.ps1)

The published app drop also includes a small CUDA device preference tool for
multi-GPU machines:
- [tools/set_crimson_cuda_device.ps1](/home/delahantyj@hhmi.org/gitrepos/crimson/tools/set_crimson_cuda_device.ps1)

Related docs:

- [docs/crimson_windows_internal_publish_workflow.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_internal_publish_workflow.md)
- [docs/crimson_windows_install_from_source.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_install_from_source.md)
- [tools/install_crimson.ps1](/home/delahantyj@hhmi.org/gitrepos/crimson/tools/install_crimson.ps1)

---

## What You Need

- a Windows machine
- a reasonably current NVIDIA driver
- access to the published Crimson app drop

You do not need:

- Visual Studio
- CUDA Toolkit
- TensorRT
- `C:\third_party`
- the Windows dependency share

Those are only needed for source builds.

---

## What To Copy

Copy the entire published app folder, not just the `.exe`.

Typical share locations:

```text
\\YOUR-SERVER\crimson\windows-app\current
Z:\crimson\windows-app\current
```

Copy that whole folder to a local path on your machine, for example:

```text
C:\Program Files\Crimson
```

or, if you do not have admin rights:

```text
C:\Users\<your-user>\AppData\Local\Crimson
```

Keep the folder layout intact.

Expected layout after copying:

```text
Crimson\
  bin\
    redgui.exe
    *.dll
  share\
    crimson\
      fonts\
      config\
```

---

## Install Steps

### Mapped-Drive Example

Preferred:

1. Open PowerShell
2. Run:

```powershell
powershell -ExecutionPolicy Bypass -File "Z:\crimson\windows-app\current\install_crimson.ps1" `
  -CreateDesktopShortcut
```

Double-click alternative:

1. Open `Z:\crimson\windows-app\current`
2. Run `install_crimson.cmd`

Optional runtime check:

```powershell
powershell -ExecutionPolicy Bypass -File "Z:\crimson\windows-app\current\check_crimson_runtime.ps1"
```

Optional CUDA GPU selection tool:

```powershell
powershell -ExecutionPolicy Bypass -File "Z:\crimson\windows-app\current\set_crimson_cuda_device.ps1"
```

### UNC Example

Preferred:

1. Open PowerShell
2. Run:

```powershell
powershell -ExecutionPolicy Bypass -File "\\YOUR-SERVER\crimson\windows-app\current\install_crimson.ps1" `
  -CreateDesktopShortcut
```

Double-click alternative:

1. Open `\\YOUR-SERVER\crimson\windows-app\current`
2. Run `install_crimson.cmd`

Optional runtime check:

```powershell
powershell -ExecutionPolicy Bypass -File "\\YOUR-SERVER\crimson\windows-app\current\check_crimson_runtime.ps1"
```

Optional CUDA GPU selection tool:

```powershell
powershell -ExecutionPolicy Bypass -File "\\YOUR-SERVER\crimson\windows-app\current\set_crimson_cuda_device.ps1"
```

Default install location:

```text
C:\Users\<your-user>\AppData\Local\Crimson
```

On machines with multiple NVIDIA/CUDA GPUs, the installer may ask which GPU
Crimson should prefer for video decode and rendering. If that saved choice no
longer matches the active display/OpenGL GPU, Crimson may ask again on first
launch. The selection is remembered in:

```text
C:\Users\<your-user>\AppData\Local\Crimson\config\cuda_device.json
```

To change that saved GPU choice later without reinstalling, run:

```powershell
powershell -ExecutionPolicy Bypass -File "C:\Users\<your-user>\AppData\Local\Crimson\set_crimson_cuda_device.ps1"
```

To update an existing install:

```powershell
powershell -ExecutionPolicy Bypass -File "Z:\crimson\windows-app\current\install_crimson.ps1" `
  -ReplaceExisting `
  -CreateDesktopShortcut
```

To install somewhere else, pass `-InstallRoot`, for example:

```powershell
powershell -ExecutionPolicy Bypass -File "Z:\crimson\windows-app\current\install_crimson.ps1" `
  -InstallRoot "C:\Program Files\Crimson" `
  -ReplaceExisting
```

---

## How To Launch

Run:

```text
<install-root>\bin\redgui.exe
```

Example:

```text
C:\Program Files\Crimson\bin\redgui.exe
```

You can create a desktop shortcut to `bin\redgui.exe` if you use Crimson often.

Running directly from the share can work for quick testing, but normal use
should prefer a local copy.

---

## How To Update

When a new internal app drop is published:

1. close Crimson
2. rerun `install_crimson.ps1` with `-ReplaceExisting`

Do not mix files from two different app drops in the same folder.

The installer now runs cheap preflight and postinstall runtime checks by
default.

---

## What Not To Do

Do not:

- copy only `redgui.exe`
- copy only the `bin\` folder
- move the `.dll` files away from `bin\`
- point the app at a partially copied folder
- run `install_crimson.ps1` from an existing local install instead of from the published app drop

The `.dll` files need to stay next to `redgui.exe` in `bin\`.

---

## Troubleshooting

If the app does not launch:

- confirm you copied the whole published app folder, not just one file
- confirm `bin\redgui.exe` exists
- confirm the `.dll` files are still in `bin\`
- confirm `share\crimson\fonts` and `share\crimson\config` exist
- confirm the machine has a current NVIDIA driver

If Windows reports a missing `.dll`, report the exact filename.

If Crimson hard-crashes after launch, look in:

```text
C:\Users\<your-user>\AppData\Local\Crimson\CrashDumps
```

Report both:

- the `.dmp` file
- the matching `.txt` sidecar

If you want to re-check the install and NVIDIA driver visibility manually, run:

```powershell
powershell -ExecutionPolicy Bypass -File "C:\Users\<your-user>\AppData\Local\Crimson\check_crimson_runtime.ps1"
```
