# Crimson Linux Distribution Strategy

Purpose: document how Crimson should move from local `/opt`-style developer
installs toward a stable Ubuntu/Linux app distribution model.

Date anchored: 2026-07-06.

Related docs:

- [docs/crimson_ubuntu_macos_platform_strategy.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_ubuntu_macos_platform_strategy.md)
- [docs/crimson_packaging_and_distribution_plan.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_packaging_and_distribution_plan.md)
- [docs/crimson_supported_dependency_stack_matrix.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_supported_dependency_stack_matrix.md)
- [docs/crimson_cuda_driver_toolkit_and_presets.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_cuda_driver_toolkit_and_presets.md)

## Current State

Crimson's maintained Linux build is a source-build workflow using a pinned
NVIDIA dependency stack:

```text
CUDA Toolkit 12.4
TensorRT 10.0.1.6
OpenCV 4.10.0
FFmpeg with NVIDIA codec support
NVIDIA Video Codec SDK headers/import libraries
```

The shared presets are:

```text
linux-trt10-cuda12.4-release
linux-trt10-cuda12.4-debug
```

Those presets read machine-local dependency roots from `CRIMSON_*`
environment variables or a local `CMakeUserPresets.json`. On the current
workstations, those roots often look like:

```bash
export CRIMSON_CUDA_TOOLKIT_ROOT=/usr/local/cuda-12.4
export CRIMSON_OPENCV_DIR=/opt/crimson/lib/opencv/lib/cmake/opencv4
export CRIMSON_FFMPEG_ROOT=/opt/orange/lib/ffmpeg-nvidia
export CRIMSON_VIDEO_CODEC_SDK_ROOT=/opt/nvidia/Video_Codec_SDK
export CRIMSON_TENSORRT_ROOT=/usr/local/TensorRT-10.0.1.6
```

That is a reasonable developer and lab-workstation model, but it is not a
stable user distribution model by itself. It assumes every machine has the same
privileged filesystem layout and that users can install or modify libraries
under `/opt` and `/usr/local`.

## Build-Time vs Run-Time Requirements

For source builds, Crimson needs development artifacts:

- CUDA Toolkit headers, libraries, and `nvcc`
- TensorRT headers and libraries
- OpenCV CMake package and libraries
- FFmpeg headers and libraries
- Video Codec SDK headers and import/static libraries
- system build tools such as CMake, Ninja, compiler, and package dependencies

For run-only installs, users should need less:

- a supported NVIDIA GPU
- a compatible NVIDIA display driver
- system OpenGL/GLX/EGL surface required by the GUI
- the app drop's bundled runtime libraries, or a launcher that selects the
  correct shared runtime roots
- access to recordings and Palette/Crimson data

A run-only user should not need the CUDA Toolkit, TensorRT SDK, OpenCV
development tree, FFmpeg development headers, or a compiler just to launch
Crimson. If the user has a different CUDA Toolkit installed on the machine, the
installed app should not accidentally bind to it.

## Driver Compatibility Policy

The NVIDIA driver and CUDA Toolkit are separate surfaces.

- `nvidia-smi` reports driver capability.
- `nvcc --version` reports the CUDA Toolkit used for builds.
- the Crimson preset names the build/runtime stack Crimson expects.

For a Linux app drop built against CUDA 12.4-era runtime libraries, the release
gate should require an NVIDIA driver new enough for that shipped runtime. In
practice, the current stack should be treated as requiring a 550-series or newer
NVIDIA driver unless the release notes for the exact bundled CUDA runtime say
otherwise.

Do not infer support from a user's installed toolkit. A user can have
`/usr/local/cuda-13.x` installed and still run a CUDA 12.4-built Crimson app if
the driver is compatible and Crimson's runtime library search path resolves the
libraries from the app drop or approved module roots.

## Why `/opt` Is Not Enough

Installing dependencies into `/opt` is useful for controlled workstations:

- admins can manage a shared dependency stack once
- module files can expose a consistent environment
- source builds can use the same roots across users
- large SDKs do not need to be copied into every checkout

The drawbacks are real:

- users need privileged install rights or an admin-managed image
- path drift silently changes which libraries the app loads
- multiple CUDA/TensorRT/OpenCV stacks can conflict through `LD_LIBRARY_PATH`
- debugging becomes machine-specific
- the app is not easily copied to another compatible workstation

The stable target should therefore be: `/opt` is allowed as an implementation
detail for builders or managed clusters, but it should not be required knowledge
for normal users.

## Packaging Decision: Two Linux Modes

Crimson should support two explicit Linux modes instead of trying to make one
binary layout serve every purpose.

### Developer / Managed Workstation Mode

Absolute dependency roots are acceptable for developer builds and controlled
workstations.

Examples:

```text
/opt/crimson/lib/opencv
/opt/orange/lib/ffmpeg-nvidia
/usr/local/TensorRT-10.0.1.6
/usr/local/cuda-12.4
```

This mode is useful because:

- developers can build quickly against the installed lab stack
- admins can manage one dependency stack per workstation image
- failures are easier to diagnose while changing CMake, CUDA, TensorRT, or
  OpenCV

The runtime checker may warn about absolute `RUNPATH` in this mode, but that is
not automatically a build failure.

### User App-Drop Mode

User-facing Linux app drops should not depend on the builder's `/opt` or
`/usr/local` layout.

The intended release shape is:

```text
Crimson/
  bin/redgui
  bin/crimson
  lib/crimson/private/
  share/crimson/
  check_crimson_runtime.sh
  release.json
```

For this mode, `redgui` should resolve private app/runtime libraries from
install-relative paths such as:

```text
$ORIGIN
$ORIGIN/../lib
$ORIGIN/../lib/crimson/private
```

The app drop should bundle or deliberately select the Crimson-compatible
runtime stack for libraries such as TensorRT, OpenCV, FFmpeg, and CUDA-adjacent
runtime libraries. It should still rely on the host for things that should be
system-owned:

- NVIDIA display driver and `libcuda.so.1`
- kernel/user-mode driver stack
- display/OpenGL platform where appropriate
- base OS ABI such as glibc/libstdc++ for the target Ubuntu baseline

This prevents accidental binding to a user's unrelated TensorRT, OpenCV,
FFmpeg, or CUDA Toolkit installation.

### Policy

Do not block developer builds just because they contain absolute dependency
roots. Do block or clearly fail user-release packaging if the app drop is
claimed to be relocatable but still resolves core private dependencies from
uncontrolled `/opt`, `/usr/local`, or home-directory paths.

## Recommended Linux Distribution Model

### Tier 1: Source Build on Managed Workstations

Keep this for developers and power users.

Expected shape:

```bash
source /opt/crimson/env/crimson-trt10-cuda12.4.sh
cmake --preset linux-trt10-cuda12.4-release
cmake --build --preset build-linux-trt10-cuda12.4-release
cmake --install build/linux-trt10-cuda12.4-release --prefix dist/Crimson
```

The environment script or module file should set only build roots and runtime
paths for that shell. It should not modify global shell startup files.

### Tier 2: Staged App Drop

This is the next practical target for user installs.

Expected shape:

```text
Crimson/
  bin/
    redgui
    crimson
  lib/
    crimson/
      private/
      plugins/
  share/
    crimson/
      fonts/
      config/
      models/
  check_crimson_runtime.sh
  README.txt
  release.json
```

`bin/crimson` should be a launcher script that:

- resolves its own install root
- sets the minimal required runtime search path
- avoids inheriting unrelated CUDA/TensorRT/OpenCV paths where practical
- starts `bin/redgui`

The installed executable should also use relative RPATH entries such as:

```text
$ORIGIN
$ORIGIN/../lib
$ORIGIN/../lib/crimson/private
```

Crimson already has Linux RPATH direction in CMake; the missing piece is a
complete Linux staging/check/publish path equivalent to the Windows app-drop
workflow.

### Tier 3: Internal Shared Release

Once the staged app drop is reliable, publish it to a shared read-only location:

```text
/groups/.../crimson/linux-app/releases/<release-name>/
/groups/.../crimson/linux-app/current/
```

Users can run the current release through a small wrapper or symlink:

```bash
/groups/.../crimson/linux-app/current/bin/crimson
```

This is usually a better first Linux distribution target than a `.deb`, because
it matches the lab filesystem model and avoids system package ownership
questions while the runtime boundary is still changing.

### Tier 4: Tarball, AppImage, or Native Package

After the staged tree is proven on multiple machines:

- tarball is the lowest-friction portable format
- AppImage is useful for unmanaged Linux desktops if OpenGL/NVIDIA runtime
  assumptions are handled carefully
- `.deb` is useful once dependencies and file ownership are stable

Native packages should not be the first target. A native package forces policy
decisions about which libraries are bundled, which are system dependencies, and
how driver/runtime checks are expressed.

## What To Bundle vs Require From The System

Likely system requirements:

- NVIDIA display driver
- kernel/user-mode driver libraries installed by the driver
- OpenGL/GLX/EGL platform libraries
- basic glibc/libstdc++ compatibility for the target Ubuntu baseline

Likely private or app-selected dependencies:

- TensorRT runtime libraries
- OpenCV libraries built for Crimson's stack
- FFmpeg libraries built with the required codec support
- CUDA-adjacent runtime libraries not guaranteed by the driver package
- Crimson plugins, models, fonts, and config

CUDA driver libraries such as `libcuda.so` should come from the installed
NVIDIA driver, not from the app. CUDA runtime and CUDA toolkit libraries need a
deliberate policy per release because bundling permissions and driver
compatibility matter.

## Runtime Checker Needed

Linux needs a `check_crimson_runtime.sh` equivalent to the Windows checker.

It should report:

- Crimson release metadata and install root
- `redgui` exists and is executable
- `ldd` unresolved dependencies
- which paths resolve TensorRT, OpenCV, FFmpeg, CUDA runtime, and NPP
- `nvidia-smi` GPU name and driver version
- OpenGL/GLX sanity through `glxinfo` or a small GLFW probe
- whether the selected driver satisfies the release's documented minimum
- whether required resources under `share/crimson` exist
- whether a GUI smoke can launch when a real display is available

This checker should be part of every app drop and should run before publish.

## Stability Work Needed

1. Add Linux build/stage helper:
   `tools/build_linux_app_drop.sh`.
2. Add Linux runtime checker:
   `tools/check_crimson_runtime.sh`.
3. Add Linux publish helper after the staged tree is proven:
   `tools/publish_linux_app_drop.sh`.
4. Ensure `cmake --install` stages all required resources under one prefix.
5. Confirm executable-relative resource lookup for fonts, config, and models.
6. Confirm RPATH points to install-relative private library directories.
7. Decide which third-party libraries are bundled versus system/module
   requirements.
8. Record release metadata:
   commit, branch, preset, dependency versions, CUDA architectures, minimum
   driver, build host, build time.
9. Run GUI smoke from the staged install, not only from the build tree.
10. Update `CMAKE_CUDA_ARCHITECTURES` before broad release. The current source
    sets `80;86`, which covers Ampere-class targets but does not explicitly
    include Ada RTX 40-series `sm_89`.

## Practical Near-Term Plan

The next Linux packaging slice should be a staged app drop, not a native
installer.

Minimum acceptable result:

```bash
tools/build_linux_app_drop.sh
dist/Crimson/check_crimson_runtime.sh
dist/Crimson/bin/crimson --zarr <known-good-zarr>
```

## Current Implementation Slice

The repo now has the first Linux app-drop helpers:

```bash
tools/build_linux_app_drop.sh
tools/check_crimson_runtime.sh
tools/crimson_linux_launcher.sh
```

The CMake install rules stage:

```text
<install-root>/bin/redgui
<install-root>/bin/crimson
<install-root>/check_crimson_runtime.sh
<install-root>/README.txt
<install-root>/share/crimson/fonts/
<install-root>/share/crimson/config/
```

Default build command:

```bash
CRIMSON_CUDA_TOOLKIT_ROOT=/usr/local/cuda-12.4 \
CRIMSON_OPENCV_DIR=/opt/crimson/lib/opencv/lib/cmake/opencv4 \
CRIMSON_FFMPEG_ROOT=/opt/orange/lib/ffmpeg-nvidia \
CRIMSON_TENSORRT_ROOT=/usr/local/TensorRT-10.0.1.6 \
tools/build_linux_app_drop.sh --clean-install
```

Useful stricter check on a real GPU/display machine:

```bash
dist/Crimson/check_crimson_runtime.sh --require-nvidia-smi --require-gl
```

Dependency audit manifest:

```bash
dist/Crimson/check_crimson_runtime.sh \
  --write-dependency-manifest dist/Crimson/dependency_manifest.json
```

Release-mode audit:

```bash
dist/Crimson/check_crimson_runtime.sh \
  --mode release \
  --write-dependency-manifest dist/Crimson/dependency_manifest.json \
  --require-nvidia-smi \
  --require-gl
```

Release mode sanitizes the library search used for `ldd`: it ignores the
caller's ambient `LD_LIBRARY_PATH` and uses only app-local library directories
plus any explicit `CRIMSON_ALLOWED_RUNTIME_ROOTS`. Developer mode keeps the
inherited `LD_LIBRARY_PATH` for convenience, but reports it and warns when it
contains developer roots such as `/opt`, `/usr/local`, or `$HOME`.

The first clean release audit on this workstation showed that `/opt/crimson`
OpenCV and `/opt/orange` OpenCV both resolve `opencv_videoio` against the
system FFmpeg 60 ABI, while Crimson itself links directly against the custom
Orange FFmpeg/NVIDIA 58 ABI. That mixed FFmpeg stack is not a releasable state.

Because Crimson's intended Linux video stack is the custom FFmpeg/NVIDIA root,
rebuild OpenCV so its `videoio` module uses the same FFmpeg ABI:

```bash
tools/build_opencv_ffmpeg_nvidia_linux.sh \
  --ffmpeg-root /opt/orange/lib/ffmpeg-nvidia \
  --install-prefix /opt/crimson/lib/opencv-ffmpeg-nvidia
```

The helper installs into a validation prefix by default instead of overwriting
`/opt/crimson/lib/opencv`. It forces `PKG_CONFIG_PATH` to the FFmpeg/NVIDIA
root and validates the result by checking that `opencv_version --verbose`
reports FFmpeg 58/56/5/3 libraries and that `libopencv_videoio.so.410` directly
needs `libavcodec.so.58` and `libavformat.so.58`.

After that succeeds, point Crimson at the rebuilt OpenCV:

```bash
export CRIMSON_OPENCV_DIR=/opt/crimson/lib/opencv-ffmpeg-nvidia/lib/cmake/opencv4
export CRIMSON_FFMPEG_ROOT=/opt/orange/lib/ffmpeg-nvidia
```

Then rebuild the app drop. The current recommended hybrid package bundles
OpenCV and FFmpeg into the app tree, while leaving TensorRT and CUDA as
admin-managed runtime roots:

```bash
CRIMSON_ALLOWED_RUNTIME_ROOTS=/opt/crimson/lib/opencv-ffmpeg-nvidia/lib:/opt/orange/lib/ffmpeg-nvidia/lib:/usr/local/TensorRT-10.0.1.6:/usr/local/cuda-12.4 \
tools/build_linux_app_drop.sh \
  --build-dir build/linux-app-drop-opencv-ffmpeg-nvidia-hybrid \
  --clean-install \
  --bundle-opencv-ffmpeg \
  --runtime-check-mode release \
  --write-dependency-manifest dist/Crimson/dependency_manifest.json
```

`--bundle-opencv-ffmpeg` copies `libopencv*.so*`, `libav*.so*`, and
`libsw*.so*` into `dist/Crimson/lib/crimson/private`. It also omits the bundled
OpenCV/FFmpeg roots from `dist/Crimson/etc/crimson/runtime_roots.env`, so the
launcher only carries the remaining managed roots such as TensorRT and CUDA.

The build helper can also run the default developer check during staging:

```bash
tools/build_linux_app_drop.sh \
  --dependency-manifest dist/Crimson/dependency_manifest.json
```

Use `CRIMSON_ALLOWED_RUNTIME_ROOTS` only when a release intentionally depends on
an admin-managed module/runtime root instead of app-local bundled libraries.

Current limitation: the hybrid app drop still carries absolute managed roots
for TensorRT and CUDA, and the executable may still contain absolute `RUNPATH`
entries from imported/prebuilt link metadata. The launcher and release audit
prefer app-local libraries first, but this is not yet the final relocatable
user-install model.

The next hardening slice should extend the bundle boundary to TensorRT and CUDA
runtime libraries where redistribution and driver compatibility are clear, then
reduce the executable `RUNPATH` to `$ORIGIN` entries plus documented system
driver expectations.

## Next Slice: Bundle Audit Before Bundling

Do not start by blindly copying every shared library reported by `ldd`.
Instead, add a bundle-audit mode first.

Goals:

1. Produce a dependency manifest for the staged `bin/redgui`.
2. Classify each dependency as:
   - `system`
   - `driver`
   - `bundle-candidate`
   - `unexpected`
3. Detect mixed dependency roots, especially OpenCV/FFmpeg mismatches.
4. Fail release-mode checks if a core private dependency resolves from an
   uncontrolled absolute path.
5. Keep developer-mode checks as warnings so local builds remain convenient.

Initial classification policy:

| Class | Examples | Release behavior |
| --- | --- | --- |
| `driver` | `libcuda.so.1`, NVIDIA driver-owned GL/driver libraries | require from host |
| `system` | glibc, libstdc++, pthread, dl, common Ubuntu base libraries | require from host |
| `bundle-candidate` | TensorRT, OpenCV, FFmpeg, NPP/CUDA-adjacent runtime libraries after redistribution review | copy/select deliberately |
| `unexpected` | libraries from user home paths, wrong `/opt` tree, mismatched FFmpeg/OpenCV stack | warn in dev, fail in release |

Implementation checklist:

1. Extend `check_crimson_runtime.sh` with `--mode dev|release`, defaulting to
   `dev`.
2. Add `--write-dependency-manifest <path>` that records:
   - library soname
   - resolved path
   - classification
   - owning root
   - whether it is allowed in release mode
3. Add root consistency checks for:
   - OpenCV libraries
   - FFmpeg libraries
   - TensorRT libraries
   - CUDA/NPP runtime libraries
4. Make release mode fail on absolute private dependency roots unless the root
   is explicitly allowed by policy.
5. Only after the manifest is stable, add a packaging copy step that copies
   `bundle-candidate` libraries into `lib/crimson/private`.
6. Re-run `ldd` after copying and verify the final app drop resolves the copied
   libraries through `$ORIGIN` paths.
7. Run GUI smoke from `dist/Crimson/bin/crimson`, not from the build tree.

Then validate on at least:

- the current development workstation
- one clean Ubuntu workstation with a compatible NVIDIA driver
- one laptop/workstation that does not have the builder's `/opt` layout

Only after that should Crimson choose between shared internal release folders,
tarballs, AppImage, or `.deb`.
