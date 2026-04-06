# Crimson Windows Run-Only Install

Purpose: give internal Windows users a short path for installing and running
Crimson from a published app drop, without building from source.

Date anchored: 2026-04-06.

This document is for run-only users. It is not a source-build guide.

Related docs:

- [docs/crimson_windows_internal_publish_workflow.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_internal_publish_workflow.md)
- [docs/crimson_windows_install_from_source.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_install_from_source.md)

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

In File Explorer:

1. Open `Z:\crimson\windows-app\current`
2. Copy the whole `current` folder to your machine
3. Rename the copied folder to `Crimson` if needed

### UNC Example

In File Explorer:

1. Open `\\YOUR-SERVER\crimson\windows-app\current`
2. Copy the whole `current` folder to your machine
3. Rename the copied folder to `Crimson` if needed

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
2. delete or rename your local `Crimson` folder
3. copy the new published app folder locally
4. launch the new `bin\redgui.exe`

Do not mix files from two different app drops in the same folder.

---

## What Not To Do

Do not:

- copy only `redgui.exe`
- copy only the `bin\` folder
- move the `.dll` files away from `bin\`
- point the app at a partially copied folder

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
