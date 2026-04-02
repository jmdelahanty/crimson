# Crimson Dependency Stack Promotion Process

Purpose: define the process and evidence required to move a dependency stack
between `experimental`, `planned`, `baseline`, and `legacy`.

Date anchored: 2026-04-02.

---

## Why This Exists

Crimson's dependency stack is sensitive to version skew across:

- CUDA toolkit
- TensorRT
- OpenCV
- FFmpeg / NVIDIA codec support

That means support status should not be a casual judgment. A stack should move
between statuses only when there is explicit evidence.

This document defines that evidence.

---

## Status Flow

Normal progression:

`experimental` -> `planned` -> `baseline` -> `legacy`

Notes:

- a stack may be retired directly from `planned` to `legacy` if work is
  abandoned
- a `baseline` stack may be downgraded to `planned` or `experimental` if
  regressions are found
- only one stack family should normally be the primary `baseline` for routine
  development on a given OS

---

## Entry Criteria by Status

### `experimental`

Use `experimental` when a stack is worth investigating but is not yet part of
shared engineering expectations.

Minimum requirements:

- a proposed version tuple exists
- at least one engineer has a concrete reason to explore it
- the stack is listed in the support matrix

Must not imply:

- support
- shared preset stability
- expected user success

### `planned`

Use `planned` when the team intends to bring up the stack next, but validation
is still incomplete.

Minimum requirements:

- exact dependency versions are chosen
- a clear target OS is named
- a proposed preset name exists
- known blockers are identified
- the support matrix is updated

Usually, but not always:

- the stack may appear in shared presets if the team wants a stable target name
  early

### `baseline`

Use `baseline` only when the stack is the expected path for routine development
on that OS.

Minimum requirements:

- shared presets exist
- exact version checks are enforced
- configure succeeds on the target OS
- build succeeds on the target OS
- runtime smoke checks pass
- install or staging flow is validated
- docs are updated to reflect the stack as current

### `legacy`

Use `legacy` when the stack is no longer maintained as a current engineering
target, but is still worth documenting historically.

Minimum requirements:

- the stack is no longer the recommended path
- the support matrix explains that clearly

---

## Promotion Gates

### Gate A: `experimental` -> `planned`

Required evidence:

- exact candidate versions chosen for CUDA, TensorRT, and OpenCV
- target OS identified
- known dependency roots and toolchain path identified
- support matrix updated with `planned` status

Recommended evidence:

- a draft preset name
- notes on likely compatibility risk

### Gate B: `planned` -> `baseline`

Required evidence:

- configure passes with the intended preset
- compile succeeds for the intended preset
- runtime launch succeeds
- core app behavior is smoke tested
- dependency versions are pinned in shared presets
- exact version checks are enforced in CMake
- install or staging path is tested
- docs are updated

Required evidence should be captured for the target OS separately. Linux success
does not automatically promote Windows, and Windows success does not
automatically promote Linux.

### Gate C: `baseline` -> `legacy`

Required evidence:

- a newer stack replaces it, or the team intentionally drops support
- the support matrix is updated
- README and related docs no longer present it as current

### Downgrade Gate: `baseline` -> `planned` or `experimental`

Use this when:

- repeatable build failures appear
- runtime regressions are found
- a key dependency becomes unavailable or unsupported

Required evidence:

- the failure mode is documented
- the support matrix is updated immediately
- the baseline claim is removed from docs

---

## Baseline Validation Checklist

A stack may be promoted to `baseline` only when the following checklist is
complete for the target OS.

### Configure

- CMake preset resolves the intended dependency roots
- exact version checks pass
- configure completes successfully

### Build

- release build completes
- debug build completes, if the OS has a debug preset
- generated binary links against the intended dependency family

### Runtime Smoke

- app launches
- fonts and config are found through installed or staged paths
- a representative dataset opens
- video decode path is exercised if the stack claims decode support
- inference path is exercised if the stack claims TensorRT support

### Packaging / Install

- `cmake --install` completes into a staging directory
- staged app launches from the staging directory
- runtime library discovery works from the staged layout

### Documentation

- support matrix updated
- README updated if the stack is current-facing
- any new environment variables or preset names documented

---

## Evidence Format

Promotion should leave behind a short written record.

Minimum recommended contents:

- stack ID
- OS
- exact versions
- preset name
- configure result
- build result
- runtime smoke result
- install/staging result
- remaining caveats
- date

This can live in:

- a PR description
- an issue comment
- a dedicated validation note

The important part is that the decision is auditable.

---

## Practical Rule of Thumb

Use these shortcuts:

- `experimental`
  - "we think this may work"
- `planned`
  - "we are targeting this next"
- `baseline`
  - "we have actually proven this enough to recommend it"
- `legacy`
  - "this used to matter, but it is not the maintained path anymore"

---

## Immediate Application to Crimson

As of 2026-04-02:

- Linux `12.4 / 10.0.1.6 / 4.10.0` is treated as `baseline`
- Windows `12.4 / 10.0.1.6 / 4.10.0` is treated as `planned`
- any CUDA `13.x` stack is `experimental`

The next stack most likely to use this process is the planned Windows target.

Working validation record:

- [docs/crimson_windows_trt10_cuda12.4_validation_record.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/crimson_windows_trt10_cuda12.4_validation_record.md)
