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

In this document, "NVIDIA codec support" currently means hardware-assisted
playback/decode for the Crimson GUI. The maintained GUI app-drop needs the
NVDEC/CUVID side of the NVIDIA video stack to read H.264/H.265 recordings. It
does not currently encode video during normal playback.

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

## Decode Path vs Future Encode Path

The current Crimson GUI playback path is a decode path:

- FFmpeg/OpenCV select and open the recording containers.
- Crimson's NVIDIA decode path uses NVDEC/CUVID APIs from the driver-provided
  `libnvcuvid.so.1`.
- CUDA/GL interop and CUDA/NPP runtime libraries support upload, conversion,
  inference, and rendering.

NVENC is the NVIDIA encode API. It is not required for today's normal GUI
playback app-drop unless a built target actually references NVENC encode
symbols. Crimson's default build leaves `CRIMSON_ENABLE_NVENC=OFF`, so it does
not require or link `libnvidia-encode.so.1` for playback. Enable
`CRIMSON_ENABLE_NVENC` only for a future encode/export target that actually
uses NVIDIA's encode API.

A future "export annotated clip" component should be treated as a separate
encoding path. That feature would likely render frames with masks, keypoints,
tracks, and other overlays, then write an output movie using either FFmpeg's
NVENC encoder or direct NVENC APIs. When that component is implemented:

- `libnvidia-encode.so.1` becomes a runtime driver dependency for the export
  binary or plugin.
- The runtime checker should add an encode/export smoke test, not only an
  `ldd` check.
- Packaging should keep `libnvidia-encode.so.1` host-resolved like
  `libcuda.so.1` and `libnvcuvid.so.1`, because it is tied to the installed
  NVIDIA driver.
- The dependency manifest should distinguish current playback/decode
  requirements from optional clip-export/encode requirements.

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

## Runtime Checker

Linux now has `tools/check_crimson_runtime.sh`, installed into each app drop as
`check_crimson_runtime.sh`.

It reports:

- Crimson release metadata and install root
- `redgui` exists and is executable
- `ldd` unresolved dependencies
- which paths resolve TensorRT, OpenCV, FFmpeg, CUDA runtime, and NPP
- `nvidia-smi` GPU name and driver version
- OpenGL/GLX sanity through `glxinfo` or a small GLFW probe
- whether the selected driver satisfies the release's documented minimum
- whether required resources under `share/crimson` exist
- whether a GUI smoke can launch when a real display is available

This checker is part of every app drop and runs in the Linux publish helper
unless explicitly skipped.

## Stability Work Needed

1. [done] Add Linux build/stage helper:
   `tools/build_linux_app_drop.sh`.
2. [done] Add Linux runtime checker:
   `tools/check_crimson_runtime.sh`.
3. [done] Add Linux publish helper after the staged tree is proven:
   `tools/publish_linux_app_drop.sh`.
4. [done] Add Linux build/check/publish chain helper:
   `tools/build_check_publish_linux_app_drop.sh`.
5. Ensure `cmake --install` stages all required resources under one prefix.
6. Confirm executable-relative resource lookup for fonts, config, and models.
7. Confirm RPATH points to install-relative private library directories.
8. Decide which third-party libraries are bundled versus system/module
   requirements.
9. Record release metadata:
   commit, branch, preset, dependency versions, CUDA architectures, minimum
   driver, build host, build time.
10. Run GUI smoke from the staged install, not only from the build tree.
11. Update `CMAKE_CUDA_ARCHITECTURES` before broad release. The current source
    sets `80;86`, which covers Ampere-class targets but does not explicitly
    include Ada RTX 40-series `sm_89`.
12. [done] Make the CMake NVENC dependency optional until an encode/export
    target actually needs it.
13. [done] Add Linux user installer:
    `tools/install_crimson.sh`.

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
tools/build_check_publish_linux_app_drop.sh
tools/check_crimson_runtime.sh
tools/crimson_linux_launcher.sh
tools/publish_linux_app_drop.sh
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
  --dependency-manifest dist/Crimson/dependency_manifest.json
```

`--bundle-opencv-ffmpeg` copies `libopencv*.so*`, `libav*.so*`, and
`libsw*.so*` into `dist/Crimson/lib/crimson/private`. It also omits the bundled
OpenCV/FFmpeg roots from `dist/Crimson/etc/crimson/runtime_roots.env`, so the
launcher only carries the remaining managed roots such as TensorRT and CUDA.
Managed roots are expanded at launch/audit time to common library directories
such as `lib`, `lib64`, and `targets/x86_64-linux/lib`.
The app-drop helper also cleans the installed executable `RUNPATH` to:

```text
$ORIGIN:$ORIGIN/../lib:$ORIGIN/../lib/crimson/private
```

Release-mode checks fail if absolute `RUNPATH` entries remain.

The build helper can also run the default developer check during staging:

```bash
tools/build_linux_app_drop.sh \
  --dependency-manifest dist/Crimson/dependency_manifest.json
```

Use `CRIMSON_ALLOWED_RUNTIME_ROOTS` only when a release intentionally depends on
an admin-managed module/runtime root instead of app-local bundled libraries.

Publish the staged app drop to a shared release root:

```bash
tools/publish_linux_app_drop.sh \
  --stage-root dist/Crimson \
  --share-root /groups/ahrens/ahrenslab/crimson/linux-app \
  --publish-current \
  --archive-existing-current
```

This validates the staged layout, runs `check_crimson_runtime.sh --mode release`
against the staged app, writes `dependency_manifest.json`, creates a versioned
release under `releases/<release-name>`, refreshes `current` when requested, and
writes `latest.json` at the publish root.

To build, check, and publish in one command:

```bash
CRIMSON_ALLOWED_RUNTIME_ROOTS=/usr/local/TensorRT-10.0.1.6:/usr/local/cuda-12.4 \
tools/build_check_publish_linux_app_drop.sh \
  --build-dir build/linux-app-drop-opencv-ffmpeg-nvidia-hybrid \
  --clean-install \
  --bundle-opencv-ffmpeg \
  --share-root /groups/ahrens/ahrenslab/crimson/linux-app \
  --publish-current \
  --archive-existing-current
```

For the current validated bundled Linux app-drop defaults, use the preset
wrapper instead of typing the full command:

```bash
tools/publish_linux_current_app_drop.sh
```

The wrapper defaults to publishing:

```text
/groups/ahrens/ahrenslab/crimson/linux-app/current
/groups/ahrens/ahrenslab/crimson/linux-app/releases/<release-name>
```

It bundles OpenCV, FFmpeg, TensorRT, and CUDA/NPP runtime libraries into the
app drop, while keeping NVIDIA driver libraries host-resolved. To inspect the
delegated command without publishing:

```bash
tools/publish_linux_current_app_drop.sh --dry-run
```

Use `--share-root <path>` to publish to a different group location, for example
a Johnson lab staging area.

User install from a published app drop is handled by the Linux installer script:

```bash
/groups/ahrens/ahrenslab/crimson/linux-app/current/install_crimson.sh \
  --replace-existing \
  --create-symlink
```

By default this copies the app drop to:

```text
~/.local/share/Crimson
```

and `--create-symlink` creates:

```text
~/bin/crimson -> ~/.local/share/Crimson/bin/crimson
```

The installer runs `check_crimson_runtime.sh --mode release` against the source
app drop before copying and against the installed copy afterward. Use
`--require-nvidia-smi` and `--require-gl` when installing on the target machine
and you want the install to fail unless the local NVIDIA/display stack is ready.
Use `--source-root <path>` to install from a staging or test app drop instead
of the default published current root.

Current default limitation: the hybrid app drop still carries absolute managed
roots for TensorRT and CUDA in `etc/crimson/runtime_roots.env`. OpenCV and
FFmpeg are app-local, and the executable `RUNPATH` is install-relative, but this
default path is not yet the final relocatable user-install model.

There is also an explicit experimental NVIDIA runtime bundle path. It copies
the observed TensorRT and CUDA/NPP runtime libraries into the app drop and
keeps the NVIDIA driver libraries host-resolved. Treat this as a hardening and
validation path until redistribution terms, GUI smoke, decode smoke, and
TensorRT inference smoke are all signed off.

## NVIDIA Runtime Redistribution Review

Vendor software being free to download does not automatically mean Crimson can
redistribute the same binaries. End users downloading CUDA or TensorRT directly
accept NVIDIA's license terms at the source. If Crimson copies those binaries
into an app drop, Crimson becomes part of the distribution chain and must follow
the vendor's redistribution terms, third-party notice requirements, export
requirements, and support boundaries.

For CUDA, the public CUDA EULA lists redistributable Linux runtime components in
Attachment A. That list includes CUDA runtime and NPP libraries such as
`libcudart.so`, `libnppc.so`, `libnppig.so`, `libnppidei.so`, and
`libnppial.so`, subject to the EULA's distribution requirements. The EULA is at:

```text
https://docs.nvidia.com/cuda/eula/index.html
```

TensorRT is less clear from the local package alone. The installed TensorRT
README points to NVIDIA's TensorRT software license agreement:

```text
/usr/local/TensorRT-10.0.1.6/doc/Readme.txt
https://docs.nvidia.com/deeplearning/tensorrt/sla/
```

Before publishing a Crimson app drop outside controlled internal machines,
confirm whether TensorRT runtime libraries may be redistributed with Crimson and
which notices must accompany them. Keep NVIDIA driver libraries external even if
some license text allows redistribution: `libcuda.so.1`, `libnvcuvid.so.1`, and
related driver libraries are tied to the installed driver/kernel stack and
should come from the host.

Current hybrid external bundle candidates from the release manifest are:

| Library | Current source | Notes |
| --- | --- | --- |
| `libnvinfer.so.10` | `/usr/local/TensorRT-10.0.1.6/lib` | TensorRT runtime; confirm redistribution terms |
| `libnvinfer_plugin.so.10` | `/usr/local/TensorRT-10.0.1.6/lib` | TensorRT plugin runtime; confirm redistribution terms |
| `libnppc.so.12` | `/usr/local/cuda-12.4/lib64` | CUDA/NPP runtime; appears in CUDA redistributable list |
| `libnppig.so.12` | `/usr/local/cuda-12.4/lib64` | CUDA/NPP runtime; appears in CUDA redistributable list |
| `libnppidei.so.12` | `/usr/local/cuda-12.4/lib64` | CUDA/NPP runtime; appears in CUDA redistributable list |
| `libnppial.so.12` | `/usr/local/cuda-12.4/lib64` | CUDA/NPP runtime; appears in CUDA redistributable list |

Do not bundle:

| Library | Reason |
| --- | --- |
| `libcuda.so.1` | NVIDIA driver API library; must match installed host driver |
| `libnvcuvid.so.1` | NVIDIA video decode driver library; must match installed host driver |
| `libnvidia-encode.so.1` | NVIDIA video encode driver library; future clip export dependency, not current playback requirement |

## Experimental NVIDIA Runtime Bundle

The app-drop helper has an explicit flag for this path. It is intentionally not
default release behavior yet:

```bash
tools/build_linux_app_drop.sh \
  --bundle-opencv-ffmpeg \
  --bundle-nvidia-runtime \
  --runtime-check-mode release \
  --dependency-manifest dist/Crimson/dependency_manifest.json
```

Experimental design:

1. Copy only the observed runtime closure into
   `dist/Crimson/lib/crimson/private`:
   - `libnvinfer.so*`
   - `libnvinfer_plugin.so*`
   - `libnppc.so*`
   - `libnppig.so*`
   - `libnppidei.so*`
   - `libnppial.so*`
2. Preserve symlink chains with `cp -a`.
3. Copy NVIDIA license/notice material into `dist/Crimson/share/crimson/legal/`
   where available:
   - CUDA EULA or local CUDA notices
   - TensorRT `doc/Readme.txt`
   - TensorRT `doc/Acknowledgements.txt`
4. Omit `/usr/local/TensorRT-10.0.1.6` and `/usr/local/cuda-12.4` from
   `etc/crimson/runtime_roots.env` if every non-driver NVIDIA runtime library
   resolves from `lib/crimson/private`.
5. Keep `libcuda.so.1`, `libnvcuvid.so.1`, and future encode-path
   `libnvidia-encode.so.1` host-resolved and classify them as `driver` in the
   dependency manifest.
6. Validate with more than `ldd`:
   - release runtime audit with `LD_LIBRARY_PATH` and
     `CRIMSON_ALLOWED_RUNTIME_ROOTS` unset
   - decode smoke that exercises `libnvcuvid.so.1`
   - TensorRT inference smoke that loads a real engine/model path used by
     Crimson
   - GUI smoke on an authenticated X display

The one-command build/check/publish wrapper forwards the same flag:

```bash
tools/build_check_publish_linux_app_drop.sh \
  --bundle-opencv-ffmpeg \
  --bundle-nvidia-runtime \
  --share-root /groups/ahrens/ahrenslab/crimson/linux-app \
  --publish-current \
  --archive-existing-current
```

When `--bundle-nvidia-runtime` is enabled, the build helper runs the runtime
checker with ambient `CRIMSON_ALLOWED_RUNTIME_ROOTS` unset. This catches
accidental success caused by the developer shell still pointing at `/usr/local`
or `/opt`.

Blockers that would stop promotion from experimental to release:

- TensorRT redistribution terms are not approved for Crimson's distribution
  path.
- Required NVIDIA runtime libraries are loaded lazily and missing from the
  initial manifest.
- Bundled TensorRT is incompatible with Crimson's serialized engines.
- The user's NVIDIA driver is older than the bundled CUDA/TensorRT runtime
  requires.
- Package size is unacceptable for the deployment channel.

## Bundle Audit Policy

The initial family-only bundle was insufficient on a second Ubuntu workstation:
OpenCV resolved from the app, but its transitive Ceres and cuDNN dependencies
did not. cuBLAS and OpenCL then resolved from that workstation's unrelated CUDA
12.2 installation. A release app drop must therefore inspect the complete
resolved `ldd` graph, not only the executable's direct dependencies.

`--bundle-runtime-closure` copies the resolved non-platform closure into
`lib/crimson/private` and records the decision in
`share/crimson/runtime_closure.txt`. It deliberately retains these host
boundaries:

- glibc and the Linux dynamic loader;
- the Ubuntu 22 C++ ABI baseline (`libstdc++` and `libgcc_s`);
- X11, OpenGL/GLX, and DRM platform libraries; and
- NVIDIA driver libraries, including `libcuda.so.1` and `libnvcuvid.so.1`.

An unfamiliar library outside the app remains a release failure. An unfamiliar
library deliberately copied under `lib/crimson/private` is accepted and remains
visible in the structured dependency manifest.

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
| `driver` | `libcuda.so.1`, `libnvcuvid.so.1`, future `libnvidia-encode.so.1`, NVIDIA driver-owned GL/driver libraries | require from host |
| `system` | glibc, libstdc++, pthread, dl, common Ubuntu base libraries | require from host |
| `bundle-candidate` | TensorRT, OpenCV, FFmpeg, NPP/CUDA-adjacent runtime libraries after redistribution review | copy/select deliberately |
| `unexpected` | libraries from user home paths, wrong `/opt` tree, mismatched FFmpeg/OpenCV stack | warn in dev, fail in release |

Current status:

1. [done] Extend `check_crimson_runtime.sh` with `--mode dev|release`,
   defaulting to `dev`.
2. [done] Add `--write-dependency-manifest <path>` that records:
   - library soname
   - resolved path
   - classification
   - owning root
   - whether it is allowed in release mode
3. [done] Add root consistency checks for:
   - OpenCV libraries
   - FFmpeg libraries
   - TensorRT libraries
   - CUDA/NPP runtime libraries
4. [done] Make release mode fail on absolute private dependency roots unless
   the root is explicitly allowed by policy.
5. [done] Add guarded packaging copy steps for the known OpenCV/FFmpeg and
   TensorRT/NPP bundle-candidate families.
6. [done] Add a non-platform transitive-closure bundle for relocatable drops.
7. [done] Re-run `ldd` after copying and verify the final app drop resolves
   the copied libraries through `$ORIGIN` paths during publish checks.
8. [todo] Run GUI smoke from the staged or published app drop, not only from
   the build tree.

## Ubuntu 22 Builder: Approved Image and Local Rebuilds

Linux releases use Ubuntu 22.04/glibc 2.35 as the compatibility baseline even
when the build host runs a newer distribution. The builder definition is:

```text
packaging/linux/ubuntu22-cuda12.4-trt10/CrimsonLinuxBuilder.def
```

The recipe names the NGC TensorRT 24.05 base and the CUDA 12.4 / TensorRT
10.0.1.6 stack, pins CMake 3.30.5 by archive SHA-256, and pins exact
OpenCV/OpenCV-contrib 4.10 commits. OpenCV is built without fast math for
`sm_80` and `sm_86` and includes the CUDA, DNN, video, and SFM modules required
by Crimson.

The repository lock at
`packaging/linux/ubuntu22-cuda12.4-trt10/CrimsonLinuxBuilder.lock.json`
identifies **one approved SIF artifact**, not the definition file. Rebuilding
or resealing an image is not promised to reproduce identical bytes. The recipe
also uses a base-image tag and distribution packages rather than an entirely
content-addressed dependency closure. A new SIF can have a different hash while
claiming the same tool versions; those claims still need validation.

### Reuse the approved image

Obtain the approved SIF and its checksum from the maintainer through the
project's agreed file-sharing location. There is no automatic approved-image
download in the repository. A packaged Crimson application is not a builder
image. Keep the SIF outside Git and do not edit the checked-in lock just to make
an unrelated image pass.

The release wrapper verifies the sibling `<builder>.sha256` when present and
always verifies the SIF against the selected lock. A missing sidecar produces
a warning; it does not skip the mandatory lock comparison. Verify before
configuring or compiling:

```bash
crimson_builder="$HOME/crimson-builders/crimson-linux-ubuntu22-cuda12.4-trt10.sif"
tools/build_linux_release_in_apptainer.sh \
  --builder "$crimson_builder" --verify-builder-only
tools/build_linux_release_in_apptainer.sh \
  --builder "$crimson_builder" --jobs 8
```

Use a basename-relative checksum sidecar when sharing an image. The current
image-building helper may write the original absolute path in that file; after
copying, a checksum error mentioning the old path is not proof of corruption.
With `jq` installed, the following creates a portable sidecar from the trusted
repository lock (not from an unverified local image) and checks the copied bytes:

```bash
crimson_expected=$(jq -er '.builder_sif_sha256' \
  packaging/linux/ubuntu22-cuda12.4-trt10/CrimsonLinuxBuilder.lock.json)
printf '%s  %s\n' "$crimson_expected" "$(basename -- "$crimson_builder")" \
  > "${crimson_builder}.sha256"
(cd -- "$(dirname -- "$crimson_builder")" && \
  sha256sum -c "$(basename -- "$crimson_builder").sha256")
```

This receipt is only as trustworthy as the repository lock used to create it.
Keep the received checksum separately if it is needed for provenance.

### Build a local development image

Use a new output path so the approved image remains available. Building the
definition needs network access, substantial disk space and compilation time,
and Apptainer fakeroot support:

```bash
crimson_candidate="$HOME/crimson-builders/crimson-linux-ubuntu22-local.sif"
tools/build_linux_apptainer_builder.sh --output "$crimson_candidate"
```

Use `--sudo` only on an approved dedicated builder when fakeroot is unavailable.
A validated, user-owned sandbox can instead be sealed with
`--from-sandbox /path/to/validated-sandbox --output "$crimson_candidate"`.
Neither route automatically promotes its result to the repository-approved SIF.

For an intentional local development build, record a separate lock outside Git.
This example requires `jq` and records only the candidate's byte identity, not
an inherited approval timestamp or unmeasured dependency versions:

```bash
crimson_candidate_sha=$(sha256sum "$crimson_candidate" | awk '{print $1}')
jq -n --arg sha "$crimson_candidate_sha" \
  '{schema_id: "crimson_linux_builder_lock_v1",
    builder_id: "local-development-candidate",
    builder_sif_sha256: $sha,
    qualification_status: "unqualified_local_build"}' \
  > "${crimson_candidate}.lock.json"

tools/build_linux_release_in_apptainer.sh \
  --builder "$crimson_candidate" \
  --builder-lock "${crimson_candidate}.lock.json" --verify-builder-only
tools/build_linux_release_in_apptainer.sh \
  --builder "$crimson_candidate" \
  --builder-lock "${crimson_candidate}.lock.json" --jobs 8
```

`--builder-lock` deliberately selects a different expected artifact; it does not
disable hashing or prove compatibility. The wrapper currently consumes the
`builder_sif_sha256` field, not the informational qualification fields. Never
replace the tracked approved lock with a locally calculated hash merely to
bypass a mismatch. Keep the local lock, image checksum, source revision, and
build/test evidence with the resulting app; its application Git revision alone
does not identify the builder used. Promoting a candidate requires maintainer
review and explicit updates to the approved artifact and validation record.

### Early validation and its limits

The definition's `%test`, the build/sealing helper, and the release wrapper all
require the TensorRT runtime library **and readable regular files** for
`include/NvInfer.h` and `include/NvInferVersion.h`. A missing header, directory
in place of a header, or broken include symlink fails with the affected path
before Crimson's CMake configuration. These preflight checks also apply to an
existing approved SIF; adding them to the scripts does not require changing its
bytes or its lock.

Header presence is not an exact-version or successful-compilation guarantee.
CMake still reads `NvInferVersion.h` and enforces the pinned CUDA/OpenCV/TensorRT
versions. `--verify-builder-only` stops before host-driver discovery, CMake,
compilation, and GUI validation. The fixture tests for these preflight and hash
gates run without a GPU or an Apptainer installation.

The release wrapper never starts the GUI inside the builder. It uses the image
only for compilation and staging, binds the build host's NVIDIA driver
libraries for link-time symbol resolution, and does not copy those driver
libraries into the app. The resulting `Crimson/` directory is a normal Linux
application directory and must still pass `check_crimson_runtime.sh --mode
release` plus the GPU/decode/GUI smokes on the destination workstation.

Then validate on at least:

- the current development workstation
- one clean Ubuntu workstation with a compatible NVIDIA driver
- one laptop/workstation that does not have the builder's `/opt` layout

Only after that should Crimson choose between shared internal release folders,
tarballs, AppImage, or `.deb`.
