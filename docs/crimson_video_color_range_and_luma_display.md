# Crimson Video Color Range And Luma Display

Date anchored: 2026-07-07.

## Why This Exists

Orange acquisition videos can be mono/luma recordings where scientific review
cares about the source intensity values. If Crimson displays those frames
through a YUV-to-RGBA conversion that assumes the wrong color range, the image
can look visibly different even when the encoded video data did not otherwise
change.

This note separates three related surfaces:

- encoded video essence: codec, pixel format, bit depth, chroma sampling, and
  compressed frame payloads
- metadata tags: color matrix, color range, transfer, primaries, and container
  stream metadata
- Crimson display conversion: how decoded NV12/luma data becomes RGBA for
  OpenGL display

## Crimson Path

The main hardware decode path is:

```text
FFmpegDemuxer
  -> NvDecoder / CUVID / NVDEC
  -> decoded NV12/P016 style surfaces
  -> CUDA or shader conversion to RGBA
  -> OpenGL texture/PBO display
```

Relevant code:

- `src/FFmpegDemuxer.cpp` reads `codecpar->color_space` and
  `codecpar->color_range`.
- `src/decoder.cpp` reads NVDEC `matrix_coefficients`, reads FFmpeg stream
  `color_range`, and publishes those as frame-slot metadata.
- `src/frame_slot.h` stores `color_matrix` and `color_range`.
- `src/ColorSpace.cu` converts NV12 to RGBA in the CUDA path.
- `src/gui/camera_view_presenter.cpp` converts NV12 to RGBA in the lighter
  OpenGL shader path.

## Orange-Side Clarification

Orange clarified on 2026-07-07 that current mono acquisition outputs should be
treated as full-range luma inside an NV12/yuv420p-style video stream:

- New mono acquisition videos write source luma `0..255` into the NV12 Y plane.
- The stream/container is tagged full-range/pc.
- Orange does not currently encode true grayscale MP4 for this path. It uses
  NVENC NV12 input, and MP4 readers may advertise the stream as `pix_fmt=yuv420p`.
- Mono chroma is neutral `0x80`, so display should be direct luma or
  full-range YUV, not limited-range expansion.

Orange-side code points reported with that clarification:

- MP4 stream metadata sets `AV_PIX_FMT_YUV420P` and `AVCOL_RANGE_JPEG`.
- NVENC VUI metadata sets the full-range flag.
- Full-frame mono writes the Y plane and fills UV with neutral chroma.
- Crop mono copies the source ROI into crop luma.
- Crop encode writes Y only with neutral chroma.

This means Crimson's range-aware NV12 display should be correct for new Orange
mono outputs when metadata is present and propagated. Direct luma display is
still the cleaner scientific review path because it avoids chroma and YUV color
conversion entirely.

## Historical Limitation

Crimson previously preserved and used color matrix information, but not color
range information.

The CUDA converter in `src/ColorSpace.cu` assumed limited/video range for
8-bit YUV:

```text
black = 16
white = 235
```

The NV12 shader path in `src/gui/camera_view_presenter.cpp` made the same
assumption:

```text
y = texture_luma * 255 - 16
scale = 1 / 219
```

That meant a full-range luma value was displayed as if it were limited-range
video:

```text
display = clamp((Y - 16) * 255 / 219)
```

That meant:

- source values `0..15` are crushed to black
- source values `236..255` are clipped to white
- midtones are contrast-expanded by about `255 / 219`, or `1.164x`

For natural color video this may look like a normal video-range expansion. For
mono scientific acquisition video, it can distort intensity interpretation.

## Does This Mean The Encoding Changed?

Not necessarily.

Changing `color_range` metadata from limited/video to full/pc can change how a
player should interpret decoded samples without changing the compressed frame
payloads. In that case the codec bitstream can be byte-for-byte equivalent or
visually equivalent under the correct interpretation, while a reader that
ignores the tag still displays it incorrectly.

But encoding can differ in other ways that are independent of the color-range
tag:

- codec: H.264 vs HEVC
- pixel format: monochrome/luma-only vs `yuv420p`/NV12-style output
- bit depth: 8-bit vs 10-bit or 16-bit surfaces
- chroma sampling: monochrome, 4:2:0, 4:2:2, 4:4:4
- quantization and encoder settings
- container metadata and stream side data

So the right question for an Orange/Crimson compatibility check is not just
"is the encoding different?" It is:

1. Did the stored frame sample values change?
2. Did the codec/pixel format/chroma/bit-depth surface change?
3. Did only the metadata interpretation change?
4. Does Crimson apply that metadata when converting to display RGBA?

For current Orange mono outputs, the expected encoding shape is not true
monochrome. It is NV12/yuv420p-style video with neutral chroma and full-range
metadata.

## Preferred Direction For Orange Mono Videos

For mono Orange acquisition videos, the best display behavior is direct luma
grayscale:

```text
R = Y
G = Y
B = Y
A = 255
```

This avoids YUV range and chroma interpretation entirely for the scientific
review path. It also matches the mental model of "show me the camera intensity"
better than passing a luma-only source through a generic color-video converter.

For generic NV12/yuv420p video, Crimson's NV12-to-RGBA display conversion is now
range-aware:

- full/pc/JPEG range: use `black = 0`, `white = 255` for 8-bit
- limited/tv/MPEG range: use `black = 16`, `white = 235` for 8-bit
- unspecified range: keep a conservative default and log enough metadata to
  debug display differences

## Implemented Range Propagation

Implemented in Crimson:

1. Add `color_range` to `FrameSlotMetadata` and `PictureBuffer` metadata.
2. Populate it from FFmpeg demuxer metadata.
3. Pass `color_range` into both display conversion paths:
   - CUDA `Nv12ToColor32`
   - OpenGL NV12 shader path
4. Add an explicit full-range mode with 8-bit `black = 0`, `white = 255`.

Remaining useful diagnostic slice:

1. Log for a few decoded frames:
   - codec
   - pixel format/chroma format
   - bit depth
   - color matrix
   - color range
   - chosen display conversion mode
2. If NVDEC and FFmpeg expose different color-range metadata on a recording,
   log both values and choose a deterministic priority.

Recommended follow-up slice:

1. Detect Orange mono/luma videos from stream metadata or an explicit
   acquisition-side contract.
2. Add a direct luma-to-RGBA presentation path.
3. Keep the generic YUV-to-RGB path for color video.
4. Add a small screenshot or pixel-stat smoke that verifies full-range luma
   does not clip values below 16 or above 235.

## Remaining Questions

- Add a parity test for cropped lossless videos. Orange intends luma parity,
  but this still needs validation on representative outputs.
- Full-frame lossy videos should not promise exact source-pixel parity.
- Full-frame resized or downsampled videos cannot promise source-pixel parity
  by definition.
- Are there old recordings where the frame payloads are already limited-range
  scaled, despite being scientific mono data?
- Should the acquisition contract state that Crimson should display the luma
  plane directly for mono camera videos?

Those answers decide whether Crimson should auto-select luma grayscale by
metadata, by recording provenance, or through an explicit user toggle.
