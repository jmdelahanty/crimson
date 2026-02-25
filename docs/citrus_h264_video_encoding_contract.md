# Citrus — H.264 Rendered Video Encoding Contract

## Context

Citrus produces H.264 rendered recordings (screen captures, stimulus playback) that are stored alongside each experiment session under `raw/`. Crimson reads these for synchronized review and annotation. Two issues in the current output affect seekability:

1. **Missing stss box** on some files — the MP4 container has no sync sample table, so FFmpeg and crimson cannot determine which frames are keyframes. The MP4 spec treats all samples as sync when stss is absent, which defeats seek optimization.
2. **GOP of 250 frames** (~2.08s at 120fps) — even when the stss is present, crimson must decode up to 250 frames forward from the nearest IDR to reach any target frame. This makes interactive scrubbing sluggish.

**Target**: GOP of **30 frames** (0.25s at 120fps), with correct `AV_PKT_FLAG_KEY` on every IDR so the MP4 muxer writes a valid stss box.

## Current State

Analyzed from `/nvme1/recordings/2026-01-28T*/raw/*.mp4`:

| Property | Current Value |
|----------|---------------|
| Codec | H.264 (AVC) |
| Resolution | 344x344 |
| Frame rate | 120 fps |
| Pixel format | yuv420p |
| Bitrate | ~1.6 Mbps |
| GOP | 250 frames (2.08s) |
| stss box | Present on most, missing on some |
| Keyframe type | IDR (NAL type 5) |

51 of 52 files in the January 28 dataset need re-encoding due to the GOP issue.

## Required Changes

### 1. Set GOP to 30 frames

Configure the encoder to insert an IDR frame every 30 frames (0.25s at 120fps). The exact mechanism depends on the encoding API:

**NVENC API** (if using NVENC directly):
```
encodeConfig.gopLength = 30;
encodeConfig.idrPeriod = 30;     // IDR every GOP, not just I-frame
encodeConfig.frameIntervalP = 1; // IPPP pattern, no B-frames
```

**FFmpeg libavcodec** (if using FFmpeg API):
```
AVCodecContext *ctx;
ctx->gop_size = 30;              // -g 30
ctx->keyint_min = 30;            // -keyint_min 30
// Plus set forced-idr via private options:
av_opt_set(ctx->priv_data, "forced-idr", "1", 0);
```

**FFmpeg CLI** (reference):
```
ffmpeg -i input -c:v h264_nvenc
    -g 30 -keyint_min 30 -forced-idr 1
    -preset p1 -tune ll -rc vbr -b:v 1600000
    -movflags +faststart
    output.mp4
```

The `-forced-idr 1` flag is critical — without it, NVENC may insert CRA (Clean Random Access) frames instead of true IDR frames at GOP boundaries. CRA frames are not full random access points and will not be flagged correctly in the stss box.

### 2. Set `AV_PKT_FLAG_KEY` on IDR packets

Every encoded packet containing an H.264 IDR frame (NAL unit type 5) must have `AV_PKT_FLAG_KEY` set before being written to the MP4 muxer. This is what causes the muxer to write the stss (sync sample table) box with the correct keyframe entries.

```cpp
// H.264 IDR detection: NAL type is bits [0:4] of the first NAL byte
uint8_t nal_type = first_nal_byte & 0x1F;
if (nal_type == 5) {  // IDR slice
    pkt.flags |= AV_PKT_FLAG_KEY;
}
```

If using FFmpeg's encoding API (avcodec), this is typically handled automatically by the encoder. If using NVENC directly and feeding packets to FFmpeg's muxer, the flag must be set manually.

**Verification**: After writing, the MP4 must contain an `stss` atom. A quick binary check:
```python
with open("output.mp4", "rb") as f:
    assert b"stss" in f.read()
```

### 3. Use `-movflags +faststart`

Write the `moov` atom at the beginning of the file (not the end). This allows crimson and other players to begin reading metadata and seeking immediately without scanning to the end of the file first.

If using FFmpeg's muxer:
```cpp
AVDictionary *opts = NULL;
av_dict_set(&opts, "movflags", "+faststart", 0);
avformat_write_header(fmt_ctx, &opts);
```

## Output Specification

Each rendered recording must satisfy:

| Property | Requirement |
|----------|-------------|
| Container | MP4 |
| Video codec | H.264 (AVC) via h264_nvenc |
| GOP | 30 frames (0.25s at 120fps) |
| Keyframe type | IDR only (not CRA) at GOP boundaries |
| stss box | Present — muxer writes sync sample table |
| moov position | At file start (`+faststart`) |
| Audio | None (or muxed separately if added later) |
| Pixel format | yuv420p |
| Resolution | As rendered (currently 344x344) |
| Frame rate | As rendered (currently 120fps) |
| Rate control | VBR with current target bitrate |

## How Crimson Uses These Files

Crimson's demuxer (`FFmpegDemuxer.cpp`) relies on:

1. **stss box** — FFmpeg reads this to build an internal keyframe index. `av_seek_frame(..., AVSEEK_FLAG_BACKWARD)` uses it to land on the nearest preceding IDR.
2. **Regular GOP** — `FindKeyFrameInterval()` scans the first two keyframes to compute GOP spacing, then uses modulo arithmetic to predict keyframe positions for all subsequent seeks.
3. **IDR frames** — The decoder can start cold from any IDR without reference to prior frames. CRA frames cannot guarantee this.

With GOP 30 at 120fps, worst-case seek decode cost is 29 frames of 344x344 H.264 — effectively instant.

## Verification Checklist

After implementing the changes, verify each output file:

```bash
# 1. Confirm H.264 codec
ffprobe -v quiet -select_streams v -show_entries stream=codec_name -of csv=p=0 output.mp4
# Expected: h264

# 2. Confirm stss box exists
python3 -c "
with open('output.mp4', 'rb') as f:
    assert b'stss' in f.read(), 'stss missing'
print('stss: OK')
"

# 3. Confirm GOP is 30
ffprobe -v quiet -select_streams v \
    -read_intervals "%+5" \
    -show_packets -show_entries packet=flags \
    -of csv=p=0 output.mp4 | head -35
# Expected: K_ at line 1 and line 31

# 4. Confirm moov is at start (faststart)
python3 -c "
with open('output.mp4', 'rb') as f:
    head = f.read(4096)
    assert b'moov' in head, 'moov not at start of file'
print('faststart: OK')
"

# 5. Open in crimson, seek around, confirm fast seek response
```

## Retroactive Fix

Existing recordings can be batch re-encoded with the crimson script:
```bash
python scripts/reencode_h264_gop.py /nvme1/recordings --gop 30 --apply
```

This uses h264_nvenc with `-g 30 -keyint_min 30 -forced-idr 1`, matches the original bitrate, and verifies stss + frame count + resolution after encoding.
