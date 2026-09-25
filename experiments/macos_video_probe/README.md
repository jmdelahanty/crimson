# Crimson macOS VideoToolbox-to-Metal Probe

This is an isolated feasibility target, not a Crimson application build. It
contains two native playback experiments on Apple Silicon without requiring
CUDA, TensorRT, OpenGL, Zarr, or the main repository CMake project.

The first retains FFmpeg demux and decode:

```text
FFmpeg demux and timestamps
  -> FFmpeg VideoToolbox hardware decoder
  -> retained CVPixelBuffer
  -> CVMetalTextureCache Y and UV textures
  -> Metal limited-range NV12 conversion
  -> MTKView presentation
```

No decoded image is copied through CPU memory. The queue retains native decode
surfaces and is bounded by `--buffer-frames`.

The second uses Apple's complete native playback stack:

```text
AVFoundation demux, clock, seek, and VideoToolbox-backed decode
  -> AVPlayerItemVideoOutput CVPixelBuffer
  -> CVMetalTextureCache Y and UV textures
  -> Metal limited-range NV12 conversion
  -> MTKView presentation
```

The AVFoundation target has no FFmpeg dependency.

## Prerequisites

- Apple Silicon Mac
- Xcode Command Line Tools
- Homebrew
- CMake and Ninja
- Homebrew FFmpeg with VideoToolbox support

Install the command-line dependencies if necessary:

```bash
brew install cmake ninja pkg-config ffmpeg
```

Confirm FFmpeg exposes VideoToolbox:

```bash
ffmpeg -hide_banner -hwaccels
```

FFmpeg is only required for the FFmpeg comparison probe.

## Build and run

From this directory:

```bash
chmod +x scripts/build_and_run.sh
scripts/build_and_run.sh "/path/to/video.mp4"
```

For a ten-minute unattended sample:

```bash
scripts/build_and_run.sh "/path/to/video.mp4" --duration 600
```

The default `complete` mode requests every decoded frame. To test decoder-side
realtime output omission:

```bash
scripts/build_and_run.sh "/path/to/video.mp4" --mode realtime --duration 60
```

`realtime` sets FFmpeg's `skip_frame` policy to `AVDISCARD_NONREF` before the
VideoToolbox decoder is opened. Non-reference pictures are not requested as
output, while reference pictures required by later pictures are still decoded.
This differs from dropping already-decoded frames in the renderer and can
reduce output and surface pressure. Whether it improves hardware decode
throughput depends on the stream's HEVC prediction structure and FFmpeg's
VideoToolbox integration.

To run the AVFoundation comparison for 60 seconds:

```bash
scripts/build_and_run_avfoundation.sh "/path/to/video.mp4" --duration 60
```

This is the closest probe to QuickTime behavior. AVFoundation owns the media
clock and decides which decoded frame is appropriate for each display time;
the probe receives native pixel buffers and presents them through its own Metal
pipeline. Its CSV files are named `results/avfoundation-*.csv`.

Controls:

- Space: pause or resume
- Left arrow: seek backward 10 seconds
- Right arrow: seek forward 10 seconds
- Q: quit

The terminal prints one line per second containing:

- target media-clock PTS;
- PTS actually presented;
- presentation lag;
- newly decoded and presented frames per second;
- decoded frames omitted from presentation;
- display refreshes that repeated the previous frame; and
- current native-surface queue occupancy.

The same samples are written to `results/metrics-*.csv`. Seek operations also
print decoder-flush-to-first-frame latency.

## How to interpret it

Success is clock-correct, visually smooth playback with bounded lag and memory,
not presentation of all 100 source frames each second. A 60 Hz display cannot
show 100 distinct refreshes. Paused/seeked frames must nevertheless remain
individually addressable.

The first probe intentionally uses a fixed limited-range BT.709 conversion for
the representative recording, whose color matrix was unspecified. Color parity
with Crimson and QuickTime is a separate acceptance test.

This code is exploratory. It does not yet implement Crimson overlays,
multicamera synchronization, clipped-recording handoffs, or inference.
The realtime mode also does not yet switch back to complete decoding for exact
paused stepping. It is a throughput experiment, not the final parity design.

## Automated feasibility checks

The headless feasibility runner tests AVFoundation's accurate-access path and
writes all build output, measurements, and the final status to a timestamped
`results/feasibility-*/log.txt`:

```bash
scripts/run_feasibility_tests.sh "/path/to/main.mp4"
```

It performs:

- decoded random access at 10%, 25%, 50%, 75%, and 90% of the recording;
- validation that the returned PTS is within 0.51 source frames of the
  requested rational frame time;
- 20 consecutive decoded frames near one-third of the recording;
- validation of monotonic one-frame PTS increments; and
- decoded dimensions and per-request latency reporting.

Optional additional videos exercise the same checks and a common-time
multistream access test:

```bash
scripts/run_feasibility_tests.sh "/path/to/main.mp4" \
  --secondary crop="/path/to/crop.mp4" \
  --secondary stimulus="/path/to/stimulus.mp4"
```

The multistream test proves that AVFoundation can independently resolve native
frames near a common requested time. It does not apply Crimson's Zarr alignment
tables and therefore does not establish end-to-end camera/crop/stimulus parity.
That must be tested after the backend is integrated with Crimson's logical
master timeline.

## Exporting and testing real Zarr alignment

On a host that can access the analysis Zarr and has NumPy plus the `zstd`
command, export Crimson's selected stimulus mapping:

```bash
tools/export_alignment_fixture.py \
  /path/to/analysis.zarr \
  /path/to/alignment.csv
```

The exporter mirrors Crimson's selection rules. It prefers the dense
`camera_to_stimulus_frame_corrected` array and otherwise resolves the legacy
camera-to-metadata-to-stimulus mapping. It writes:

- `alignment.csv`: dense camera frame, stimulus frame, interpolation, and
  validity columns; and
- `alignment.json`: source/run/schema provenance.

Copy both files to the Mac with the corresponding videos, then run:

```bash
scripts/run_feasibility_tests.sh "/path/to/main.mp4" \
  --secondary crop="/path/to/crop.mp4" \
  --secondary stimulus="/path/to/stimulus.mp4" \
  --alignment "/path/to/alignment.csv"
```

The `zarr_mapped_alignment` suite samples five valid camera frames, resolves
their actual mapped stimulus frames, decodes all three native frames, and
checks each returned PTS against that stream's rational frame grid. This is an
end-to-end mapping/access check; simultaneous realtime presentation skew is a
separate GUI integration milestone.

## Simultaneous aligned Metal playback

Build and run the three-texture playback probe with matching videos and the
exported alignment:

```bash
scripts/build_and_run_aligned.sh \
  --main "/path/to/main.mp4" \
  --crop "/path/to/crop.mp4" \
  --stimulus "/path/to/stimulus_reencoded.mp4" \
  --alignment "/path/to/alignment.csv" \
  --duration 600
```

The main player is the logical camera clock. Each display refresh resolves the
camera frame, looks up the mapped stimulus frame, requests all three native
pixel buffers, and renders them in one Metal window. Main occupies the left;
crop and stimulus occupy the upper-right and lower-right.

Controls:

- Space: pause/resume all streams
- Left/Right: seek the logical camera clock by 10 seconds
- Comma/period: pause and step one camera frame backward/forward
- Q: quit

The timestamped CSV reports logical camera and stimulus frames, presented PTS,
per-stream frame error, and secondary-player clock error. All three players are
started at their mapped item times against one shared host-clock start. The
probe measures natural secondary-clock drift without periodic hard seeks.
