# Crimson Windows Status Checkpoint

Purpose: capture the current Windows state of Crimson after the first source
build, packaging, internal app-drop, crash-capture, and multi-GPU follow-up
work.

Date anchored: 2026-04-23.

Branch at checkpoint:
- `codex/windows-no-sfm-mvp-202604021712`

Related docs:

- [docs/crimson_windows_trt10_cuda12.4_validation_record.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_trt10_cuda12.4_validation_record.md)
- [docs/crimson_windows_install_from_source.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_install_from_source.md)
- [docs/crimson_windows_run_only.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_run_only.md)
- [docs/crimson_windows_internal_publish_workflow.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_internal_publish_workflow.md)

---

## Current Summary

Crimson now has a workable internal Windows path in two forms:

- source-build flow for Windows developers and power users
- run-only published app drop for internal users who should not build from source

The Windows work is no longer only "first laptop bring-up." It now includes:

- dependency-share publishing for builders
- published app-drop installs for run-only users
- bundled runtime DLLs in the staged app
- cheap runtime validation scripts
- crash dump generation for hard Windows failures
- surfaced background-thread failures in the UI
- multi-GPU CUDA device selection and a post-install CUDA device tool

This is good enough for internal use and support, but it is not yet a polished
public installer product.

---

## What Works Now

### Internal Source-Build Path

The repo now has a documented and scripted internal Windows source-build flow:

- dependency archives can be staged from a shared `windows-deps` folder
- `check_windows_prereqs.ps1` validates the expected Windows toolchain and dependency roots
- `setup_windows_from_internal_share.ps1` prepares a machine from the internal dependency share
- `smoke_test_windows_build.ps1` verifies configure, build, install, and staged executable presence

For first Windows bring-up where 3D triangulation is not needed, the preferred
preset remains:

- `windows-trt10-cuda12.4-no-sfm`

### Run-Only Published App Drop

Crimson now has an internal run-only Windows app-drop flow.

Published app layout:

- `bin\\redgui.exe`
- `bin\\*.dll`
- `share\\crimson\\fonts\\...`
- `share\\crimson\\config\\...`
- root-level helper scripts and `README.txt`

Important point:

- the app drop is no longer just a copied `.exe`
- the staged and published layout is the supported runtime unit

The published app now includes:

- `install_crimson.ps1` / `install_crimson.cmd`
- `check_crimson_runtime.ps1` / `check_crimson_runtime.cmd`
- `set_crimson_cuda_device.ps1` / `set_crimson_cuda_device.cmd`
- `README.txt`
- `release.json`

At least one internal user was able to launch the published app drop on another
Windows laptop after the runtime DLL bundling fixes landed. That was the first
real confirmation that the run-only path was viable.

### Internal Publishing Workflow

Internal publishing now has two supported deliverables:

- `windows-deps` for builders
- `windows-app` for run-only users

Recommended `windows-app` layout:

- `current\\`
- `releases\\<timestamp>\\`

The publisher scripts now support:

- versioned release publishing
- refreshing `current\\`
- archiving the previous `current\\`
- writing `release.json` in each published release
- writing `latest.json` at the share root

### Runtime Diagnostics

Windows diagnostics are materially better than they were at first bring-up.

Current coverage includes:

- surfaced decoder-thread failures in the UI
- surfaced stimulus software-decode thread failures in the UI
- surfaced YOLO worker-thread failures in the UI
- Windows hard-crash dump writing to `%LOCALAPPDATA%\\Crimson\\CrashDumps`

So the app is less likely to "just disappear" without any artifact or message.

### Multi-GPU Handling

Crimson no longer assumes CUDA device `0` is always the right GPU.

Current behavior:

- installer can prompt for a preferred CUDA GPU on multi-GPU Windows machines
- the app can prompt at startup if the saved CUDA choice conflicts with the
  active OpenGL/display GPU
- the choice is saved in `%LOCALAPPDATA%\\Crimson\\config\\cuda_device.json`
- users can change it later with `set_crimson_cuda_device.ps1` without reinstalling

This was added after real multi-GPU support issues surfaced on a Windows
machine that exposed more than one NVIDIA GPU.

### File Open Support

The media/stimulus open UI no longer behaves as if only `.mp4` is valid.

The relevant Windows/open-dialog path now supports common video extensions,
including:

- `.mp4`
- `.avi`
- `.mov`
- `.mkv`

---

## What Is Still Limited

### Large Main-Camera Playback on Low-Tier Laptop GPUs

The high-resolution playback problem is not fully solved on weaker Windows
laptop GPUs.

The important current conclusion is:

- a `4512x4512 @ 60 fps` HEVC main camera stream is still likely to run below
  stable `1.0x` playback on an RTX A1000 laptop-class machine

That result held even after:

- renderer experiments
- upload/presentation fixes
- VSync investigation
- GPU/decode profiling
- software main-camera decode experiments
- discrete-GPU/OpenGL forcing checks

The practical interpretation is:

- the remaining ceiling is mostly hardware + architecture, not one obvious bug
- internal users on weaker GPUs should expect that this workload may stay below
  true real-time playback

### Run-Only Distribution Is Internal, Not Fully Productized

The current Windows packaging is good enough for internal lab use, but it is
still not a finished public distribution story.

What is still missing from a more polished product:

- a formal installer package such as MSI or similar
- a fully automatic updater
- broader clean-machine validation across more hardware and IT environments
- a public redistribution review for third-party binary packaging

### Updates Are Notify-Only

The publish/install flow now writes:

- `latest.json`
- `release.json`
- `install_metadata.json`

and the app can notify users when a newer published version exists.

But updates are still applied manually:

- rerun the installer from the newer published app drop
- use `-ReplaceExisting`

This is not a silent or automatic in-place updater.

---

## Recommended Current Windows Usage

### For Builders

Use:

- the internal dependency share
- the setup/prereq/smoke scripts
- the `windows-trt10-cuda12.4-no-sfm` preset unless SFM-backed triangulation is explicitly needed

### For Run-Only Users

Use:

- the published `windows-app\\current` path
- `install_crimson.ps1` or `install_crimson.cmd`
- `check_crimson_runtime.ps1` for cheap support checks
- `set_crimson_cuda_device.ps1` on multi-GPU systems when the saved GPU needs to change

Do not use:

- raw build directories
- copied single executables
- ad hoc DLL copying

---

## Suggested Next Steps

The highest-value next steps are now less about "can Windows work at all" and
more about support and maintainability:

1. validate the app drop on more clean Windows machines and GPU layouts
2. keep improving machine-side diagnostics around NVDEC/driver/GPU mismatch
3. continue backend-decoupling work so decode, presentation, and UI are less tightly coupled
4. decide whether the internal app drop should remain the long-term delivery
   mechanism or whether a fuller installer/updater is worth building

---

## Bottom Line

Current Windows status is:

- source-build path: working for internal use
- run-only app-drop path: working for internal use
- diagnostics: materially improved
- multi-GPU handling: improved and user-configurable
- low-end playback for very large HEVC streams: still hardware-limited

That is a meaningful step forward from the original Windows bring-up, but it is
not yet the end of the Windows work.
