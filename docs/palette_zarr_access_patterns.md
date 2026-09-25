# Palette Zarr Access Patterns

Date anchored: 2026-06-24.

This note documents Crimson-side read/write/seek behavior for small Palette
Zarr array families that Palette may rechunk to reduce file-count and metadata
overhead.

Canary archive used for spot checks:

```text
/nvme1/recordings/chunking_canary_2026-06-24_heartrate/zarr/2026-06-14T21-12-08Z_arena_4_GoodCopBadCop_analysis.zarr
```

The canary contains the listed candidate arrays. The conclusions below are from
Crimson's reader and writer code, not from one archive's chunk layout.

## Reader Model

Most small lineage/count arrays are read as whole arrays through helper
functions and copied into `ZarrDetectionData` vectors. Random seeking and
overlay rendering then use those cached vectors; they do not re-read these Zarr
arrays per frame.

Relevant helpers:

- `src/zarr_loader_internal.h:169` opens Zarr arrays read-only.
- `src/zarr_loader.cpp:873` reads 1D integer arrays fully.
- `src/zarr_loader.cpp:945` reads 1D int64 arrays fully.
- `src/zarr_loader.cpp:1017` reads 1D float arrays fully.
- `src/zarr_loader.cpp:1082` reads 2D float matrix arrays fully.
- `src/zarr_loader.cpp:1146` reads 1D bool arrays fully.

## Access Table

| array family | Crimson access mode | cached? | write? | seek-path? | recommended Palette policy | evidence |
| --- | --- | --- | --- | --- | --- | --- |
| `crop_runs/*/bbox_norm_coords` | Not consumed by current Crimson crop metadata, keypoint placement, or mask placement paths. | No. | No. | No. | Safe to chunk larger from Crimson's perspective. | Search hits are detection/refined-detection paths, not crop-run reads. |
| `crop_runs/*/detection_indices` | Not consumed by current crop metadata or overlay placement paths. | No. | No. | No. | Safe to chunk larger from Crimson's perspective. | `loadMovementCropRunMetadata` reads crop `frame_indices`, not detection indices: `src/zarr_loader_movement.cpp:2482`. |
| `crop_runs/*/frame_indices` | Read fully for crop metadata and refined subject-mask crop-row verification. | Yes, in `data_.crop_data.frame_indices` and refined-mask placement vectors. | No. | No Zarr read during seek. | Safe to chunk larger or preload whole at run load. | Startup crop metadata fallback: `src/zarr_loader.cpp:789`; crop metadata loader: `src/zarr_loader_movement.cpp:2482`; refined mask source crop verification: `src/zarr_loader_eye_keypoint.cpp:1991`. |
| `crop_runs/*/source_detect_row_index` | Not consumed by current Crimson read paths. | No. | No for crop runs. | No. | Safe to chunk larger from Crimson's perspective. | Only write-side occurrence is manual refined detections under `refined_detect_runs/.../instances`: `src/zarr_loader_write.cpp:223`. |
| `crop_runs/*/source_refined_row_ids` | Not consumed by current Crimson read paths. | No. | No. | No. | Safe to chunk larger from Crimson's perspective. | No Crimson read references found. |
| `keypoints_runs/*/frame_indices` | Read fully when raw keypoints are the selected source. Used to map ROI rows to detection rows. | Yes, converted into detection-aligned keypoint/ROI vectors. | No. | No Zarr read during seek. | Safe to chunk larger or preload whole. | Raw keypoint fallback resolution: `src/zarr_loader_eye_keypoint.cpp:504`; full `frame_indices` read: `src/zarr_loader_eye_keypoint.cpp:530`; ROI-to-detection alignment: `src/zarr_loader_eye_keypoint.cpp:866`. |
| `keypoints_runs/*/detection_indices` | Not consumed by the keypoint overlay loader. | No. | No. | No. | Safe to chunk larger from Crimson's perspective. | The loader maps by `frame_indices` plus per-frame cursor, not this array: `src/zarr_loader_eye_keypoint.cpp:866`. |
| `keypoints_runs/*/source_detect_row_index` | Not consumed by current Crimson read paths. | No. | No. | No. | Safe to chunk larger from Crimson's perspective. | No Crimson read references found. |
| `keypoints_runs/*/source_refined_row_ids` | Not consumed by current Crimson read paths. | No. | No. | No. | Safe to chunk larger from Crimson's perspective. | No Crimson read references found. |
| `keypoints_runs/*/n_keypoints` | Not consumed. Crimson infers keypoint count from `keypoints_*` array shape. | No. | No. | No. | Safe to chunk larger from Crimson's perspective. | Shape inference: `src/zarr_loader_eye_keypoint.cpp:562`. |
| `keypoints_runs/*/n_rois` | Not consumed. Crimson uses `frame_indices.size()` as ROI count. | No. | No. | No. | Safe to chunk larger from Crimson's perspective. | ROI count assignment: `src/zarr_loader_eye_keypoint.cpp:530`. |
| `refined_keypoints_runs/*/frame_indices` | Preferred keypoint source; read fully at archive load. | Yes, converted into detection-aligned vectors. | No. | No Zarr read during seek. | Safe to chunk larger or preload whole. | Refined keypoints preferred: `src/zarr_loader_eye_keypoint.cpp:482`; full read: `src/zarr_loader_eye_keypoint.cpp:530`. |
| `refined_keypoints_runs/*/detection_indices` | Not consumed by current overlay loader. | No. | No. | No. | Safe to chunk larger from Crimson's perspective. | Mapping uses `frame_indices` plus per-frame cursor: `src/zarr_loader_eye_keypoint.cpp:866`. |
| `refined_keypoints_runs/*/source_detect_row_index` | Not consumed by current read or edit paths. | No. | No. | No. | Safe to chunk larger from Crimson's perspective. | Refined edit context reads metadata/keypoint shape, not lineage arrays: `src/refined_keypoint_repository.cpp:1821`. |
| `refined_keypoints_runs/*/source_refined_row_ids` | Not consumed by current read or edit paths. | No. | No. | No. | Safe to chunk larger from Crimson's perspective. | Refined keypoint row edits write keypoint/quality rows, not lineage arrays: `src/refined_keypoint_repository.cpp:2244`. |
| `refined_keypoints_runs/*/n_rois` | Not consumed. Crimson infers ROI count from `frame_indices` and `keypoints_roi` shape metadata. | No. | No. | No. | Safe to chunk larger from Crimson's perspective. | Loader ROI count: `src/zarr_loader_eye_keypoint.cpp:530`; edit context shape read: `src/refined_keypoint_repository.cpp:1873`. |
| `detect_runs/*/frame_counts` | Read fully only as fallback if `n_detections` is unavailable. Used to build frame offsets. | Yes, `data_.n_detections` and `data_.frame_offsets`. | No for `detect_runs`. | No Zarr read during seek. | Safe to chunk larger or preload whole. Larger chunks should reduce full-read chunk/file overhead. | Detection run load: `src/zarr_loader.cpp:1538`; full counts read: `src/zarr_loader.cpp:1804`; offset build: `src/zarr_loader.cpp:1993`. |
| `detect_runs/*/n_detections` | Read fully at detection-run load. | Yes, `data_.n_detections` and `data_.frame_offsets`. | No for `detect_runs`. | No Zarr read during seek. | Safe to chunk larger or preload whole. Larger chunks should reduce full-read chunk/file overhead. | Full read and fallback order: `src/zarr_loader.cpp:1804`; cached primary vectors: `src/zarr_loader.cpp:1677`; render uses cached offsets: `src/zarr_loader_detections.cpp:1213`. |
| `analysis/track_kinematics_runs/*/tracks/*/frame_indices` | Startup only discovers track-kinematics availability. Actual track arrays are read fully after deferred movement load. | Yes, per `MovementSeries`; also builds `frame_to_row`. | No. | No Zarr read after load. | Safe to chunk larger. Startup is unaffected; deferred load should benefit from fewer chunks/files. | Startup discovery: `src/zarr_loader.cpp:781`; lazy UI button: `src/gui/analysis_timeline_motion_sources.cpp:145`; deferred load: `src/zarr_loader_movement.cpp:459`; frame map: `src/zarr_loader_movement.cpp:2468`. |
| `analysis/track_kinematics_runs/*/tracks/*/detection_indices` | Read fully during deferred movement load if present. | Yes, per `MovementSeries`. | No. | No Zarr read after load. | Safe to chunk larger. | Read candidate list: `src/zarr_loader_movement.cpp:2139`; cached assignment: `src/zarr_loader_movement.cpp:2455`. |
| `analysis/track_kinematics_runs/*/tracks/*/positions_px` | Read fully during deferred movement load. | Yes, per `MovementSeries`. | No. | No Zarr read after load. | Safe to chunk larger. | 2D vector read: `src/zarr_loader_movement.cpp:2265`; `positions_px` read: `src/zarr_loader_movement.cpp:2296`; seek sample uses cached vector: `src/zarr_loader_movement.cpp:2733`. |
| `analysis/track_kinematics_runs/*/tracks/*/positions_mm` | Read fully during deferred movement load. | Yes, per `MovementSeries`. | No. | No Zarr read after load. | Safe to chunk larger. | `positions_mm` read: `src/zarr_loader_movement.cpp:2299`; cached assignment: `src/zarr_loader_movement.cpp:2452`. |
| `analysis/track_kinematics_runs/*/tracks/*/heading/speed/time arrays` | Read fully during deferred movement load. | Yes, per `MovementSeries`. | No. | No Zarr read after load. | Safe to chunk larger. | Time reads: `src/zarr_loader_movement.cpp:2142`; speed reads: `src/zarr_loader_movement.cpp:2176`; heading reads: `src/zarr_loader_movement.cpp:2250`; cached assignment: `src/zarr_loader_movement.cpp:2444`. |

## Seek And Overlay Path

During random timeline seeking, Crimson does not synchronously touch the
candidate lineage/count arrays in Zarr. It uses cached vectors:

- Detection boxes use cached `frame_offsets`, boxes, scores, and class vectors:
  `src/zarr_loader_detections.cpp:1188`.
- Keypoint overlays use cached detection-aligned keypoint vectors built from
  keypoint `frame_indices`: `src/zarr_loader_eye_keypoint.cpp:845`.
- Refined subject-mask overlays use cached `mask_rows_by_frame`,
  `source_crop_row_ids`, crop frame verification results, and crop ROI offsets:
  `src/zarr_loader_eye_keypoint.cpp:1940`.
- Track-kinematics display uses cached `MovementSeries::frame_to_row` and
  cached series vectors after deferred load: `src/zarr_loader_movement.cpp:2680`.

Modern refined subject masks are the main overlay path that requires crop
placement metadata:

```text
display frame
-> refined_subject_masks_runs/<run>/frame_indices
-> mask row
-> source_crop_row_ids[mask row]
-> crop_runs/<source_crop_run>/frame_indices[ crop row ] verification
-> crop_runs/<source_crop_run>/roi_coordinates_full[ crop row ] placement
```

The generic crop/keypoint lineage arrays `source_detect_row_index` and
`source_refined_row_ids` are not used for this placement path.

## Write Paths

The listed `detect_runs`, `crop_runs`, `keypoints_runs`, and
`analysis/track_kinematics_runs` candidate arrays are treated as read-only by
Crimson.

Two adjacent write paths are worth keeping separate from the chunking decision:

- Manual refined detections rewrite arrays under
  `refined_detect_runs/<run>/instances/`, including `frame_indices`,
  `bbox_norm_coords`, `frame_counts`, and `source_detect_row_index`:
  `src/zarr_loader_write.cpp:6`.
- Refined keypoint editing writes row slices of keypoint and quality arrays,
  not the listed lineage arrays: `src/refined_keypoint_repository.cpp:858` and
  `src/refined_keypoint_repository.cpp:2244`.

## Chunking Guidance

Safe to chunk larger or preload whole for Crimson:

- `detect_runs/*/frame_counts`
- `detect_runs/*/n_detections`
- `crop_runs/*/frame_indices`
- all listed crop/keypoint/refined-keypoint lineage arrays that Crimson does
  not currently consume
- `keypoints_runs/*/frame_indices`
- `refined_keypoints_runs/*/frame_indices`
- all listed track-kinematics arrays

For these arrays, larger row chunks such as `16384` or `65536` should generally
improve Crimson's load behavior or not matter. Crimson reads them fully when it
reads them at all, then caches the result for the loaded archive.

Keep conservative or decide separately for non-candidate heavy/editable arrays:

- `refined_keypoints_runs/*/keypoints_roi`
- `refined_keypoints_runs/*/keypoints_img`
- `refined_keypoints_runs/*/keypoints_norm`
- refined keypoint quality/heading/reason arrays that are edited row-wise
- `refined_subject_masks_runs/*/masks_roi`
- `refined_subject_masks_runs/*/mask_rle/**`
- `crop_runs/*/roi_images`

Those arrays have row/chunk-sensitive render or edit behavior and should not be
rechunked based only on the small-lineage-array access pattern.

