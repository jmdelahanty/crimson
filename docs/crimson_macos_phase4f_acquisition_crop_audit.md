# Crimson Phase 4F-A Acquisition Crop Audit

Date: 2026-07-12

Phase 4F-A audits the production acquisition crop contract before adding a
shared resolver or a macOS playback session. No runtime behavior changes in
this checkpoint.

## Scope

The acquisition crop stream is an Orange-produced, full-duration sidecar video
described by the Palette Zarr group:

```text
analysis/acquisition_video_streams
```

It is distinct from both existing Crimson crop features:

- Crop Preview derives an ROI from the currently presented full camera frame,
  with persisted Zarr `roi_images` as a fallback.
- Palette clipped playback replaces the primary camera video with one of
  several parent-frame-mapped clip videos.

Neither current path discovers or decodes the Orange acquisition crop MP4.

## Crop Source Model

Crimson needs three explicit crop-image sources under one camera-frame and
geometry contract:

1. `LiveGeometry`: derive a crop from the exact presented full camera frame and
   the resolved crop geometry. This is the normal path for geometry-only
   recordings and keeps the full frame as the canonical visual source.
2. `AcquisitionVideo`: decode the Orange full-duration crop sidecar and resolve
   it through `recording_frame_id`. This preserves the pixels produced during
   acquisition and provides a dedicated crop surface instead of deriving one
   from the current full-frame presentation surface.
3. `PersistedZarr`: use materialized `crop_runs/<run>/roi_images` as a
   compatibility or offline-artifact fallback, not as a runtime requirement.

All three sources must resolve from the same Crimson camera frame and expose
the same full-frame crop geometry. Source selection must not create a second
clock or change overlay coordinates. A recording without an acquisition crop
video is therefore not missing crop capability when full-frame media and valid
geometry are available.

## Production Evidence

The audit inspected two network-mounted recordings and their external CSV,
keyframe JSON, and video metadata:

```text
2026-06-14T21-12-08Z_arena_1_GoodCopBadCop
2026-06-23T16-01-09Z_arena_1_RedScare
```

Both inventories use:

```text
schema_id = palette.acquisition_video_streams.v1
schema_version = 1
source_frame_clock = recording_frame_id
stream_keys = [crop, full]
```

The Zarr nodes contain attributes rather than frame arrays. The crop frame map
therefore comes from the external `crop_meta.csv` file referenced by the crop
stream descriptor.

Root `attributes.streams` descriptors duplicate the `streams/full` and
`streams/crop` child-group attributes. Version 1 does not define conflict
precedence, so the loader should validate that duplicate descriptors agree
rather than silently choosing one. The `files.*` existence and size fields are
import-time diagnostics, not current-host filesystem truth. A missing optional
status file also cannot invalidate a stream whose required inventory is marked
available; this occurs for the production full stream.

| Property | GoodCopBadCop | RedScare |
|---|---:|---:|
| Full video | 4512 x 4512 | 4512 x 4512 |
| Crop video | 256 x 256 | 384 x 384 |
| Frame rate | 100 fps | 100 fps |
| Contract/video/CSV frames | 140035 | 139908 |
| Crop keyframes | 140035 | 139908 |
| Encoder-reported drops | 0 | 0 |
| Explicit blank frames | 1537 | 5 |

Both crop videos are HEVC Main, NV12/YUV420 video-range streams. Every video
frame is listed as a keyframe, and the first and last keyframe identities are
`0` and `frame_count - 1`.

GoodCopBadCop's recording manifest also warns that the crop MP4 lacks an `stss`
sample table despite the GOP-1 keyframe sidecar. The sidecar is useful contract
evidence but does not itself control AVFoundation or FFmpeg seeking, so exact
access remains a decoder validation gate. The `pixel_source_format = mono8`
provenance and limited-range video tags also need to retain Crimson's existing
direct-luma/color-range treatment rather than being inferred from the crop
geometry contract.

The full and crop metadata CSV files are byte-identical within each recording.
An exhaustive row audit found:

- `recording_frame_id == zero_based_row + 1` for every row;
- `local_frame_id` is contiguous across each recording;
- `camera_frame_id` wraps from `65535` to `1` twice in each recording;
- `blank_frame == 1` exactly when `has_detection == 0`;
- blank rows have zero geometry but still have an encoded video frame;
- detected crop geometry has the declared 256 or 384 pixel dimensions;
- crop rectangles stay inside the 4512 x 4512 full frame; and
- detection rectangles stay inside their crop rectangles.

`camera_frame_id` is consequently provenance, not a usable playback index.
`local_frame_id` is also producer provenance rather than Crimson's logical
frame identity.

## Verified Version 1 Mapping

For the two inspected version 1 inventories, zero-based Crimson camera frame
`F` resolves as:

```text
camera frame F
  -> crop video frame F
  -> crop CSV row F
  -> recording_frame_id F + 1
```

The resolver must validate this relationship when loading an archive instead
of silently deriving it from common media time. A future schema may add an
offset, source-frame index, gaps, or segmented media, but version 1 supplies no
such mapping structure.

A blank crop row is still `Mapped`. It selects a real encoded black video
frame and suppresses geometry-derived overlays. It is not `Missing`, a decoder
drop, or a request to retain the previous crop.

Version 1 describes one full-duration video per stream. The audited acquisition
crop streams therefore have no clip boundary, overlap, or handoff semantics.
Those concepts belong to the separate `PaletteClippedResolver` contract and
must not be imported into this resolver.

## Geometry Contract

Full-frame pixels remain the scientific coordinate system. For a crop video
with encoded dimensions `video_width` by `video_height`, a full-frame point is
mapped with:

```text
x_crop = (x_full - crop_x) * video_width / crop_w
y_crop = (y_full - crop_y) * video_height / crop_h
```

The audited recordings happen to have `crop_w == video_width` and
`crop_h == video_height`, but the shared transform must preserve the scaled
form. Geometry is unavailable on a blank/no-detection row even though the video
mapping remains exact.

## Current NVIDIA Behavior

Crop Preview follows `camera.last_uploaded_frame`, the camera frame actually
presented rather than the logical clock target. It prefers the live full-frame
texture, otherwise searches the camera display ring, and finally falls back to
persisted Zarr crop images. This presented-frame authority is the correct
identity to preserve for acquisition crop synchronization.

The current providers are not a portable acquisition-stream interface:

- `LiveCropImageProvider` exposes CUDA and OpenGL storage;
- `ZarrPersistedCropProvider` reads analysis ROI images rather than crop video;
- `CropImageProvider` exposes raw GL texture identifiers; and
- `StimulusPlayback` is independently owned secondary media, but its current
  implementation is CUDA/OpenGL and stimulus-policy specific.

The Zarr loader already supports a geometry-only form of this workflow. It
loads crop `frame_indices` and ROI dimensions without reading `roi_images`
unless `CRIMSON_EAGER_CROP_IMAGES=1`. `LiveCropImageProvider` then derives the
selected crop from the current full-frame texture or frame payload. Crop
selection can come from an edited box, refined keypoint or mask ROI, movement
ROI, or frame detection.

There is a current UI-discovery gap: the advanced Crop Preview gate checks
persisted crop pixels, keypoints, or eye masks, but has no `hasCropMetadata()`
condition. A recording containing only full-frame media plus crop geometry may
load enough metadata for live cropping while still failing to expose the
window. The shared source-capability contract should make `LiveGeometry`
availability explicit instead of inferring it from unrelated analysis types.

Palette clipped playback is also not reusable as the acquisition resolver. It
maps parent frames to clip-local frames, owns primary-camera decoder reloads,
and performs clip handoffs. Acquisition crop is an auxiliary stream with a
stable one-video identity.

The audit found one relevant lifetime defect in the legacy Crop Preview path:
its fallback ring scan passes mutable host/CUDA slot payloads to
`LiveCropImageProvider` without holding a `FrameSlotReadLease`. A concurrent
seek can reset those slots. Phase 4F must not copy this ownership pattern; the
existing defect should be fixed in a separate, scoped NVIDIA change.

## Shared Repository Boundary

The next slice should introduce a pure C++ acquisition crop repository whose
public types contain no CUDA, OpenGL, Metal, AVFoundation, or TensorStore types.
The repository should expose:

- the declared and resolved video, CSV, keyframe, summary, and status paths;
- stream identity, dimensions, rate, format, frame count, and coordinate spaces;
- per-camera-frame crop video and metadata-row identities;
- `Mapped`, `OutOfRange`, and invalid-contract outcomes;
- a blank/no-detection flag separate from mapping status;
- producer provenance IDs and optional valid full-frame geometry; and
- the full-to-crop coordinate transform.

Above that repository, a portable crop-source selection contract should report
`LiveGeometry`, `AcquisitionVideo`, and `PersistedZarr` capabilities separately.
The acquisition repository supplies sidecar identity and CSV geometry; existing
analysis repositories can supply geometry-only crop specifications. Pixel
production remains backend-owned.

The initial version 1 resolver should reject or clearly diagnose:

- unsupported schema ID or version;
- unavailable or incomplete crop inventory;
- missing required columns or malformed values;
- noncontiguous or non-one-based `recording_frame_id`;
- inconsistent blank/detection/geometry fields;
- contract, CSV, keyframe, and video count disagreement; and
- invalid dimensions, rate, or out-of-bounds geometry.

The TensorStore adapter should read Zarr v2/v3 group attributes and eagerly
parse the approximately 16 MB CSV once per session. CSV fields should be found
by header name rather than fixed position.

## Path Resolution Prerequisite

`ArchiveContext::resolveStoredPath()` currently repairs a foreign absolute path
only when it contains the recording directory name. Acquisition inventory
paths are normally recording-root-relative, such as:

```text
derived/external_crop_recorder/..._crop_external.mp4
```

Those paths currently remain unresolved. Before the repository adapter is
added, archive path resolution must support relative recording paths for all
sidecar types, not only videos, and retain the existing foreign-absolute-path
rebasing behavior. The internal TensorStore context and attribute reader also
need a narrow reusable boundary because they currently live inside the stimulus
repository implementation.

## Apple Playback Boundary

The Apple session should wrap the generic `AppleVideoPlaybackBuffer` with
stream identity `crop`. It accepts camera-frame requests, resolves only through
the repository, and verifies that the decoded frame identity equals the
resolved crop-video identity.

It must not own an autonomous clock. Pause, step, seek, and normal playback all
derive from the main camera's requested or actually selected identity. Later
Metal presentation can extend the Phase 4E atomic composite contract so a
mapped crop is committed only with its exact camera frame. A mapped blank crop
is an exact black crop surface, while an unavailable exact crop defers the
candidate composite rather than presenting mismatched content.

## Required Automated Coverage

The implementation checkpoint should add three layers:

1. Portable resolver tests for identity mapping, bounds, blanks, malformed
   rows, count mismatches, geometry validation, and scaled transforms.
2. Repository fixture tests for Zarr v2/v3 attributes, reordered CSV headers,
   relative and foreign absolute paths, unavailable streams, missing files,
   and inconsistent counts.
3. Apple session tests for forward, repeated, backward, and discontinuous
   camera requests; exact decoded identity; blank mapped frames; out-of-range
   requests; and bounded buffering.

A cross-platform repository probe should then validate both production
archives. An Apple playback probe should sample first, middle, last, and
backward-seek frames in both the 256 and 384 pixel crop videos. The existing
NVIDIA portable tests, `redgui` build, and authenticated playback smoke remain
required after shared-source changes.

## Phase Boundary

Phase 4F-A establishes the acquisition crop contract and implementation seam.
Phase 4F-B subsequently added the portable crop-source capability, exact-frame
selection, and geometry contract. The next checkpoint is 4F-C: add the
acquisition repository, generic archive path resolution, TensorStore/filesystem
adapter, and repository tests. It should not yet change Crop Preview, `redgui`,
or Metal presentation.
