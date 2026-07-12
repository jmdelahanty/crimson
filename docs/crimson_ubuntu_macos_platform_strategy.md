# Crimson Ubuntu and macOS Platform Strategy

Purpose: summarize the practical build and distribution path for Ubuntu/Linux
and macOS, based on Crimson's current CUDA/TensorRT/OpenGL architecture.

Date anchored: 2026-07-06.

Related docs:

- [docs/crimson_supported_dependency_stack_matrix.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_supported_dependency_stack_matrix.md)
- [docs/crimson_cuda_driver_toolkit_and_presets.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_cuda_driver_toolkit_and_presets.md)
- [docs/crimson_packaging_and_distribution_plan.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_packaging_and_distribution_plan.md)
- [docs/crimson_linux_distribution_strategy.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_linux_distribution_strategy.md)
- [docs/crimson_windows_installation_procedures.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_windows_installation_procedures.md)

## Current Reality

Crimson is currently an NVIDIA-first desktop application:

- `CMakeLists.txt` declares `project(redgui LANGUAGES CXX CUDA)`.
- The maintained presets target CUDA `12.4`, TensorRT `10.0.1.6`, and OpenCV
  `4.10.0`.
- The video path links FFmpeg plus NVIDIA decode libraries such as `nvcuvid`.
- The inference path links TensorRT.
- The UI/rendering path uses GLFW, GLEW, ImGui, ImPlot, and OpenGL.
- CUDA/NPP code is compiled directly into the app.

This means "cross-platform" is not just a packaging question. Ubuntu and
Windows can share the NVIDIA dependency family. macOS cannot, because modern
macOS has no CUDA or TensorRT support.

## Ubuntu / Linux

Ubuntu is the tractable platform path. It is mostly a reproducibility,
packaging, and validation problem.

The baseline Linux stack already exists in shared presets:

```text
linux-trt10-cuda12.4-release
linux-trt10-cuda12.4-debug
```

Those presets should remain aligned with:

```text
CUDA Toolkit 12.4
TensorRT 10.0.1.6
OpenCV 4.10.0
FFmpeg/NVIDIA codec development root
```

Current workstations may expose those dependencies through `/opt`,
`/usr/local`, environment variables, or module-style setup. That is acceptable
for developers, but the user-facing Linux path should move toward a staged app
drop with executable-relative resources and controlled runtime library search.
See [docs/crimson_linux_distribution_strategy.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_linux_distribution_strategy.md)
for the concrete plan.

The near-term Ubuntu work should be:

1. Pick supported Ubuntu versions, likely Ubuntu 22.04 and/or 24.04.
2. Add a Linux equivalent of the Windows dependency path checker.
3. Verify clean configure/build/install from the shared Linux preset.
4. Make `cmake --install` produce a runnable staged tree.
5. Fix runtime resource lookup so fonts/config/models are executable-relative,
   not current-working-directory-relative.
6. Set Linux RPATH or launcher behavior so required private libraries are found
   from the install tree.
7. Add a GUI smoke command for the staged install.
8. Only after the staged tree is reliable, decide between a tarball, AppImage,
   `.deb`, or internal module-style deployment.

Ubuntu should not need a renderer rewrite. It should use the existing
CUDA/OpenGL/TensorRT architecture.

## macOS

macOS is a different product slice, not a simple port of the current build.

The blockers are structural:

- CUDA is unavailable on modern macOS.
- TensorRT is unavailable on macOS.
- NVDEC/CUVID and NVENC are unavailable on macOS.
- OpenGL is deprecated on macOS and should not be the long-term rendering
  foundation.
- Current CMake requires CUDA as a project language, so a macOS build would
  need a no-CUDA build variant before it can even configure.

MLX is not a rendering replacement. MLX is relevant to ML/tensor execution on
Apple Silicon. Rendering would need Metal or a Metal-backed UI/rendering layer.

A realistic macOS stack would likely require:

- Metal for image presentation and overlay rendering.
- VideoToolbox for hardware video decode.
- CoreML, MPS, MLX, or ONNX Runtime with a Metal/CoreML execution provider for
  inference, replacing TensorRT.
- A no-CUDA build variant.
- Backend abstractions for decode, inference, GPU buffers, and texture upload.

## Recommended macOS Slices

Do not target full parity first. Use staged scope.

### Slice 1: Archive Viewer

Goal: open useful Crimson/Palette data on macOS without live inference.

Scope:

- build with no CUDA language enabled
- load Zarr metadata and overlays
- display existing videos through CPU decode or VideoToolbox
- render overlays through a macOS-compatible UI/render path
- no TensorRT inference
- no CUDA/NPP processing
- no NVDEC/NVENC

This is still a meaningful project, but it avoids replacing every GPU path at
once.

### Slice 2: Accelerated Viewer

Goal: make viewing and random seeking feel native on Apple hardware.

Scope:

- VideoToolbox decode
- Metal texture presentation
- efficient CPU/GPU upload path for overlay data
- platform-independent frame and texture interfaces

### Slice 3: Inference Parity

Goal: replace the TensorRT-specific model execution path.

Possible backends:

- CoreML for packaged model deployment
- MPS or MLX for Apple Silicon tensor execution
- ONNX Runtime with an Apple backend if model conversion and performance are
  acceptable

This slice should only start after the viewer slice proves the app structure can
run without CUDA.

## VideoToolbox Feasibility Benchmark (2026-07-11)

A representative main-camera recording was tested on an Apple Silicon Mac to
reduce the largest decode-path uncertainty before implementation begins.

Test host:

- MacBook Air (`Mac15,12`)
- Apple M3: 8 CPU cores and 10 GPU cores
- 24 GB unified memory
- macOS 26.3.1
- Homebrew FFmpeg 8.0.1 with VideoToolbox enabled

Input recording:

```text
codec:       HEVC Main, yuv420p
dimensions:  4512 x 4512
frame rate:  100 fps
duration:    1434.47 seconds
frames:      143447
file size:   26,896,785,029 bytes
```

All decode modes and all four random-seek samples exited successfully. This
confirms that the representative HEVC stream is accepted by VideoToolbox on the
M3; codec compatibility is not presently a blocker.

Thirty-second throughput results:

| Decode/output path | Frames | Elapsed | Effective rate | Source-rate multiple |
| --- | ---: | ---: | ---: | ---: |
| VideoToolbox, native hardware surface | 3000 | 43.60 s | 68.8 fps | 0.688x |
| VideoToolbox, frame copied to system memory | 3000 | 54.75 s | 54.8 fps | 0.548x |
| CPU/software HEVC decode | 3000 | 36.28 s | 82.7 fps | 0.827x |
| VideoToolbox with FFmpeg RGBA conversion | 3000 | 47.18 s | 63.6 fps | 0.636x |

The native VideoToolbox path used about 5.8 aggregate CPU seconds over 43.6
wall-clock seconds. The software path used about 183.5 aggregate CPU seconds
over 36.3 wall-clock seconds. Although software decode produced more frames per
second in this isolated test, VideoToolbox preserves substantially more CPU
capacity for the UI, Zarr reads, overlays, and analysis.

Random seeks at 143, 358, 717, and 1075 seconds each succeeded. Seeking and then
decoding 120 frames took 2.37--2.53 seconds, with a mean of approximately 2.45
seconds. The stable results across the recording are encouraging, but this
measurement includes FFmpeg process and decoder startup and does not isolate
time to first presented frame. A persistent in-process decoder should avoid
some of that overhead.

QuickTime Player displayed the same source smoothly on the test Mac. That
observation is compatible with the FFmpeg throughput result: smooth playback
does not require presenting all 100 source frames on a display refresh cycle.
QuickTime can use a persistent AVFoundation/VideoToolbox pipeline, retain native
surfaces, synchronize presentation to the display, and omit late or
unnecessary presentations while keeping the media clock correct.

Crimson already follows the same broad presentation policy. During playback,
the render loop computes a source-frame target from the playback clock, selects
the exact buffered frame when available or the newest suitable buffered frame
at or before that target, presents one selected frame for that render-loop
iteration, commits the frame actually presented, and releases older buffer
slots. It therefore does not require every decoded 100 fps source frame to be
visibly presented. Paused frame stepping and frame-accurate seeking remain
separate requirements in which every source frame must stay addressable.

The decoder generally still advances sequentially through the compressed
stream to populate Crimson's buffer. Decode throughput therefore remains
important, but the `0.688x` native-surface FFmpeg result must not be interpreted
as proof that Crimson would play at `0.688x` media speed. The user-facing test
is whether a persistent VideoToolbox decoder keeps the selected presentation
frame sufficiently close to Crimson's logical 100 fps clock while the display
presents at its own refresh rate. Offline inference or export that must process
every frame is a separate throughput requirement.

### Benchmark Conclusions

- VideoToolbox is a viable decoder backend for the representative Crimson HEVC
  recording.
- Smooth clock-correct playback is plausible and is supported by QuickTime's
  behavior on the test host, but it has not yet been demonstrated inside
  Crimson's complete render and overlay workload.
- None of the tested paths decoded all source frames at 100 fps. This remains a
  performance risk for offline every-frame work and for maintaining a playback
  buffer, but not necessarily for smooth display-rate presentation.
- The preferred application path remains FFmpeg demux/seek to a persistent
  VideoToolbox session, retained `CVPixelBuffer`/IOSurface surfaces,
  `CVMetalTextureCache`, and Metal YUV-to-RGB conversion and composition.
- Copying every decoded frame into ordinary CPU memory should be avoided.
- The FFmpeg RGBA result is diagnostic only. The application should perform
  color conversion in Metal rather than materializing CPU RGBA frames.
- MLX remains an inference candidate, not a video presentation backend.

Before declaring accelerated-viewer feasibility complete, measure:

1. time to first frame with a persistent VideoToolbox decoder;
2. steady-state decode-to-present throughput through `CVMetalTextureCache`;
3. frame-accurate random seeking and multiview synchronization;
4. unified-memory use with a realistically sized native-surface ring;
5. thermally sustained performance on the fanless MacBook Air; and
6. behavior when full-rate playback cannot be maintained (pacing, buffering,
   or frame-dropping policy), checked against the parity contract.

### Persistent Decode-to-Metal Probe Result (2026-07-11)

A standalone Objective-C++ probe then exercised the intended application path:
FFmpeg demux and timestamps, VideoToolbox hardware decode, retained
`CVPixelBuffer`, `CVMetalTextureCache`, Metal NV12 conversion, and `MTKView`
presentation. The representative recording opened successfully and produced
its first Metal-presented frame in 157 ms.

In the initial every-frame decode mode, VideoToolbox stabilized near 69--77
decoded frames per second while Metal presented near the 60 Hz display rate.
The native-surface queue stayed at zero or one frame, but presentation lag grew
from 250 ms at 0.85 seconds to 3.74 seconds at 12.93 seconds: approximately 300
ms of additional lag per wall-clock second. This matches the gap between the
100 fps source clock and roughly 70 fps decode throughput.

This isolates the bottleneck more precisely:

- native VideoToolbox surfaces and `CVMetalTextureCache` mapping work;
- Metal presentation sustains the display refresh rate;
- neither texture conversion nor an oversized queue caused the accumulating
  lag; and
- dropping frames only after decode saves presentation work but cannot recover
  decoder throughput.

The next experiment must apply a decoder-side realtime policy that avoids
requesting output for frames that are unnecessary for display-rate playback.
The every-frame path must remain available for paused stepping, exact seeks,
inference, and export. Realtime success means bounded media-clock lag and smooth
presentation, not 100 distinct presentations per second.

The first decoder-side attempt set FFmpeg's `skip_frame` policy to
`AVDISCARD_NONREF` before opening the VideoToolbox decoder. It did not improve
the representative stream: output remained near 68--72 fps, the native-surface
queue remained empty or nearly empty, and lag reached 18.9 seconds after 60.4
seconds. The HEVC hardware path therefore did not omit enough work or output for
this policy to maintain the media clock.

QuickTime Player, by observation, kept its media position aligned with elapsed
wall time and appeared smooth on the same machine and recording. This argues
against treating the FFmpeg result as a demonstrated M3 hardware limit.
QuickTime uses Apple's higher-level playback stack, which can pipeline decode
asynchronously and choose frames before presentation work is performed.

VideoToolbox is only a codec API: it does not demux MP4, select tracks, own a
playback clock, or provide application-level seeking. A macOS backend therefore
needs either FFmpeg demux plus a directly managed `VTDecompressionSession`, or
AVFoundation for container/timing/seek with VideoToolbox-backed decoded output.
The next probe uses `AVPlayer`/`AVPlayerItemVideoOutput` to test the latter and
maps its `CVPixelBuffer` output directly into Metal. If it maintains bounded lag,
FFmpeg is not technically required for the macOS playback backend, although
retaining FFmpeg may still reduce cross-platform timing and container-semantic
differences.

### AVFoundation-to-Metal Probe Result (2026-07-11)

The AVFoundation probe successfully played the same representative recording
at a constant player rate of `1.00`. It delivered native NV12 `CVPixelBuffer`
surfaces through `AVPlayerItemVideoOutput`, mapped them with
`CVMetalTextureCache`, and presented them through the probe's Metal renderer.

Measured behavior over 60 seconds:

- first Metal-presented frame in 172 ms;
- approximately 60--61 new pixel buffers per second after startup, matching the
  display refresh rather than the 100 fps source rate;
- player-to-presented-frame lag bounded between approximately 11 and 22 ms;
- no accumulating lag;
- effectively no repeated display refreshes after startup; and
- 60.2 seconds of media time reached in approximately 60 seconds of playback.

This is a successful feasibility result. AVFoundation selected display-relevant
frames from the 100 fps source while keeping the presented PTS aligned with the
media clock. The same operation through FFmpeg's VideoToolbox hardware path was
approximately 19 seconds behind after 60 seconds. The earlier result is
therefore an FFmpeg-path limitation for this workload, not evidence that the M3
or Metal cannot provide clock-correct Crimson playback.

AVFoundation is now the preferred first implementation candidate for the macOS
video backend. FFmpeg is not technically required for normal MP4/MOV playback
if AVFoundation satisfies the remaining parity fixtures. It may still be useful
as a fallback for unsupported containers/codecs or if sharing exact demux
semantics across platforms proves more important than native playback behavior.

#### Exact Access and Synchronization Feasibility

AVFoundation provides the primitives needed for the following Crimson
requirements, but each remains parity-capable rather than parity-verified:

1. **Exact random seeking:** `AVPlayer` accepts zero-tolerance seeks and
   AVFoundation decodes forward from the appropriate synchronization sample.
   Crimson must verify that the first returned pixel buffer has the requested
   frame's expected PTS, including near GOP and clipped-recording boundaries.
2. **Paused access to arbitrary 100 fps frames:** Crimson can map a frame index
   to its rational media time, perform an exact seek, and request the pixel
   buffer for that item time. A separate accurate paused/stepping path can be
   used while the realtime playback path remains display-rate selective. Tests
   must cover consecutive forward/backward stepping and confirm no rounding or
   off-by-one errors.
3. **Multistream synchronization:** Crimson should retain one logical master
   timeline rather than allowing independent `AVPlayer` clocks to define
   synchronization. Each main-camera, crop, and stimulus output should resolve
   the master frame through the existing Zarr alignment and select the matching
   stream PTS. This is compatible with `AVPlayerItemVideoOutput`, but bounded
   skew, missing-frame behavior, seek settlement, and clipped handoffs must be
   measured across real sessions.

These capabilities do not require every 100 fps source frame to be presented
during playback. All source frames remain addressable for accurate paused
operations, while realtime presentation selects the frame appropriate to each
display refresh.

#### Accurate-Access Feasibility Result (2026-07-11)

The headless AVFoundation accurate-access probe passed on the representative
4512 x 4512, 100 fps recording.

Five decoded random-access requests at frames 14,344, 35,861, 71,723, 107,585,
and 129,102 returned the requested frame PTS. Four had zero measured frame
error; frame 107,585 differed by 0.002 frame due to timestamp representation,
well inside the 0.51-frame acceptance bound. Decode-to-first-frame latency was
49.8--243.9 ms, with the first/cold request the slowest.

Twenty consecutive frames beginning at frame 47,337 also passed. Every returned
PTS matched its expected 10 ms interval with zero measured frame error, and
every adjacent pair advanced by exactly one source frame.

This moves two requirements from architecturally plausible to demonstrated on
the representative recording:

- exact decoded random access is feasible through AVFoundation; and
- arbitrary 100 fps paused-frame access and consecutive stepping are feasible.

The multistream common-time check was skipped because no secondary crop or
stimulus file was supplied. Multistream capability and end-to-end Zarr-aligned
synchronization therefore remain unverified.

A subsequent three-input capability run supplied the 4512 x 4512 main stream,
the corresponding 256 x 256 external crop, and the main stream again under the
`stimulus` label. All accurate-access and 20-frame stepping checks passed for
all three inputs. Crop random-access latency was 3.9--17.5 ms; main-stream
latency was 52.2--215.9 ms in that run.

At a shared requested time near 573.79 seconds, the main and crop streams both
returned PTS 573.790000 with zero measured residual and a zero-millisecond
residual spread. This demonstrates common-time decoded access across the real
main/crop pair. It does not yet prove simultaneous realtime synchronization or
Zarr-aligned synchronization.

The `stimulus` result from this run is not evidence about the real stimulus
stream because its path was the main-camera file. The actual stimulus video,
including its distinct codec/frame rate and Zarr alignment, must still be
tested. The remaining synchronization work is therefore:

- run the capability check with the actual stimulus file;
- apply the session's real Zarr camera-to-stimulus mapping rather than common
  media time;
- drive all active outputs from one Crimson logical clock; and
- measure displayed PTS skew during playback, after exact seeks, and across
  clipped handoffs.

The real May 29 stimulus was subsequently loaded successfully as a 344 x 344,
approximately 120 fps H.264 track with a 1379.609-second duration. Sequential
QuickTime playback worked, but every `AVAssetReader` random-access request
failed immediately with `Cannot Decode`, as did the consecutive-frame test
when starting midstream. Main and crop continued to pass in the same run.

An analogous lab `GoodCopBadCop` stimulus file was inspected directly and did
have broken MP4 sync-sample metadata: decoded IDR frames occur about every 2.083
seconds, but the MP4 has no `stss` box, so all packets are treated as sync
samples. The repository's `scripts/check_h264_keyframes.py` independently
reported `Missing stss: 1` for that artifact.

The May 29 file is different. Its packet flags correctly mark the first sample
as sync and following P-frames as non-sync, ruling out the missing-`stss`
diagnosis for that file. Its failure instead exposed a flaw in the first Mac
feasibility test: the `AVAssetReader` time range began exactly at the requested
P-frame. AVAssetReader does not guarantee that this construction will decode
the preceding GOP for preroll. The accurate path must begin at or before the
preceding synchronization sample, decode forward, and discard frames until the
requested PTS, matching Crimson's existing two-phase seek design. The probe was
first updated to retry failed exact-start reads five seconds earlier, but that
also failed because an arbitrary earlier P-frame is still not a valid decoder
start. The probe now scans the compressed AVFoundation samples once, builds the
actual sync-sample PTS index, retries from the nearest preceding sync sample,
and discards decoded frames until the target. This indexed-preroll result,
rather than either arbitrary-start failure, determines H.264 accurate-access
feasibility.

The indexed-preroll probe subsequently passed after the May 29 stimulus was
re-encoded as a controlled H.264 derivative. It did not establish that the
original stimulus works. All per-stream random-access and consecutive-frame
checks passed with the derivative, followed by a three-stream common-time pass:

```text
requested common time: 551.833600 s
main:     requested 551.830000, returned 551.830000, latency 61.9 ms
crop:     requested 551.830000, returned 551.830000, latency  6.5 ms
stimulus: requested 551.833333, returned 551.833333, latency 31.4 ms
residual spread: 0.000 ms
```

This demonstrates that AVFoundation can accurately resolve the original HEVC
main/crop streams together with a controlled approximately 120 fps H.264
stimulus derivative when each request is aligned to its own rational frame grid
and decoding begins at the preceding sync sample. The original stimulus remains
an unresolved compatibility failure: both arbitrary-start and indexed-preroll
AVAssetReader decoding returned `Cannot Decode`, while QuickTime could play it
sequentially. The derivative pass indicates that the blocker lies in the
original stimulus encoding/container characteristics, not in the required
Crimson frame rate, dimensions, multistream timing model, or AVFoundation's
general H.264 capability.

The common-time result remains a backend capability test. It does not yet apply
the Zarr camera-to-stimulus frame mapping or measure simultaneous presented-frame
skew under realtime playback. Those are integration acceptance tests, not codec
feasibility blockers.

### Portable Zarr Alignment Fixture (2026-07-11)

The Mac probe now includes a portable alignment exporter and a mapped-access
suite. The exporter mirrors Crimson's run-selection and mapping priority: it
prefers `camera_to_stimulus_frame_corrected`, then falls back to the legacy
camera-frame to metadata-row to stimulus-frame path. It emits dense CSV plus a
JSON provenance manifest without requiring the Python Zarr package.

The representative June 14 analysis Zarr exported successfully from run
`stimulus_external_ipc_20260616_01` using the legacy metadata variant:

```text
camera mapping rows: 139024
valid stimulus mappings: 138000
first mapped stimulus frame: 0
last mapped stimulus frame: 165578
```

The Mac mapped-access suite accepts that CSV, samples five valid camera frames,
resolves each actual Zarr-mapped stimulus frame, and validates exact native
decode for main, crop, and stimulus on their respective rational frame grids.
This is the next acceptance gate before implementing simultaneous three-texture
presentation from one Crimson logical clock.

That mapped-access gate passed on the Apple M3 after using the controlled H.264
stimulus derivative. Five sampled mappings spanning camera frames 13,902 through
125,121 returned the exact main, crop, and mapped stimulus PTS values. Maximum
measured error was 0.002 stimulus frame; main access took 38.7--97.2 ms, crop
9.4--18.4 ms, and stimulus 9.0--20.7 ms.

The first exported CSV mislabeled the legacy `camera_interpolation_mask`
polarity: stored value 1 means original, not interpolated. The exporter was
corrected to invert that legacy mask when producing its `interpolated` column.
This label error did not affect any exported camera-to-stimulus frame number or
the mapped-access result.

## Architecture Work Needed Before macOS

The work that helps macOS also improves testability on Linux and Windows:

- Make CUDA an optional build feature instead of a required project language.
- Split decode behind an interface:
  - FFmpeg/NVDEC on NVIDIA platforms
  - VideoToolbox or software decode on macOS
- Split inference behind an interface:
  - TensorRT on NVIDIA platforms
  - placeholder/no-op initially on macOS
  - later CoreML/MPS/MLX/ONNX backend
- Split presentation/upload behind an interface:
  - OpenGL on current platforms
  - Metal on macOS
- Keep Zarr readers and overlay preparation independent of GPU backend.

## Policy

Ubuntu/Linux should stay on the current NVIDIA baseline stack unless the team
explicitly promotes a new stack.

macOS should not be described as "supported" until a no-CUDA viewer build can:

- configure on macOS
- build on macOS
- open a real Zarr/video session
- display key overlays
- seek reliably
- package into a runnable app bundle or staged folder

Until then, macOS is exploratory. The first macOS milestone should be a viewer,
not full inference/editing parity.
