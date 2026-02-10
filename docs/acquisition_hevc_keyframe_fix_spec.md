# Acquisition Agent — HEVC Keyframe Flag Fix & Seekability Improvements

## Context

Crimson's demuxer (`src/FFmpegDemuxer.cpp:313`) relies on `AV_PKT_FLAG_KEY` in the MP4 container to find keyframes. This drives:
- `FindKeyFrameInterval()` — scans for the first two keyframes to compute GOP spacing
- `FindClosestKeyFrame()` / `FindClosestKeyFrameFNI()` — modulo arithmetic to predict keyframe positions
- `av_seek_frame(..., AVSEEK_FLAG_BACKWARD)` — FFmpeg's internal seek, which uses the container's keyframe index

**The problem**: The acquisition agent's `FFmpegWriter` only sets `AV_PKT_FLAG_KEY` for H.264 by checking NAL type 5 (IDR). For HEVC, the IDR frames (NAL types 19/20) exist in the bitstream per the encoder's `gopLength`/`idrPeriod` settings, but the muxer never flags them in the container. This means:

1. `FindKeyFrameInterval()` returns incorrect results for HEVC recordings (may scan the entire file without finding 2 keyframes)
2. FFmpeg's seek index has no keyframe entries, so `AVSEEK_FLAG_BACKWARD` may land at frame 0 instead of the nearest IDR
3. The two-phase accurate seek in `decoder.cpp:169-185` works around this but at the cost of decoding from the beginning of the file

**Impact on bidirectional buffer**: The proposed 32-frame history window requires seeking back and decoding forward from the nearest IDR. Without reliable keyframe flags, seek cost is unbounded (potentially decoding from frame 0 every time).

## Changes for the Acquisition Agent

### 1. Fix HEVC keyframe flag detection in FFmpegWriter

In the packet callback that feeds encoded NALUs to the muxer, add HEVC IDR detection alongside the existing H.264 check.

**Current** (H.264 only):
```cpp
// Pseudocode of existing logic
if (codec == H264 && nal_unit_type == 5) {
    pkt.flags |= AV_PKT_FLAG_KEY;
}
```

**Required** (add HEVC):
```cpp
if (codec == H264) {
    // H.264 IDR: NAL type 5
    uint8_t nal_type = first_nal_byte & 0x1F;
    if (nal_type == 5) {
        pkt.flags |= AV_PKT_FLAG_KEY;
    }
} else if (codec == HEVC) {
    // HEVC IDR: NAL types 19 (IDR_W_RADL) and 20 (IDR_N_LP)
    // HEVC NAL header is 2 bytes; type is bits [1:6] of the first byte
    uint8_t nal_type = (first_nal_byte >> 1) & 0x3F;
    if (nal_type == 19 || nal_type == 20) {
        pkt.flags |= AV_PKT_FLAG_KEY;
    }
}
```

**HEVC NAL type reference**:
| NAL Type | Name | Keyframe? |
|----------|------|-----------|
| 19 | IDR_W_RADL | Yes — IDR with leading pictures allowed |
| 20 | IDR_N_LP | Yes — IDR with no leading pictures |
| 21 | CRA_NUT | Partial — clean random access, but not a full IDR |

Flag types 19 and 20. Type 21 (CRA) can optionally be flagged but is not a hard requirement since NVENC with `idrPeriod = gopLength` produces true IDR frames, not CRA.

### 2. Validate with existing encoder settings

The fix should be verified against all three encoder paths:

| Path | GOP | IDR | B-frames | Expected keyframe interval |
|------|-----|-----|----------|---------------------------|
| HW encoder (`encoder_hw_worker.cpp`) | `fps * 2` (120 @ 60fps) | `= gopLength` | None (IPPP) | Every 120 frames (~2s) |
| Headless GPU encoder (`gpu_video_encoder.cpp`, ll/ull) | 15 | Not explicitly set | None | Every 15 frames |
| Headless GPU encoder (default) | NVENC default | NVENC default | NVENC default | Varies |
| Crop encoder (`crop_and_encode_worker.cpp`) | NVENC default | NVENC default | NVENC default | Varies |

After the fix, a recording at 60fps with the HW encoder should produce keyframe flags every 120 frames in the MP4 container.

### 3. Recommended: Write keyframe index as sidecar metadata

For maximum reliability (especially for the NVENC-default paths where GOP is not explicitly controlled), the acquisition agent should write a sidecar keyframe index alongside each recording:

```
recording.mp4
recording.keyframes.json   # or .bin
```

Format:
```json
{
    "codec": "hevc",
    "gop_length": 120,
    "idr_period": 120,
    "framerate": 60.0,
    "keyframe_frames": [0, 120, 240, 360, ...],
    "total_frames": 108000
}
```

This is recommended for all encoder paths. While fixing the container flags (change 1) is sufficient for FFmpeg-based seeking with regular GOPs, a sidecar index gives crimson exact keyframe positions without scanning — particularly valuable for the NVENC-default encoder paths where GOP spacing isn't guaranteed to be regular.

### 4. Optional: Reduce GOP for better seek granularity

The current 2-second GOP (120 frames at 60fps) means worst-case decode-forward of ~120 frames to reach any target. For the bidirectional buffer's 32-frame history, this is acceptable but not ideal.

Possible alternatives (tradeoffs):

| GOP | Seek granularity | Bitrate impact | Recommendation |
|-----|-------------------|----------------|----------------|
| 120 (current) | 2s | Baseline | Keep for bandwidth-sensitive paths |
| 60 (1s) | 1s | ~5-10% increase | Good balance for review/analysis recordings |
| 30 (0.5s) | 0.5s | ~10-20% increase | Best for interactive scrubbing |

This is a policy decision, not a bug fix. The current GOP is fine for the bidirectional buffer — the main bottleneck is the missing keyframe flags, not the GOP size.

## Priority

1. **Must do**: Change 1 (HEVC keyframe flag fix) — this is the root cause of unreliable seeking
2. **Recommended**: Change 3 (sidecar keyframe index) — gives crimson exact keyframe positions for all encoder paths, especially those using NVENC defaults
3. **Policy decision**: Change 4 (GOP reduction) — only if interactive scrubbing latency is a priority

## How Crimson Benefits

Once the acquisition agent writes proper HEVC keyframe flags:

- `FindKeyFrameInterval()` returns the correct GOP spacing (e.g. 120 at 60fps)
- `av_seek_frame()` lands on the correct IDR, not frame 0
- The two-phase accurate seek (`decoder.cpp:169-185`) works optimally: seeks to IDR N-1, decodes forward only ~120 frames max
- The bidirectional buffer's seek warmup (Phase 4 in the buffer TODO) has bounded, predictable cost

**No changes needed in crimson itself** — the demuxer already checks `AV_PKT_FLAG_KEY` correctly. The fix is entirely on the write side.

## Verification

1. Record a short HEVC clip with the patched FFmpegWriter
2. Verify with `ffprobe -select_streams v -show_frames -show_entries frame=pict_type,key_frame recording.mp4 | grep -A1 "key_frame=1"` — should show keyframes every GOP frames
3. Open in crimson; check console for `identified seek interval: 119` (or your GOP-1)
4. Seek to various frames; confirm no decode-from-frame-0 fallback in logs
5. Compare seek latency before/after with a large recording (>10 min)
