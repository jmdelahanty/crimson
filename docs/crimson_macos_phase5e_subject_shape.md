# Crimson Phase 5E Production Subject Shape

Date: 2026-07-14

Phase 5E connects the native macOS camera view to Crimson's production
subject-shape analysis. Exact subject rows supply the body coordinate frame,
snout, caudal anchor, tail base and tip, centerline, B-spline, and optional
debug samples. The shared read-only scene converts ROI-local scientific
coordinates into full-camera pixels and submits the resulting vectors through
the existing Metal overlay renderer.

This is a read-only parity slice. Subject-mask component contours remain owned
by Phase 5D. Subject-shape QC navigation, GUI visibility controls, analysis
mutation, and tail-kinematics overlays remain later work. The shared scene
contract exposes the maintained visibility switches so a later macOS control
surface does not require a renderer or repository redesign.

## Source Contract

`SubjectShapeOverlayRepository` is backend neutral. It contains no TensorStore,
Metal, OpenGL, CUDA, ImGui, decoder, or Python dependency. A mapped resolution
contains every verified subject detection for one camera frame, including:

- subject-shape row, refined-mask lineage ID, and crop row;
- camera frame and detection identity;
- full-camera crop placement and ROI coordinate dimensions;
- body-frame origin, forward axis, and left axis;
- snout, caudal anchor, tail base, and tail tip landmarks;
- centerline validity and whether it reaches the snout;
- sampled B-spline and B-spline control points; and
- tail samples and normals for opt-in diagnostics.

The vector repository used by deterministic tests and the TensorStore
repository publish the same payload. `Missing`, `OutOfRange`,
`InvalidDimensions`, and `ReadFailed` remain distinct from a mapped row so the
playback layer can withhold unavailable data without treating corrupted reads
as ordinary gaps.

## Run and Row Identity

The TensorStore implementation selects
`analysis/subject_shape_runs/<latest_complete>` unless an explicit run is
requested. The production contract requires:

```text
row_axis = refined_subject_mask_rows
source_refined_subject_masks_run = <run name>
```

`row_index/source_refined_row_ids` contains lineage identifiers, not positional
array indices. Because the row axis is refined-mask rows, subject-shape row
position `i` must match refined-mask row position `i`. Repository construction
verifies all of the following before publishing any row:

```text
shape.frame[i]          == refined_mask.frame[i]
shape.detection[i]      == refined_mask.detection[i]
shape.refined_row_id[i] == refined_mask.source_refined_row_id[i]
shape.crop_row[i]       == refined_mask.source_crop_row_id[i]
shape.frame[i]          == crop.frame[shape.crop_row[i]]
shape.detection[i]      == crop.detection[shape.crop_row[i]]
```

Negative or out-of-range crop rows fail repository construction. A mismatch
fails closed instead of drawing valid-looking geometry on the wrong subject.
The run's schema ID/version, method/version, row axis, and head endpoint
semantics remain visible in the descriptor for diagnostics.

## Coordinates

Subject-shape points are stored in refined-mask ROI pixel coordinates. The
refined `masks_roi` dimensions define `coordinate_width` and
`coordinate_height`; the source crop run supplies `roi_size` and
`roi_coordinates_full`. The shared scene performs the maintained conversion:

```text
full_x = roi_x + local_x / coordinate_width  * roi_width
full_y = roi_y + local_y / coordinate_height * roi_height
```

Coordinates use the Phase 5 canonical top-left origin with +y downward. The
maintained ImPlot path performs its own plot-axis flip; the shared scene and
Metal path do not. Non-finite points are omitted. The ROI placement itself must
be finite and positive.

## Async and Chunk Policy

The render thread never reads subject-shape Zarr arrays. A bounded
`SubjectShapeOverlayBuffer` owns one worker, requests six frames ahead, and
retains at most 16 immutable frame resolutions. It displays geometry only when
the cached resolution exactly matches the presented camera frame. It never
holds the last available shape over a newer video frame.

Seeks, backward movement, and large forward jumps increment a generation,
clear pending/cache state, and discard results produced by the old generation.
Normal playback prunes requests that have fallen behind the active read-ahead
window. Buffer metrics report queue/cache peaks, exact cache hits, resolved,
missing, failed and discarded results, and worst resolve time.

Production geometry arrays use 256-row chunks. A cold row request reads all
required validity, point, and sequence arrays for its aligned 256-row window in
parallel, materializes neutral geometry, and keeps at most two such windows.
Subsequent frame resolutions in either cached window are memory lookups. The
two-window geometry cache is only a few MiB and is independent of the 16-frame
presentation cache.

This policy is important on PRFS, VPN, and Wi-Fi. A cold chunk may still arrive
after video has advanced; those frames intentionally render without shape
geometry. Video playback is never stalled and stale geometry is never shown.
Automated smokes wait for the exact initial frame before starting the playback
clock, making startup validation deterministic. `--no-subject-shapes` isolates
playback when diagnosing storage or overlay performance.

## Shared Scene

Default subject-shape rendering matches the maintained NVIDIA UI:

- centerline: yellow 2.2-pixel polyline;
- B-spline sample: green 2.0-pixel polyline;
- snout: orange circle;
- tail base: pink diamond;
- tail tip: purple cross; and
- caudal anchor: blue upward triangle.

Body forward/left axes, B-spline sample markers and control points, tail sample
markers, and tail normals are available as opt-in shared-scene switches. Axis
and normal lengths are derived in ROI coordinate space before conversion, just
as in the maintained renderer. Every subject-shape primitive uses the
`SubjectShape` layer between subject masks and keypoints in the shared camera
layer order.

The Metal backend needs no subject-specific shader. It consumes the shared
polyline and marker primitives, applies the existing source viewport transform,
and enforces the camera display rectangle through the existing scissor.

## Deterministic Coverage

`subject_shape_overlay_repository_tests` creates a real Zarr v3 archive through
TensorStore. It covers:

- latest-run selection and descriptor metadata;
- positional row-axis alignment, explicit lineage identifiers, and rejection
  of an incompatible row axis;
- deliberately reordered camera rows and multiple detections in one frame;
- exact crop placement and ROI-to-full-camera scaling;
- all production validity, landmark, sequence, and diagnostic arrays;
- mapped, missing, out-of-range, invalid-dimension, and stale-frame behavior;
- rejection of a mismatched shape/refined-mask/crop join;
- default and opt-in shared-scene primitives; and
- bounded async read-ahead and discontinuity discard.

`apple_read_only_overlay_metal_tests` adds subject-shape pixel coverage in full
view and verifies stale-frame withholding. Existing mask fill/contour/zoom,
box, heading, skeleton, and keypoint assertions continue in the same test.

## Production Fixture

The Phase 5E production gate uses:

```text
2026-05-29T18-11-16Z_arena_1_GoodCopBadCop_analysis.zarr
```

On 2026-07-14 its latest complete run was:

```text
subject_shape_goodcopbadcop_arena1_eye_shape_20260713_v001
```

The schema is `analysis.subject_shape_runs` version 3 with method
`subject_shape_from_refined_masks_v8`. It references the same 143,305-row
refined-mask run and crop run used for placement. Coordinates are 512 by 512;
centerline, B-spline sample, and B-spline control arrays contain 64 points per
row, while tail sample and normal arrays contain 32 points.

The May 29 camera asset is 4512 by 4512 HEVC at 100 fps. The June 14 Phase 5D
fixture intentionally has no subject-shape run and remains the unavailable
overlay regression: opening it must log an optional-overlay message and keep
ordinary playback functional.

## Validation

The production repository probe selected the run above and resolved camera
frame 100 as one exact detection with 64 centerline and 64 B-spline points.
Parallel 256-row loading reduced the mounted-volume cold resolve from about
3,026 ms in the initial implementation to 471 ms in the standalone probe and
337 ms in the final GUI smoke.

The macOS production smoke passed frames 100 through 160. It published 35 exact
subject-shape presentations, resolved 262 frame requests from the chunk cache,
reported zero missing/failed/discarded results, and bounded the presentation
cache at 16 and pending queue at seven. The camera video reached frame 160 with
zero presentation skew and no render-thread wait. The offscreen Metal pixel
test and the deterministic TensorStore fixture passed.

The isolated NVIDIA build completed with CUDA architectures 80 and 86. All 13
portable CTest targets passed, including the subject-shape TensorStore fixture.
The server-local production probe resolved frame 100 in 50 ms with the same run
and geometry counts. The maintained NVIDIA loader independently selected all
143,305 subject-shape rows, and its authenticated GUI smoke passed frames 100
through 160 on an RTX A6000 with 70 presented frames in 0.596 seconds. This
confirms that the new portable contracts compile and test on Linux without
changing the maintained CUDA/OpenGL subject-shape presentation path.
