# Crimson macOS Phase 0 Baseline and Parity Inventory

Date: 2026-07-12

Lifecycle: **archive-ready historical baseline**. Keep this path until the
README and port-roadmap links are migrated; it is not an active parity plan.

This document records the baseline used for the first native Apple Silicon
build slice. It is an inventory and validation contract, not a claim that the
macOS application has production feature parity.

## Scope

Phase 0 freezes the existing NVIDIA behavior and identifies the source and test
surfaces that later backend work must preserve. Phase 1 only makes CUDA
conditional and adds a native GLFW/Cocoa, ImGui, ImPlot, and Metal application
shell. Decode, Zarr integration, overlays, editing, and inference remain on the
existing NVIDIA application until their later phases are reviewed.

## Repository Baseline

| Item | Recorded value |
| --- | --- |
| Branch | `codex/ui-monolith-20260404181029` |
| Baseline commit | `9b80054` (`macos: add Apple Silicon feasibility plan and probes`) |
| Upstream | `origin/codex/ui-monolith-20260404181029` |
| Initial worktree | Clean |
| Maintained NVIDIA stacks | Linux and Windows, CUDA 12.4, TensorRT 10.0.1.6, OpenCV 4.10.0 |
| Linux GUI smoke evidence | `AGENTS.md`, 2026-06-21, frames 0 through 300 passed in 3.00624 s |
| Current Linux validation | Passed 2026-07-12 on `delahantyj-ws1` in a detached temporary worktree; details below |

The pre-port top-level project required CUDA before any option could be
evaluated, discovered CUDA/OpenGL/GLEW/FFmpeg/NVDEC dependencies globally, and
globbed all application C++ into one `redgui` executable. Therefore the
production target could not be configured on a host without CUDA even for
CPU-only helper targets.

## macOS Host Baseline

| Item | Recorded value |
| --- | --- |
| Hardware architecture | Apple Silicon `arm64` |
| macOS | 26.3.1 (25D2128) |
| Compiler | AppleClang 17.0.0 |
| macOS SDK | 15.5, Command Line Tools |
| CMake | 4.4.0 |
| Ninja | 1.13.2 |
| GLFW | Homebrew 3.4 |
| FFmpeg | Homebrew 8.1.2_1; probe comparison only, not required by Phase 1 |
| UI dependencies | Repository-pinned ImGui, ImPlot, ImGuiFileDialog, and font-header submodules |

The representative June 14 recording and analysis Zarr are mounted under:

```text
/Volumes/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop
```

No video, Zarr archive, model binary, generated probe result, or app bundle is
committed to this repository.

## Platform Coupling Inventory

| Area | Current NVIDIA source | Contract to preserve | Later Apple work |
| --- | --- | --- | --- |
| Build graph | `CMakeLists.txt`, `CMakePresets.json` | Existing Linux/Windows presets keep CUDA/NVDEC/OpenGL/TensorRT defaults | Conditional language/dependencies and explicit Apple target |
| Demux and timestamps | `src/FFmpegDemuxer.*` | Stream start PTS, rational time conversion, keyframe seek behavior | AVFoundation track time and explicit timebase metadata |
| Decode | `src/NvDecoder.*`, `src/decoder.*` | Global/local frame identity, PTS, seek settlement, bounded slots | AVFoundation/VideoToolbox provider in Phase 3 |
| Frame lifetime | `src/frame_slot.*` | Acquire, publish, read, release, cancel, multi-reader blocking | Neutral metadata/surface ownership in Phase 2 |
| Logical clock | `src/red.cpp`, `src/playback_session_controller.*` | Crimson clock owns requested frame; actual presented frame is committed | Shared clock remains authoritative |
| Presentation | `src/render.h`, `src/gx_helper.h`, `src/gui/camera_view_presenter.*` | Single GUI-thread renderer, front/staging ordering, post-present release | Metal texture and command ownership in Phases 2-5 |
| Stimulus | `src/stimulus_playback.*`, `src/zarr_loader_stimulus.cpp` | Camera frame maps through corrected/legacy Zarr precedence | Native secondary surfaces driven by the camera clock |
| Crop/clipped media | `src/media_session_loader.*`, `src/zarr/palette_clipped_resolver.*` | Parent, recording, and clip-local identities remain distinct | Native provider and exact boundary handoff |
| Color | `src/ColorSpace.cu`, `src/gui/camera_view_presenter.cpp` | Matrix and limited/full/unspecified range metadata | Metal conversion with the same declared metadata |
| Zarr | `src/zarr_loader*.cpp`, `src/zarr/*.cpp` | Discovery, latest-run selection, schemas, dtypes, dimensions, and write destinations | Remains backend-neutral; no Phase 1 changes |
| Inference | `src/yolo_detection.*`, `src/yolov8_det.*`, `src/yolov8_pose.*` | Preprocess, decode, NMS, boxes, keypoints, masks, and consumers | Actual-model backend comparison in Phase 6 |
| Packaging/resources | CMake install rules, `src/ui_path_config.*` | Executable-relative Linux/Windows resource behavior | `.app/Contents/Resources` lookup and bundle validation |

Current shared headers are not backend-neutral. `decoder.h`, `ColorSpace.h`,
`render.h`, `gx_helper.h`, the stimulus types, presenter types, and inference
headers expose CUDA, NVDEC, OpenGL, NPP, or TensorRT details. Phase 1 does not
attempt to hide those types; it prevents the Apple shell from compiling that
source graph. The interface work begins only after the Phase 1 architecture
review.

## User-Visible Parity Inventory

| Workflow | Primary source locations | Current validation | Gap before parity |
| --- | --- | --- | --- |
| Open recording/Zarr | `media_session_loader.*`, `zarr_loader_internal.*`, `ui_path_config.*` | External real-session use | No checked-in discovery fixture |
| Main camera playback | `red.cpp`, `decoder.*`, `camera_view_presenter.*` | NVIDIA GUI playback smoke | No Mac production viewer yet |
| Crop and multiview | `crop_image_provider.h`, `live_crop_image_provider.*`, `crop_preview_window.*` | Manual/external data | No synchronized automated fixture |
| Stimulus playback | `stimulus_playback.*`, `stimulus_playback_windows.*` | External stimulus smoke and Mac probes | No production Mac integration |
| Pause/resume | `playback_session_controller.*`, transport controls | NVIDIA smoke/manual use | No focused controller test |
| Random seek and stepping | controller, decoder seek paths | Mac AVFoundation probe and manual NVIDIA use | Production exact-identity test missing |
| Clipped handoff | `palette_clipped_resolver.*`, session loader | External clipped canary | No checked-in clipped fixture |
| Main/crop/stimulus sync | playback controller and Zarr stimulus mapping | Portable Mac mapped-access probe | No simultaneous production assertion |
| Zoom/pan/rotation | camera view/window/presenter and crop preview | Manual GUI use and perf logs | No transform golden fixture |
| Boxes/keypoints/skeletons | overlay renderer and keypoint modules | Manual review contracts | No screenshot/coordinate golden |
| Masks/contours/eye overlays | mask, subject-shape, and eye overlay modules | External contour/mask smokes | No pixel/coordinate golden |
| Stimulus overlays | camera stimulus overlay and timelines | External timeline data | No mapping/click target unit test |
| Analysis plots/timelines | `src/gui/analysis_timeline_*` | Manual and perf smoke | No behavior fixture |
| Bounding-box editing | `zarr_bbox_edit.*`, `zarr_loader_write.cpp` | Manual workflows | Writer identity/atomicity coverage missing |
| Refined keypoint review/write | refined repository and review/write panels | Written read/write contracts | No miniature write fixture |
| Subject-mask editing/writeback | subject mask edit/writeback modules | Preview/manual workflow | Service mode and portability incomplete |
| Detection/review acceptance | review panels and loader contracts | Contract documents | No end-to-end automated fixture |
| Inference | YOLOv5 OpenCV CUDA and YOLOv8 TensorRT paths | Platform-dependent manual use | No committed models, provenance, or tolerance fixtures |
| Diagnostics/performance | diagnostics, frame debug, perf logging | Existing logs and smoke hooks | Backend-neutral metric contract missing |
| Packaged launch | CMake install and platform scripts | Linux/Windows staging procedures | Mac signing/notarization is Phase 7 |

## Existing Automated and Smoke Coverage

- `frame_slot_tests`: write/read leases, cancel, multiple readers, threaded
  publication/reuse, and a local nearest-slot helper. Its header dependency is
  still CUDA-coupled, so it is built only with the NVIDIA graph for now.
- `tensorstore_zarr3_check`: in-memory Zarr v3 driver registration. Maintained
  presets currently disable the configure-time probe.
- `crimson_decode_smoke`: sequential NVIDIA decode throughput. It does not
  cover seek, color, frame identity, or presentation.
- `scripts/gui_smoke_playback.sh`: authenticated X display, CUDA/OpenGL interop,
  and presenter settlement at the requested end frame.
- Additional external scripts cover stimulus inset, clipped boundaries, compact
  timelines, and sampled contours. They are not substitutes for checked-in
  miniature fixtures.

There are no focused automated tests for production frame selection,
post-present commit/release ordering, controller seek/resume behavior, Zarr
mapping precedence, overlay coordinates, color conversion pixels, writers,
inference, or clean-machine packaging.

## Test Expansion Strategy

The maintained test structure should use GUI smokes only for behavior that
requires an actual window system:

1. Pure C++ contract tests: requested/decoded/presented frame selection,
   global/local identity, mapping precedence, clipped boundaries, color
   metadata, and seek state transitions.
2. Backend-neutral lifetime tests: fake frame surfaces and presentation handles
   exercising acquire, publish, retain, release, cancellation, and device-error
   paths.
3. Headless backend tests: real Metal command queues and offscreen textures,
   shader/conversion output, ImGui rendering, and pixel readback without a
   Cocoa window.
4. Windowed smokes: Cocoa/GLFW initialization, drawable acquisition, resize and
   input plumbing, real presentation, and app-bundle launch.

Phase 1 implements layers 3 and 4 for the shell. The existing frame-slot test
is an early layer-2 test but must be separated from `decoder.h` in Phase 2
before it is genuinely backend-neutral. Production playback and Zarr contract
tests are added with the interfaces and fixtures in later phases, not by
copying current `red.cpp` logic into an unreviewed test-only abstraction.

## Golden Data and Model Status

Measured starting evidence is recorded in
`docs/crimson_ubuntu_macos_platform_strategy.md` and
`experiments/macos_video_probe/README.md`:

- representative 4512 x 4512, 100 fps HEVC main camera;
- corresponding 256 x 256 crop;
- controlled approximately 120 fps H.264 stimulus derivative;
- portable real-Zarr camera-to-stimulus mapping export;
- exact AVFoundation access and stepping;
- simultaneous aligned Metal probe capability.

The original incompatible stimulus remains a source-data compatibility case,
not something Crimson may silently rewrite. Model engines are resolved beneath
recording roots by current runtime code. No portable `.onnx`/`.pt` inventory or
TensorRT engine provenance is checked in; that must be collected before Phase
6 begins.

## Baseline Discrepancies to Resolve

These are confirmed inventory findings, not Phase 1 behavior changes:

1. Production `extractLatestRunName` prefers `latest`, then
   `latest_completed`, `latest_complete`, and `latest_success`. The portable
   alignment exporter prefers completion pointers first and omits
   `latest_success`.
2. Some recording/video/stimulus filesystem fallbacks take the first directory
   entry and therefore lack deterministic ordering.
3. The current stimulus loader still requires legacy alignment arrays even
   when direct corrected mapping is present.
4. Decoder recreation on seek is disabled unless
   `CRIMSON_RECREATE_DECODER_ON_SEEK` is enabled, while an older design note
   describes recreation as the default.
5. `config/ui_paths.json` contains a Linux-local `/nvme1` default. Missing paths
   are filtered at runtime, but the packaged default is not useful on macOS.

Changing any of these can affect scientific or user-visible behavior and
requires a separate reviewed contract change.

## Phase 1 macOS Validation Record

Commands:

```bash
git submodule update --init --recursive
cmake --preset macos-arm64-release --fresh
cmake --build --preset build-macos-arm64-release
ctest --preset test-macos-arm64-headless
ctest --preset test-macos-arm64
```

Recorded on the host above:

- configure succeeded with `CRIMSON_BUILD_NVIDIA_APP=OFF` and
  `CRIMSON_BUILD_MACOS_APP=ON`;
- the cache contains no CUDA compiler or NVIDIA/OpenGL dependency entries;
- `build/macos-arm64-release/Crimson.app/Contents/MacOS/Crimson` is a native
  arm64 Mach-O;
- linked non-system runtime dependency is Homebrew GLFW 3.4;
- linked frameworks are Cocoa, Metal, and QuartzCore; no OpenGL framework is
  linked;
- `crimson_macos_metal_headless` passed after rendering ImGui and ImPlot to a
  384 x 240 shared Metal texture and reading 92,156 nonblack pixels;
- `crimson_macos_shell_smoke` passed after presenting 12 completed Metal command
  buffers through a real GLFW/Cocoa window;
- both tracked fonts were copied under
  `Crimson.app/Contents/Resources/fonts` and the bundled Roboto font resolved at
  runtime;
- `cmake --install` produced a staged `Crimson.app`; both headless rendering and
  the real-window smoke passed from the staged copy.
- the isolated `experiments/macos_video_probe` project configured from a clean
  temporary build directory and built all four existing Objective-C++ targets:
  FFmpeg/VideoToolbox, AVFoundation/Metal, AVFoundation feasibility, and aligned
  playback.

The shell is intentionally linked to the Homebrew GLFW dylib. Embedding and
signing non-system libraries, hardened runtime configuration, notarization, and
clean-machine distribution remain Phase 7 packaging work.

## NVIDIA Validation Commands

Run these on an available maintained NVIDIA Linux host after the build split:

```bash
CUDACXX="$CRIMSON_CUDA_TOOLKIT_ROOT/bin/nvcc" \
cmake --preset linux-trt10-cuda12.4-release
cmake --build --preset build-linux-trt10-cuda12.4-release
ctest --test-dir build/linux-trt10-cuda12.4-release --output-on-failure
```

Then run the authenticated GUI playback smoke from `AGENTS.md`:

```bash
CRIMSON_PLAYBACK_SMOKE_ZARR=/groups/johnson/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop/zarr/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop_analysis.zarr \
CRIMSON_PLAYBACK_SMOKE_RANGE=0:300 \
scripts/gui_smoke_playback.sh
```

Acceptance requires the configure summary to retain CUDA 12.4, architectures
80/86, NVDEC/CUVID, OpenGL/GLEW, and TensorRT behavior for the maintained
presets. The smoke must report the requested end frame as presented.

### NVIDIA Validation Record

Validation host:

| Item | Recorded value |
| --- | --- |
| Host | `delahantyj-ws1.hhmi.org`, Ubuntu Linux 6.8.0-134 |
| GPU | NVIDIA RTX A6000, 49,140 MiB |
| Driver | 580.173.02 |
| Compiler | GNU C++ 13.3.0 and NVCC 12.4.99 |
| Validation checkout | Detached worktree from baseline `9b80054` with only the Phase 0/1 files copied in |

The maintained release preset configured with:

- `CRIMSON_BUILD_NVIDIA_APP=ON` and `CRIMSON_BUILD_MACOS_APP=OFF`;
- CUDA 12.4 and architectures 80/86;
- TensorRT 10.0.1.6;
- OpenCV 4.10.0 with SFM enabled;
- NVIDIA FFmpeg/NVDEC/CUVID;
- OpenGL, GLEW, GLFW, NVML, NPP, and the existing TensorStore build.

The complete 129-step build succeeded, including `redgui`, all three CUDA
translation units, `crimson_decode_smoke`, `frame_slot_tests`, and the enabled
Zarr helper targets. `frame_slot_tests` passed under CTest.

The authenticated GUI playback smoke used the representative June 14 Zarr and
the current working `DISPLAY=:1` Xwayland authority pair. Result:

```text
[PlaybackSmoke] PASS start_frame=0 end_frame=300 presented_frame=300 presented_slot=0 view_idx=0 presented_count=351 elapsed_s=2.99366
```

The non-interactive SSH shell required `CUDACXX` and the maintained runtime
library roots to be provided explicitly. Interactive workstation shells and
the staged Linux launcher normally provide those paths. The build retained
pre-existing deprecation warnings from FFmpeg, CUDA GL interop, and TensorRT,
plus existing OpenCV/FFmpeg SONAME conflict warnings at final link. No new
compile or link failure was introduced by the backend selection change.

## Phase Boundary

Phase 1 is accepted only when the existing NVIDIA presets explicitly select the
NVIDIA graph, macOS configures without any NVIDIA/OpenGL dependencies, the
native arm64 app bundle builds, bundled resources resolve, and an actual Metal
drawable is presented. This is a build/backend skeleton. No viewer, overlay,
editing, Zarr, or inference parity is claimed.
