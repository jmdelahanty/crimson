# Crimson Phase 4F-D Apple Acquisition Crop Playback

Date: 2026-07-12

Phase 4F-D connects the validated acquisition crop repository to the existing
bounded AVFoundation playback buffer. It produces exact, retained Apple crop
surfaces aligned to Crimson camera-frame identity. This checkpoint does not
add Crop Preview or Metal presentation.

## Session Boundary

`AppleAcquisitionCropPlaybackSession` owns:

- one `AcquisitionCropRepository`;
- one `AppleVideoPlaybackBuffer` with an explicit capacity;
- the full-camera dimensions supplied by the active camera backend; and
- request and decoder metrics.

The session opens the repository's resolved crop-video path with stream ID
`crop`. It has no autonomous clock and never calls the playback buffer's timed
playback API. Every hold, bounded forward follow, or seek is caused by a camera
frame requested by the caller.

For a successful request, `AppleAlignedAcquisitionCropFrame` contains:

- the repository's camera/video/metadata resolution and provenance row;
- the portable `AcquisitionCropFrameState`;
- the existing `PreferAcquisitionVideo` shared crop selection; and
- an exact `AppleDecodedVideoFrame` with a reference-counted VideoToolbox
  surface.

The session returns no frame unless decoded stream, video-frame, and local-frame
identities all equal the repository mapping and the shared crop selection is
`Selected`. A mapped blank row remains selected and returns its exact encoded
black surface with unusable live geometry. Out-of-range requests clear the
target without asking the decoder for another frame.

## Open-Time Validation

Before retaining the repository, `open()` rejects:

- null repositories, nonpositive full-camera dimensions, or capacities below
  two;
- descriptor frame counts that disagree with repository row counts;
- any crop geometry invalid for the supplied full-camera dimensions; and
- a missing resolved video path; and
- MP4 width, height, frame count, or nominal rate that disagrees with the
  repository contract.

The inventory deliberately does not invent full-camera dimensions. The future
application integration must pass dimensions from the exact full-frame camera
backend that participates in source selection.

## Request Policy

The decoder action is observable as `Clear`, `Hold`, `Follow`, or `Seek`:

- an already buffered exact frame is held;
- a nearby forward request follows bounded read-ahead;
- backward, explicitly discontinuous, initial unbuffered, and capacity-sized
  jumps seek; and
- out-of-range requests clear without decoding.

This policy affects how the exact frame is obtained, not which identity may be
presented. Returned `FrameSurface` ownership remains reference-counted, so a
caller can retain a selected surface across buffer eviction, suspension, seek,
or session close.

## Automated Coverage

`apple_acquisition_crop_playback_tests` uses a deterministic 12-frame Apple
video fixture and covers:

- null, dimensions, capacity, geometry, descriptor, MP4 dimensions, MP4 count,
  and rate validation;
- forward, repeated, backward, and discontinuous requests;
- exact decoded stream/video/local identity and shared source selection;
- a mapped blank frame with no usable live geometry;
- negative and end-bound out-of-range requests;
- suspend and exact resume;
- retained surface lifetime across seeks; and
- current and peak buffering bounded by four frames.

The shared Apple fixture runner retries once only when AVAssetWriter reports
the transient macOS media-service error `Cannot Encode`. Other failures are not
retried, and a repeated media-service failure still fails the test.

macOS validation:

```text
cmake --build --preset build-macos-arm64-release -j 8
ctest --preset test-macos-arm64-headless --output-on-failure
13/13 passed
```

The shared CMake changes were also applied to the isolated maintained NVIDIA
worktree. Configuration succeeded with CUDA 12.4, OpenCV 4.10, TensorRT
10.0.1.6, the NVIDIA FFmpeg stack, and TensorStore. The full `redgui` target
remained linked with no work required, and all five portable CTests passed.
The Apple session sources remain outside NVIDIA source discovery.

## Production Decode Probes

`apple_acquisition_crop_playback_probe` opened each production repository and
crop MP4 directly from `/Volumes/johnsonlab`. Each ordered probe requested the
first, middle, last, middle again for a backward seek, and the first metadata
blank frame. VideoToolbox access requires running this probe outside the tool
sandbox; a sandboxed decoder correctly failed before producing evidence.

| Recording | Video | Exact requests | Sampled blank | Blank mean luma | Peak buffer |
| --- | ---: | ---: | ---: | ---: | ---: |
| GoodCopBadCop | 256x256, 100 fps, 140035 frames | 5/5 | 4774 | 0.0 | 1 |
| RedScare | 384x384, 100 fps, 139908 frames | 5/5 | 108520 | 0.0 | 1 |

Both probes performed four seeks, including the last-to-middle backward seek.
All decoded identities matched the repository's zero-based video identity and
one-based recording identity. The sampled blank rows returned retained NV12
surfaces whose decoded luma planes were black.

## Next Checkpoint

Phase 4F-E should integrate this session with the macOS application and Metal
Crop Preview. Candidate presentation must consume the shared source selection
and commit only when the crop surface and main camera surface carry the same
camera identity. It should then exercise acquisition-video and geometry-only
source preference without changing the repository or decoder mapping rules.
