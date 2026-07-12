# Crimson Phase 3 Apple Main-Camera Viewer

Date: 2026-07-12

Phase 3 adds production main-camera playback to the native macOS application.
It does not load Zarr analysis data, crops, stimulus media, overlays, editing, or
inference; those remain later phases.

## Architecture

Crimson owns the logical frame clock through `LogicalPlaybackClock`.
AVFoundation supplies decoded frames but does not own transport state or
requested frame identity. Playing and exact access use one bounded
`AVAssetReader` worker:

```text
Playing
  Crimson logical clock
    -> AVAssetReader sequential decode worker
    -> discard decoded frames that have missed their deadline
    -> restart at the current target when backlog exceeds one source second
    -> fixed-capacity native-surface queue
    -> native NV12 CVPixelBuffer / IOSurface
    -> CVMetalTextureCache Y and UV textures
    -> metadata-driven Metal color conversion

Pause / seek / step
  Crimson exact frame request
    -> AVAssetReader positioned at rational media time
    -> bounded preroll and exact PTS settlement
    -> retained native NV12 CVPixelBuffer
    -> the same Metal renderer
```

The worker decodes ahead only until its six-surface queue is full. During
playback it drops frames that can no longer be presented. If it falls at least
one source second behind, it performs one bounded exact reposition at Crimson's
current target rather than decoding an obsolete backlog. The recovery count is
reported so repeated seek thrashing is visible. Pause, seek, step, range
settlement, and pass/fail decisions remain exact and clock-owned by Crimson.

An `AVPlayerItemVideoOutput` experiment was rejected after it accumulated a
57.25-second presented-frame deficit in a two-minute SMB/VPN run while its
independent media clock continued to report no error. The bounded reader
recovered on the same source and connection and does not introduce a second
application clock.

## Native Ownership

Every Apple surface retains its `CVPixelBuffer` and publishes
`FrameSurfaceOwnership::ReferenceCounted` with a reference-counted lifetime. A
Metal command-buffer completion handler holds the surface until GPU sampling is
finished. AVFoundation may recycle its IOSurface only after the final Crimson or
Metal reference releases it.

The queue has a fixed capacity and never uses the legacy `PictureBuffer`
malloc/CUDA ring. It holds at most six decoded surfaces plus the surface being
presented and AVFoundation's internal decode pool.

## Frame Identity

The checked-in fixture
`tests/fixtures/macos_main_camera_frame_identity.csv` records eight exact
frame/PTS pairs across the representative recording, including its final
region. The headless provider test verifies:

- 4512 x 4512 dimensions;
- 140,035 frames at 100/1 fps;
- 1/90000 media timebase;
- exact frame and PTS identity at every fixture row;
- consecutive one-frame stepping;
- exact-session suspend and restart;
- surface lifetime after provider teardown; and
- bounded queue occupancy under a clock-paced consumer.

Seek targets are constructed with CoreMedia rational multiplication rather than
floating-point frame-time reconstruction.

## Color

AVFoundation provides native two-plane NV12 output. Pixel-buffer attachments and
pixel format declare matrix and limited/full range. When the source matrix is
unspecified, the main-camera fallback is BT.709, matching the earlier Crimson
probe. The representative stream declares limited (`tv`) range and leaves
matrix, transfer, and primaries unspecified.

The headless Metal pixel suite validates concrete BGRA output for:

- limited-range black and white;
- full-range mid-gray;
- BT.601 colored output;
- BT.709 colored output; and
- BT.2020 colored output.

The viewer was also inspected in its native Cocoa window and accepted visually
during Phase 3 development.

## Metrics

The viewer and structured smoke record expose:

- requested and presented frame;
- final PTS error in source frames;
- intentional source frames skipped between display refreshes;
- repeated presentations;
- late presentations and maximum observed lag;
- decoded and catch-up-discarded frame counts;
- automatic catch-up seek count;
- startup and exact-seek latency;
- queue capacity and peak occupancy;
- `nextDrawable` and command-buffer wait latency;
- current and peak process physical footprint; and
- macOS thermal state.

`skipped_source_frames` is expected when displaying a 100 fps source on a lower
refresh-rate display. It is not a decode failure. A late presentation means the
surface shown on a refresh trails Crimson's requested frame. Acceptance requires
that lag be bounded and recoverable, not that a network-backed source never
experiences an isolated stutter. The structured smoke rejects a run whose
single worst presentation lag exceeds five source seconds, even if its final
exact seek settles correctly.

## Validation Environment

The representative video is read directly from:

```text
//delahantyj@prfs.hhmi.org/johnsonlab
  -> /Volumes/johnsonlab
  -> smbfs
```

The developer reported that this SMB path was accessed from home over Wi-Fi and
VPN. No local video copy was used for the primary playback gates. This is the
intended operational environment and also means storage/network behavior is part
of the measured end-to-end result.

The HEVC source averages 150.0 Mbit/s. A cold 512 MiB mid-file sequential read
over the mounted path sustained 35.8 MB/s (about 287 Mbit/s). This proves useful
average headroom but does not eliminate VPN/SMB latency or short throughput
variability. Ethernet can improve the local leg without changing the VPN,
internet route, or PRFS server.

Repeatable commands:

```bash
cmake --build --preset build-macos-arm64-release
ctest --preset test-macos-arm64

build/macos-arm64-release/apple_video_provider_tests \
  --asset /Volumes/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop/cams/Cam2010093_2026-06-14T21-12-08Z_arena_1.mp4

scripts/macos_gui_smoke_playback.sh \
  /Volumes/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop/cams/Cam2010093_2026-06-14T21-12-08Z_arena_1.mp4 \
  70000:70300
```

## Gate Results

Phase 3 passed on 2026-07-12.

The representative identity test passed all eight checked-in frame/PTS pairs
from frame 0 through frame 140,033. Every result used the 1/90000 media
timebase, and exact seeks completed in 110--229 ms during the final run.
Consecutive paused stepping also passed. The native Cocoa viewer was inspected
with the representative recording and its color/range presentation was
accepted visually. The offscreen Metal suite provides repeatable pixel-level
coverage for the declared limited/full and matrix conversions.

The final duration smoke read the 4512 x 4512, 100 fps HEVC directly from the
SMB mount over home Wi-Fi and VPN. No local copy was used. Results for frames
0--60,000 were:

```text
elapsed_s=600.262
requested=60000 presented=60000 pts_error_frames=+0.000
decoded=59901 peak_buffer=6
repeats=974 skipped_source_frames=24989
late_presentations=987 max_lag_frames=318.0
catchup_discarded_frames=1515 catchup_seeks=1
startup_ms=451.5 seek_ms=111.0
memory_mib=136.7 peak_memory_mib=151.0 thermal=nominal
```

The one 3.18-second network/decode disturbance was recovered by one catch-up
seek. It did not accumulate into timeline drift, queue growth, memory growth, or
an incorrect end frame. This is accepted for the intended high-bitrate
SMB/VPN/Wi-Fi workflow. Ethernet is expected to reduce risk on the local leg,
but is not required for correctness. The smoke exposes the transient rather
than classifying it as an ordinary display-rate source-frame skip.

The native release CTest suite passed 5/5. ASan+UBSan passed the portable frame
contracts, Apple provider/buffer, offscreen Metal, and headless Metal tests.
TSan passed the portable lease/clock and threaded Apple provider/buffer tests.

The exact staged Phase 1--3 tree was also applied to a disposable worktree on
the maintained RTX A6000 host. The CUDA 12.4, architectures 80/86, TensorRT
10.0.1.6, OpenCV 4.10, NVIDIA FFmpeg/NVDEC/CUVID, OpenGL, GLEW, and TensorStore
configuration succeeded. The complete 5,203-step build linked `redgui`; NVIDIA
CTest passed; and the authenticated playback smoke reported:

```text
[PlaybackSmoke] PASS start_frame=0 end_frame=300 presented_frame=300
presented_slot=0 view_idx=0 presented_count=350 elapsed_s=2.99059
```
