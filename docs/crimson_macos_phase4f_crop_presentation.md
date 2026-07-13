# Crimson Phase 4F-E Atomic Crop Presentation

Date: 2026-07-13

Phase 4F-E connects the acquisition crop repository and Apple playback session
to the native macOS application. Crop Preview can now present either the exact
dedicated acquisition crop video or a live Metal crop sampled from the exact
full-camera surface.

## Atomic Presentation Contract

The camera frame selected by the main playback buffer is the candidate
identity. A dedicated crop is not committed until:

```text
displayed camera frame
  == crop repository resolution camera frame
  == crop presentation surface camera frame
```

For acquisition video, the repository also requires the decoded crop-video
identity to equal the mapped video identity. For live geometry, the full-frame
surface identity and geometry identity must both equal the candidate camera
frame.

`CropPresentationCoordinator` implements this as a portable state machine:

- `Present` commits a new exact camera/crop pair;
- `Hold` reuses the already retained exact surface for the same identity;
- `Clear` commits an explicit missing, unavailable, or out-of-range crop; and
- `Wait` defers the candidate without changing the visible matched pair.

The first application integration committed the camera before acquisition
decode completed and cleared Crop Preview on `Wait`. Production playback
exposed that as black flashing. The corrected path stages the camera, stimulus,
and crop candidates and commits them together. While the next crop is pending,
the previous complete matched presentation remains visible.

## Metal Crop Preview

The main camera remains the playback clock owner. The app opens one shared
`ArchiveContext`, then opens stimulus and acquisition crop repositories and
their bounded Apple playback sessions. Crop Preview occupies the upper part of
the right media rail, with stimulus below it.

`AppleVideoMetalRenderer::encodeRegion()` adds validated normalized source
sampling without changing whole-frame callers. Geometry-only mode converts the
repository's full-frame crop rectangle into a normalized source region and
samples the retained camera `CVPixelBuffer` directly. Acquisition mode submits
the exact crop `CVPixelBuffer`. Both use the same render encoder and command
buffer as the camera and stimulus, preserving surface ownership through GPU
completion.

The source selector offers `Acquisition video` and `Live geometry`. Detection
boxes are transformed from full-frame coordinates into crop-output coordinates
with the shared scaled geometry contract. Blank acquisition rows still present
their exact encoded black surface and suppress the geometry overlay.

Geometry-only mode initially left the unused crop decoder running, adding a
third network video stream and causing visibly low playback frame rate. The app
now suspends and empties the crop decoder whenever live geometry is selected.
Switching back to acquisition mode restarts it with an explicit candidate seek.

## Automated Coverage

`crop_presentation_coordinator_tests` covers acquisition waits, exact presents,
retained same-frame holds, mapping and surface mismatches, source changes,
missing geometry, backward presentation, invalid state, and zero committed
camera skew.

`apple_crop_presentation_metal_tests` uses deterministic decoded video surfaces
to render acquisition and live-geometry crops into an offscreen Metal target.
It checks camera and crop pixels, the untouched gutter, forward and backward
identity, retained presentation during a deferred candidate, bounded buffering,
and zero skew.

`apple_video_metal_tests` additionally verifies normalized region sampling with
distinct BGRA source halves. The complete macOS headless preset passed 15/15.

## Production Validation

The production smoke is:

```bash
scripts/macos_gui_smoke_crop.sh [VIDEO] [ANALYSIS_ZARR] [START:END] \
  [acquisition|geometry]
```

Both modes passed `1024:1324` directly from the GoodCopBadCop network mount.

Acquisition video:

- final camera, crop camera, decoded video, and source frame were all `1324`;
- zero mapping or surface mismatches and zero camera skew;
- 84 deferred candidate refreshes with a maximum run of nine, retained rather
  than cleared;
- crop decoder peak depth six; and
- 300 source frames completed in 3.55 seconds.

Live geometry:

- final camera, crop camera, and source frame were all `1324`;
- zero deferred candidates, mismatches, or camera skew;
- zero crop seeks, follows, decoded frames, and buffered frames; and
- 300 source frames completed in 3.26 seconds with visually stable playback.

## NVIDIA Validation

The cumulative shared changes were applied to a detached worktree on the
maintained NVIDIA host. Configuration succeeded with CUDA 12.4, OpenCV 4.10,
TensorRT 10.0.1.6, NVIDIA FFmpeg, and the prebuilt TensorStore stack. `redgui`
linked with only the existing FFmpeg/OpenCV version-family warnings. All five
portable frame, crop, and stimulus CTests passed.

The authenticated NVIDIA `0:300` GUI smoke passed with final presented frame
`300`, 582 presentations, and 4.99 seconds elapsed.

## Next Checkpoint

Phase 4F-E completes native macOS acquisition-video and geometry-only Crop
Preview under the shared source-selection and exact-presentation contracts.
The next checkpoint should validate source switching and discontinuities over
longer network ranges, then extend overlay parity beyond the acquisition
detection rectangle without changing these identity or ownership rules.
