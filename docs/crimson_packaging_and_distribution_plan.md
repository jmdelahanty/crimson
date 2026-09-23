# Crimson Packaging and Distribution Plan

Purpose: define a packaging strategy that makes a Windows build realistic
without regressing Linux, and moves Crimson toward a cleaner install model on
both platforms.

Date anchored: 2026-04-02.

---

## Goals

- Produce a reproducible install tree from CMake `install()` rules, not from
  ad hoc copies out of the build directory.
- Support a Windows-first distribution path for `redgui`.
- Improve Linux layout, relocatability, and runtime resource discovery.
- Make optional features explicit at build time, install time, and runtime.
- Start with a staged folder or archive, then add a real installer after the
  runtime layout is stable.

## Non-Goals

- Final legal review of third-party binary redistribution terms.
- A generic CPU-only Windows build.
- A plugin ABI stable enough for third-party extensions.
- Native distro packaging (`.deb`/`.rpm`) in the first pass.

---

## Why This Is Needed

Current repo state is suitable for local Linux development, but not for
cross-platform distribution:

- `CMakeLists.txt` hard-codes Linux-specific roots such as `/opt/crimson`,
  `/usr/local/TensorRT-10.0.1.6`, `/opt/orange/lib/ffmpeg-nvidia`, and
  `/usr/local/cuda/include`.
- `src/camera.h` includes `opencv2/sfm.hpp` from an absolute Linux path.
- `src/common.hpp` uses POSIX filesystem calls directly.
- `src/red.cpp` and `src/ui_path_config.*` assume Unix-style config and temp
  paths.
- `build.bat` exists, but it is stale relative to the current source tree and
  should not be treated as a maintained Windows build path.

The immediate problem is not "how to make an installer". The immediate problem
is "how to make the app produce a relocatable install tree whose dependencies
and resources are discoverable on a clean machine".

---

## Packaging Basics

There are three different kinds of "optional", and Crimson should treat them
separately:

- `Build-time optional`
  - A feature is compiled in or out.
- `Install-time optional`
  - An installer includes or omits a component.
- `Runtime optional`
  - The app still starts if a component is absent, and disables only that
    feature.

Important constraint:

- If `redgui` directly links a DLL or shared library, that dependency is not
  truly install-time optional. On Windows especially, the process may fail to
  start before Crimson code runs if a required DLL is missing.

Therefore:

- Installer checkboxes alone do not make a dependency optional.
- Truly optional features must either:
  - live behind separate build variants, or
  - move into runtime-loaded modules/plugins.

---

## Recommended Distribution Model

### Guiding Rule

Make the install tree self-contained and executable on a clean machine.

The simplest successful test is:

1. Build Crimson.
2. Run `cmake --install ...` into a staging directory.
3. Copy the staging directory to another machine.
4. Launch `redgui` successfully from that directory.

If that works, packaging is mostly a wrapping problem.

### Target Install Tree

Use one logical install tree on both Windows and Linux:

```text
Crimson/
  bin/
    redgui(.exe)
    required runtime DLLs or shared libs that must sit beside the executable
  lib/
    crimson/
      plugins/
        optional runtime-loaded modules
      private/
        bundled non-system libraries when needed
  share/
    crimson/
      fonts/
      config/
      models/
      examples/
```

Notes:

- On Windows, direct runtime DLL dependencies usually belong next to
  `redgui.exe` in `bin/`.
- On Linux, private bundled shared libraries should typically live in
  `lib/crimson/private`, with runtime search paths configured relative to the
  executable.
- Resources such as fonts and default config should not depend on the current
  working directory.

### Runtime Resource Discovery

Crimson should resolve resources relative to the executable install root, not
relative to `cwd`.

Preferred lookup order:

1. explicit environment override
2. executable-relative installed resource path
3. repo-local development path
4. user config path

This change helps both Windows and Linux.

---

## Proposed Components

The current codebase is still too tightly coupled for every feature to become a
runtime-optional plugin immediately. The practical approach is to define
components now, even if the first implementation uses build variants instead of
dynamic modules.

### `core`

Always installed.

Contents:

- `redgui`
- fonts
- default config files
- any runtime libraries required just to start the app and show the UI
- core data loading and rendering stack

Likely dependencies in the first pass:

- OpenGL
- GLFW
- GLEW
- OpenCV core modules used by the main app
- HDF5
- TensorStore
- CUDA runtime if the current rendering path still depends on CUDA/OpenGL
  interop at startup

### `decode_nv`

Optional component.

Contents:

- FFmpeg/NVDEC/NVENC-related runtime support
- decode helpers or a future decode plugin/module

Likely dependencies:

- FFmpeg binaries/libraries used by Crimson
- NVIDIA Video Codec SDK runtime libraries

### `inference_trt`

Optional component.

Contents:

- TensorRT inference support
- YOLO/TensorRT engine files if shipped with the app

Likely dependencies:

- TensorRT runtime libraries
- NPP/CUDA pieces required by inference codepaths

### `tools` or `dev_tools`

Optional component.

Contents:

- non-essential CLI tools
- debug helpers
- dataset/export scripts if they are intended to be shipped

### `models`

Optional component.

Contents:

- model assets
- TensorRT engine files
- example presets

This should remain separate from `core` so users are not forced to install
large binary assets to run the base app.

---

## Recommended Technical Direction

### Phase 0: Define Support Targets

Document the supported first-release matrix clearly:

- Windows: NVIDIA GPU required
- Linux: NVIDIA GPU required
- macOS: unsupported

This matters because the current code is not "cross-platform desktop" in the
abstract. It is "cross-platform NVIDIA workstation" software.

### Phase 1: Install Tree Hygiene

Before building a Windows installer, do the following on Linux first:

- replace hard-coded paths in `CMakeLists.txt` with cache variables and proper
  package discovery
- remove absolute includes such as the OpenCV SFM include in `src/camera.h`
- add executable-relative resource lookup
- define `install()` rules for:
  - executable
  - fonts
  - default config
  - shipped models/assets
- stop treating the build directory as the runtime distribution

This phase improves Linux immediately and is prerequisite to Windows.

### Phase 2: Introduce Explicit Feature Options

Add clear build options such as:

- `CRIMSON_ENABLE_NVDEC`
- `CRIMSON_ENABLE_TENSORRT`
- `CRIMSON_ENABLE_DEV_TOOLS`

Expected outcome:

- a `core` build can compile and install cleanly
- heavier features can be compiled out intentionally instead of failing by
  accident because a machine is missing a dependency

### Phase 3: Choose the First Optionality Mechanism

There are two realistic paths:

- `Build variants`
  - Example: `Crimson Core` and `Crimson Full`
  - Faster to ship
  - Less elegant
- `Runtime-loaded modules`
  - Example: `crimson_decode` and `crimson_inference`
  - Cleaner install-time optionality
  - Requires refactoring to remove direct linkage from `redgui`

Recommendation:

- Start with build variants.
- Move to runtime-loaded modules after the Windows build exists and the
  boundaries are clearer.

This is the fastest path that does not trap the project in a bad packaging
model long term.

### Phase 4: Create a Staging Target

Add a packaging/staging build step that produces something like:

```text
dist/Crimson/
  bin/
  lib/
  share/
```

This staged directory should become the single source for:

- zipped developer drops
- Windows installers
- Linux tarballs
- future AppImage or native packages

---

## CMake Best Practices to Adopt

### 1. Use `GNUInstallDirs`

Use standard install directory variables:

- `${CMAKE_INSTALL_BINDIR}`
- `${CMAKE_INSTALL_LIBDIR}`
- `${CMAKE_INSTALL_DATADIR}`

This improves Linux packaging immediately and gives Windows a cleaner install
layout too.

### 2. Stop Using Global Path Assumptions

Replace hard-coded roots with:

- `find_package(...)`
- cache variables such as `TENSORRT_ROOT`
- imported targets where possible

Do not encode local workstation layout into the default build.

### 3. Use Target-Based Configuration

Prefer:

- `target_include_directories`
- `target_compile_definitions`
- `target_link_libraries`
- generator expressions for platform-specific behavior

Avoid adding new global compile flags unless they are truly universal.

### 4. Use `install()` Rules as the Packaging Contract

Example shape:

```cmake
install(TARGETS redgui
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT core)

install(DIRECTORY fonts/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/crimson/fonts
        COMPONENT core)

install(FILES config/ui_paths.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/crimson/config
        COMPONENT core)
```

This is the foundation for both Windows and Linux packaging.

### 5. Add Component Definitions Early

Even before optional modules exist, define packaging components now:

- `core`
- `decode_nv`
- `inference_trt`
- `tools`
- `models`

That makes later installer work much cleaner.

---

## Windows Packaging Strategy

### First Successful Deliverable

Do not start with a full installer.

Start with:

- a clean Windows build
- a staged install directory
- a `.zip` or unpacked folder that launches on a clean machine

Why:

- it isolates runtime layout issues from installer authoring issues
- it is easier to debug missing DLLs
- it is easier to iterate on quickly

### App-Local Deployment

For Windows, prefer app-local deployment:

- `redgui.exe` and its direct DLL dependencies in `bin/`
- optional runtime-loaded modules in a dedicated plugin directory

That is more reliable than depending on global `PATH` or asking users to place
DLLs in system directories.

### Optional Components on Windows

The likely sequence is:

1. ship `Crimson Core`
2. ship `Crimson Full`
3. later refactor heavy features into optional runtime-loaded modules

If the app still directly links TensorRT or decode libraries, those components
are not genuinely optional at runtime.

### Installer Options

After the staged folder is reliable, wrap it using one of:

- CPack
- Inno Setup
- WiX

Recommendation:

- keep the first Windows deliverable as a `.zip`
- add a GUI installer only after the staging directory is stable

### Redistribution Caution

Before bundling vendor binaries, confirm current redistribution terms for:

- CUDA runtime pieces
- TensorRT
- FFmpeg binaries
- NVIDIA Video Codec SDK-related binaries

This document does not assume any redistribution is automatically permitted.

---

## Linux Packaging and Best Practices

This packaging cleanup should improve Linux as much as Windows.

The detailed Linux distribution plan lives in
[docs/crimson_linux_distribution_strategy.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_linux_distribution_strategy.md).
That document treats `/opt` and module files as acceptable builder or
admin-managed workstation mechanisms, but not as the final user-facing
installation model.

### Core Linux Principles

- Do not hard-code `/opt`, `/usr/local`, or workstation-specific library roots
  into normal builds.
- Support relocatable installs where practical.
- Distinguish between:
  - public system dependencies the distro should provide
  - private libraries Crimson bundles itself
- Follow XDG conventions for per-user config and cache files.

### Recommended Linux Layout

Installed tree:

```text
<prefix>/
  bin/redgui
  lib/crimson/private/
  lib/crimson/plugins/
  share/crimson/fonts/
  share/crimson/config/
```

User config:

- `${XDG_CONFIG_HOME}/crimson/...`
- fallback: `~/.config/crimson/...`

User cache or temp output:

- `${XDG_CACHE_HOME}/crimson/...`
- fallback: `~/.cache/crimson/...`

Avoid Linux-only defaults such as `/tmp/crimson_buffer_dumps` and `/nvme1`
except as development overrides.

### Linux Runtime Search Paths

If Crimson bundles private shared libraries on Linux, prefer executable-relative
runtime paths such as:

- `$ORIGIN/../lib`
- `$ORIGIN/../lib/crimson/private`

That is safer than embedding machine-specific library directories.

### Packaging Formats

Recommended order:

1. staged install tree
2. tarball for manual install
3. optional AppImage for users outside package managers
4. optional native packages later

Native distro packages should usually depend on system-provided public
libraries rather than bundling everything.

### Linux Improvements Enabled by This Plan

- cleaner install prefixes
- fewer local-path hacks in `CMakeLists.txt`
- better reproducibility on non-Janelia machines
- easier CI packaging later
- consistent resource lookup whether launched from source tree or installed tree

---

## Provisional Component Boundaries for Crimson

The current code suggests this likely breakdown:

| Area | Near-Term Status | Long-Term Packaging Goal |
|------|------------------|--------------------------|
| UI + resource loading | `core` | `core` |
| TensorStore/HDF5/Zarr I/O | `core` | `core` |
| OpenCV calibration utilities | `core` | `core` |
| FFmpeg decode path | likely `full` only at first | `decode_nv` |
| TensorRT/YOLO inference | likely `full` only at first | `inference_trt` |
| Models/engine files | separate asset bundle | `models` |

Important caveat:

- Because the current render and processing path already uses CUDA in several
  core flows, `core` may still require CUDA on both Windows and Linux in the
  first shippable version.

That is acceptable as long as it is documented honestly.

---

## Immediate Refactors That Unlock Packaging

These are the highest-value next steps:

1. Replace hard-coded Linux paths in `CMakeLists.txt` with portable discovery
   and cache variables.
2. Remove the absolute OpenCV SFM include from `src/camera.h`.
3. Move resource lookup to executable-relative installed paths.
4. Add `GNUInstallDirs` and baseline `install()` rules.
5. Define explicit feature toggles for decode and TensorRT support.
6. Produce a Linux staging directory first, then mirror the same layout on
   Windows.

These changes improve Linux immediately and also make the first Windows build
materially easier.

---

## Recommended Rollout

### Milestone A: Packaging Hygiene

- install tree exists
- executable-relative resources work
- Linux build/install works without workstation-specific paths

### Milestone B: Build Variants

- `Crimson Core`
- `Crimson Full`

### Milestone C: Windows ZIP Drop

- clean Windows build
- staged folder runs on a clean supported machine

### Milestone D: Optional Runtime Modules

- decode and inference can be omitted without blocking app startup

### Milestone E: Installers

- Windows GUI installer
- Linux tarball/AppImage

---

## Open Questions

- Which dependencies are legally redistributable as bundled binaries?
- Is a `core` build allowed to depend on CUDA, or is a non-CUDA build a product
  requirement?
- Should FFmpeg/NVDEC and TensorRT be split at build-variant level first, or is
  it worth paying the refactor cost for runtime-loaded modules immediately?
- Which user assets should ship with Crimson versus remain external?

---

## Recommendation

For the next implementation pass, prioritize packaging hygiene over installer
work:

- make the install tree real
- make resources relocatable
- define components and feature toggles
- get a staged `core` and `full` layout working on Linux first

Once that exists, the same structure can drive Windows packaging cleanly, and
the Linux build will already be closer to best practice instead of carrying
another special-case path.
