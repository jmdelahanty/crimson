# Crimson Acquisition Crop Video Streams Plan

Date anchored: 2026-06-25.

## Purpose

Palette archives can now describe Orange runtime-acquired video streams in
`analysis/acquisition_video_streams`. In the inspected GoodCopBadCop and
RedScare archives this includes:

- an authoritative full-frame camera video, and
- a dedicated lossless crop video generated during Orange runtime.

This document summarizes what Crimson currently does, what the inspected
archives contain, and the smallest useful implementation path for loading and
displaying dedicated crop videos in the UI.

## Inspected Recordings

The user-provided base path was:

```text
/groups/johnsonlab/johnson/jeremy/recordings
```

That path was not present. The inspected recordings live under:

```text
/groups/johnson/johnsonlab/jeremy/recordings
```

GoodCopBadCop sample:

```text
/groups/johnson/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop
```

RedScare sample:

```text
/groups/johnson/johnsonlab/jeremy/recordings/2026-06-23T16-01-09Z_arena_1_RedScare
```

## Archive Metadata

Both inspected archives expose a Palette stream inventory at:

```text
analysis/acquisition_video_streams/zarr.json
analysis/acquisition_video_streams/streams/full/zarr.json
analysis/acquisition_video_streams/streams/crop/zarr.json
```

The group attrs use:

```text
schema_id = "palette.acquisition_video_streams.v1"
source_schema_id = "orange_runtime_video_streams_v1"
source_frame_clock = "recording_frame_id"
stream_keys = ["crop", "full"]
crop_stream_available = true
```

The `full` stream contract describes an authoritative full-frame stream:

```text
role = "ingest_authoritative_full_frame"
output_kind = "full"
coordinate_space = "full_frame_pixels"
video = "cams/Cam...mp4"
frame_clock = "recording_frame_id"
```

The `crop` stream contract describes the Orange sidecar crop stream:

```text
role = "runtime_derived_acquisition_input"
output_kind = "crop"
video_pixel_coordinate_space = "crop_frame_pixels"
source_geometry_coordinate_space = "full_frame_pixels"
video = "derived/external_crop_recorder/..._crop_external.mp4"
metadata = "derived/external_crop_recorder/..._crop_meta.csv"
keyframes = "derived/external_crop_recorder/..._crop_external_keyframe.json"
frame_clock = "recording_frame_id"
geometry_columns = [
  "crop_x",
  "crop_y",
  "crop_w",
  "crop_h",
  "detection_x",
  "detection_y",
  "detection_w",
  "detection_h"
]
blank_frame_policy = "encode_black_frame_when_no_detection"
selection_policy = "largest_detection_by_confidence"
```

## Sample Video Properties

GoodCopBadCop arena 1:

```text
Full video:
  path: cams/Cam2010093_2026-06-14T21-12-08Z_arena_1.mp4
  codec: HEVC Main
  size: 4512 x 4512
  fps: 100
  frames: 140035

Crop video:
  path: derived/external_crop_recorder/Cam2010093_2026-06-14T21-12-08Z_arena_1_crop_external.mp4
  codec: HEVC Main
  size: 256 x 256
  fps: 100
  frames: 140035
```

RedScare arena 1:

```text
Full video:
  path: cams/Cam2010093_2026-06-23T16-01-09Z_arena_1.mp4
  codec: HEVC Main
  size: 4512 x 4512
  fps: 100
  frames: 139908

Crop video:
  path: derived/external_crop_recorder/Cam2010093_2026-06-23T16-01-09Z_arena_1_crop_external.mp4
  codec: HEVC Main
  size: 384 x 384
  fps: 100
  frames: 139908
```

In both samples, the dedicated crop video has the same frame count, FPS, and
duration as the full-frame video. The crop keyframe JSON reports `gop=1` style
behavior: every frame is keyframe-addressable. That makes random seeking much
more viable than a normal long-GOP auxiliary stream.

## Crop Sidecar Geometry

The crop metadata CSV has one row per crop-video frame. The inspected header is:

```text
recording_frame_id,local_frame_id,camera_frame_id,timestamp,timestamp_sys,
has_detection,blank_frame,detection_confidence,crop_x,crop_y,crop_w,crop_h,
detection_x,detection_y,detection_w,detection_h
```

Example RedScare first rows:

```text
recording_frame_id,local_frame_id,camera_frame_id,...,crop_x,crop_y,crop_w,crop_h,...
1,5205,5205,...,2521,3569,384,384,...
2,5206,5206,...,2521,3567,384,384,...
```

Example GoodCopBadCop first rows:

```text
recording_frame_id,local_frame_id,camera_frame_id,...,crop_x,crop_y,crop_w,crop_h,...
1,292754,30614,...,338,1853,256,256,...
2,292755,30615,...,338,1853,256,256,...
```

The encode CSV also maps crop-video frame order to source frame order:

```text
encode_index,source_frame_index,...,recording_frame_id,local_frame_id,...
0,0,...,1,...
1,1,...,2,...
```

For the inspected archives, Crimson can initially map UI frame `F` to crop-video
frame `F` and crop metadata row `F`. The metadata row has
`recording_frame_id == F + 1`.

## Current Crimson Behavior

Crimson currently auto-loads a single affiliated camera video from Zarr metadata.
The metadata path reads root and `raw_video` source path fields such as
`source_video_path`, `source_path`, and `source_video`.

Relevant code:

- `src/zarr_loader_detections.cpp`
  - `ZarrDetectionLoader::loadMetadata`
  - reads root video metadata and `raw_video` metadata.
- `src/media_session_loader.cpp`
  - `MediaSessionLoader::tryAutoLoadAffiliatedVideoFromZarr`
  - resolves one source video from `zarr_loader.getSourceVideoPath()`.
  - `MediaSessionLoader::loadSingleVideoMedia`
  - clears demuxers/cameras and sets `scene->num_cams = 1`.
- `src/ui_path_config.cpp`
  - `ResolveAffiliatedVideoPath`
  - resolves source video hints relative to the archive and recording root.

Crimson does not currently discover or use
`analysis/acquisition_video_streams`. The crop-video metadata is present in
Palette archives but invisible to the media loader.

Crimson's current Crop Preview is related, but it is not the same feature. The
Crop Preview path uses:

- `LiveCropImageProvider`, which derives a crop from the current full-frame
  image and selected ROI geometry, and
- `ZarrPersistedCropProvider`, which falls back to Zarr `crop_runs/<run>/roi_images`.

It does not read Orange `derived/external_crop_recorder/*_crop_external.mp4`.

## Recommended Runtime Model

The full-frame video should remain Crimson's canonical editing and overlay
coordinate space.

The Orange crop video should be treated as an auxiliary acquisition stream:

- synchronized to the current Crimson display frame,
- displayed in a separate Crop Video window or optional inset,
- optionally used as a high-quality local visual source,
- not used as the canonical coordinate space for detection, keypoint, or mask
  storage.

This keeps Crimson aligned with the existing full-frame editing direction while
making the dedicated crop video useful for users who have it.

## Proposed Architecture

Add a small acquisition video stream model loaded from
`analysis/acquisition_video_streams`:

```text
AcquisitionVideoStream
  stream_key
  role
  output_kind
  camera_id
  stream_id
  video_path
  metadata_path
  keyframes_path
  summary_path
  coordinate_space
  source_geometry_coordinate_space
  frame_clock
  width
  height
  frame_count
  frame_rate
  codec
  blank_frame_policy
```

Then add a crop sidecar index:

```text
CropVideoFrameGeometry
  recording_frame_id
  local_frame_id
  camera_frame_id
  timestamp
  timestamp_sys
  has_detection
  blank_frame
  detection_confidence
  crop_x
  crop_y
  crop_w
  crop_h
  detection_x
  detection_y
  detection_w
  detection_h
```

The first implementation can load this CSV once when the crop video is enabled.
For these recordings the CSV is around 16 MB and about 140k rows, so eager
parsing on user enable is acceptable. Later, if needed, this can be replaced by a
binary sidecar or a Palette Zarr array.

## Decoder And UI Approach

Do not add the crop video as another `scene->camera` in the first slice. The
camera path currently assumes a single primary media source in important places,
and overlays are tied to full-frame image coordinates.

Instead, reuse the shape of the existing stimulus playback path:

- separate demuxer,
- separate decoder context,
- separate decoder thread,
- separate frame buffer,
- separate texture,
- independent window draw function,
- sync/update logic driven by current camera frame.

The stimulus path already has most of the needed mechanics:

- `StimulusPlayback` owns the secondary media state.
- `initializeStimulusPlayback` opens the video and starts the decoder.
- `updateStimulusPlaybackPresentation` chooses the target frame, manages
  catch-up seeking, throttles decode, and uploads a decoded frame to texture.
- `drawStimulusPlaybackDebugWindows` draws the secondary video texture.

This should likely be generalized into a reusable secondary video playback type
instead of copying stimulus-specific naming into a crop-video feature.

## Coordinate Transform

For full-frame overlays displayed over the crop video:

```text
x_crop = (x_full - crop_x) * crop_video_width / crop_w
y_crop = (y_full - crop_y) * crop_video_height / crop_h
```

In the inspected archives, `crop_w` and `crop_h` equal the crop video width and
height, so the first implementation is equivalent to:

```text
x_crop = x_full - crop_x
y_crop = y_full - crop_y
```

The scaled form should still be implemented so Crimson does not depend on that
coincidence.

Rows with `blank_frame == 1` or `has_detection == 0` should display the crop
video frame but suppress geometry-derived overlays, or clearly mark them as
unavailable.

## Smallest Useful Implementation Slice

1. Add a read-only loader for `analysis/acquisition_video_streams`.
2. Resolve stream paths relative to the recording root or Zarr archive root.
3. Store discovered `full` and `crop` stream descriptors in `ZarrDetectionLoader`.
4. Add UI status showing available acquisition video streams.
5. Add an on-demand Crop Video window.
6. Load the crop MP4 with a secondary-video playback path.
7. Sync crop-video frame `F` to current display frame `F`.
8. Parse `crop_meta.csv` once when the crop video is enabled.
9. Show basic status for current frame:
   - display frame,
   - crop video frame,
   - `recording_frame_id`,
   - `camera_frame_id`,
   - `crop_x/y/w/h`,
   - `has_detection`,
   - `blank_frame`.
10. Draw the current full-frame detection box transformed into crop-video pixels.
11. Add smoke logging for open, seek, and playback.

This slice gets users a real crop-video view without destabilizing the primary
camera path.

## Follow-Up Slices

After the first crop-video window works:

1. Generalize stimulus playback into a reusable secondary video playback module.
2. Add optional crop-video inset in the main camera view.
3. Add transformed keypoint overlays.
4. Add transformed subject-mask and eye-mask overlays.
5. Add crop-video source selection to Crop Preview:
   - live crop from full-frame video,
   - Orange dedicated crop video,
   - persisted Zarr `roi_images` fallback.
6. Add manual frame offset override for archives that do not have exact one-to-one
   frame parity.
7. Add contract validation warnings when video frame count, CSV row count, and
   stream contract `frame_count` disagree.
8. Consider Palette-side Zarr materialization of crop frame geometry if CSV parse
   cost or CSV schema drift becomes a problem.

## Open Contract Questions

The current samples are easy to map, but the contract should be explicit about
zero-based versus one-based frame identity:

- Crimson display frame and video frame are zero-based.
- `crop_meta.csv` row index is zero-based by file position.
- `recording_frame_id` starts at `1` in inspected samples.
- `encode_csv.source_frame_index` starts at `0`.

The next Palette contract clarification should state whether readers should map:

```text
display frame F -> crop video frame F -> crop_meta row F
```

and whether `recording_frame_id == F + 1` is guaranteed for these stream
inventories.

The contract should also clarify whether future crop videos may:

- have frame drops,
- have `frame_count != full frame_count`,
- have crop video frame offsets,
- use non-identity `source_frame_index`,
- use crop scaling where `crop_w/h` differ from encoded crop video dimensions.

## Validation Plan

Use the inspected samples for the first smoke:

```text
/groups/johnson/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop/zarr/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop_analysis.zarr

/groups/johnson/johnsonlab/jeremy/recordings/2026-06-23T16-01-09Z_arena_1_RedScare/zarr/2026-06-23T16-01-09Z_arena_1_RedScare_analysis.zarr
```

Validate:

- acquisition stream inventory is discovered,
- full-frame video still auto-loads normally,
- crop stream paths resolve,
- crop video opens,
- crop video seeks to early, middle, and late frames,
- crop video frame count and CSV row count match the contract,
- current-frame crop geometry is displayed,
- detection overlay follows the fish in crop-video pixels,
- playback still feels normal with the crop window open,
- no regressions in existing Crop Preview behavior.

