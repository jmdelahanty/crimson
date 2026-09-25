# Crimson Phase 5F Production Eye Geometry

Date: 2026-07-14

Phase 5F connects the native macOS camera view to Crimson's production
eye-angle analysis. Exact eye rows supply refined ellipse axes, body-frame gaze
directions, per-eye signed angles, eye-frame angles, and vergence. The shared
read-only scene converts ROI-local scientific coordinates into full-camera
pixels and sends vector geometry through Metal while ImGui draws clipped text
annotations in the same camera viewport.

This is a read-only parity slice. The Mac shell does not yet expose the
maintained overlay-debug controls, analysis timeline, representation selector,
or eye-analysis mutation workflows. The shared scene contains independent
visibility switches for axes, gaze rays, signed-angle arcs, visual cones, and
labels so those controls can be connected without changing the repository or
renderer contracts.

## Source Contract

`EyeGeometryOverlayRepository` is backend neutral. It contains no TensorStore,
Metal, OpenGL, CUDA, ImGui, decoder, or Python dependency. A mapped resolution
contains every verified detection for one camera frame, including:

- eye row, camera frame, detection identity, and source crop row;
- full-camera crop placement and ROI coordinate dimensions;
- frame and per-eye validity;
- refined left/right ellipse major and minor axes;
- body-frame origin, forward axis, and anatomical-left axis;
- left/right gaze vectors and signed gaze angles;
- left/right Bianco/Engert eye-frame angles; and
- eye-frame vergence.

The vector repository used by neutral tests and the TensorStore implementation
publish the same payload. `Missing`, `OutOfRange`, `InvalidDimensions`, and
`ReadFailed` remain distinct from a mapped result. An unavailable optional run
does not stop video playback, while an array read failure is reported and never
presented as an ordinary missing frame.

## Run and Channel Selection

The TensorStore repository selects
`analysis/eye_angle_runs/<latest_complete>` unless a run is requested
explicitly. Phase 5F supports the production schema-5 `compact_dense_v2`
layout. Logical channel names are resolved through the run's
`angle_channel_index`, `vector_channel_index`, and `qa_channel_index`; numeric
column positions are never hard-coded.

The maintained camera-overlay precedence is preserved:

```text
labels:     left/right eye-frame angle, then gaze-signed fallback
arcs:       left/right gaze-signed angle in the stored body frame
rays/cones: stored left/right gaze vector, then ellipse minor-axis fallback
vergence:   stored eye-frame vergence, then left + right eye-frame angles
```

Required names must be marked ROI-available and point inside their backing
array. A named compact channel outside the array shape fails repository
construction instead of silently reading another field. Refined eye ellipse
parameters and ellipse-success flags come from the run named by
`source_refined_subject_masks_run`; the eye-angle run supplies the body frame,
angles, vectors, and QA channels.

## Row Identity

The production run declares:

```text
row_axis = keypoint_detection_rows
source_refined_subject_masks_run = <run name>
```

Eye rows and refined-mask rows are positionally aligned. The refined-mask row
contains the explicit crop-row join. Repository construction verifies:

```text
eye.frame[i]         == refined_mask.frame[i]
crop_row             = refined_mask.source_crop_row_id[i]
0 <= crop_row        < crop.row_count
eye.frame[i]         == crop.frame[crop_row]
```

Detection identity and full-camera ROI placement are taken from that verified
crop row. A row-count mismatch, negative/out-of-range crop row, or frame
mismatch fails closed before any eye geometry can be published. The selected
schema ID/version, method/version, row axis, layout, refined-mask run, and crop
run remain visible in the descriptor and startup diagnostics.

## Coordinates

Ellipse points, body axes, and gaze vectors use ROI image coordinates with a
top-left origin, +x right, and +y down. The source crop run supplies
`roi_coordinates_full` and `roi_size`. The shared scene maps points with:

```text
full_x = roi_x + local_x / coordinate_width  * roi_width
full_y = roi_y + local_y / coordinate_height * roi_height
```

Vectors are scaled by the same per-axis ratios before normalization. This is
equivalent to the maintained path for the production 512-by-512 ROI and also
remains correct if stored geometry and crop dimensions differ. Non-finite or
zero-length geometry is withheld. When a gaze vector is invalid, the outward
minor-axis endpoint is the endpoint farthest from the ROI subject center,
matching the maintained renderer's fallback rule.

## Async and Chunk Policy

The render thread never reads eye Zarr arrays. A bounded
`EyeGeometryOverlayBuffer` owns one worker, requests six frames ahead, and
retains at most 16 immutable frame resolutions. Geometry is appended only when
the cached resolution exactly matches the presented camera frame. Late data is
not held over a newer video frame.

Seeks, backward movement, and large forward jumps increment a generation,
clear queued/cache state, and discard results from the old generation. Normal
playback prunes requests behind the active read-ahead window. Metrics report
requests, exact cache hits, resolved/missing/failed/discarded results, queue and
cache peaks, and worst resolve time.

Repository construction eagerly validates the relatively small identity and
lineage arrays. Geometry remains lazy. A cold request reads all required
ellipse, body-frame, angle, vector, and QA arrays for one aligned 256-row window
in parallel, materializes neutral rows, and keeps at most two windows. The
16-frame presentation cache is independent of this two-window geometry cache.

This prevents PRFS, VPN, or Wi-Fi latency from stalling video. Automated smokes
wait for the exact initial eye result before starting the playback clock, which
makes the startup assertion deterministic without introducing a render-thread
read. `--no-eye-geometry` isolates video and the other overlays during
diagnosis.

## Shared Scene

The Phase 5F scene mirrors the maintained eye presentation:

- left and right Feret major/minor axes use distinct green and purple colors;
- gaze rays originate at the eye center and end in a circular marker;
- signed gaze arcs use the stored body-forward and anatomical-left axes;
- translucent 163-degree visual cones follow each gaze direction;
- cone intersection is filled green as the visual-field overlap;
- per-eye labels prefer eye-frame angles and identify gaze-signed fallbacks;
  and
- the overlap label reports eye-frame vergence.

Eye primitives remain in the maintained `SubjectMasks` camera layer alongside
the refined eye components and before subject shape and keypoints. The shared
renderer contract adds a filled polygon primitive for the cones and overlap;
Metal tessellates it into triangles and uses the existing camera scissor. Text
annotations are transformed by the same source viewport and clipped to the
camera display rectangle before ImGui submission. No eye-specific Metal shader
or CPU image readback is required.

## Deterministic Coverage

`eye_geometry_overlay_repository_tests` creates real Zarr v3 archives through
TensorStore. It covers:

- latest-run selection and descriptor metadata;
- deliberately reordered rows and multiple detections in one camera frame;
- refined-mask and crop-row lineage validation;
- compact name-to-channel resolution and out-of-shape channel rejection;
- ellipse axes, body frame, gaze vectors, eye-frame/signed angles, and
  vergence;
- exact crop placement and ROI-to-full-camera conversion;
- mapped, missing, out-of-range, invalid-dimension, and stale-frame behavior;
- cone, overlap-polygon, label, and tessellation generation without a GUI; and
- bounded asynchronous read-ahead and exact cache publication.

`apple_read_only_overlay_metal_tests` adds an exact synthetic eye frame to the
combined offscreen overlay fixture. It asserts polygon and label counts,
triangle generation, non-background pixels in the eye region, and stale-frame
withholding while retaining the existing mask, subject-shape, keypoint,
heading, zoom, and scissor coverage.

## Production Fixture

The Phase 5F gate uses:

```text
2026-05-29T18-11-16Z_arena_1_GoodCopBadCop_analysis.zarr
```

On 2026-07-14 its latest complete eye run was:

```text
eye_angles_goodcopbadcop_arena1_eye_shape_20260713_v001
```

The run is `analysis.eye_angle_runs` schema 5 with method
`ellipse_and_centroid_eye_angles`, method version `eye_angle_analysis.v5`, and
layout `compact_dense_v2`. It contains 143,305 rows and references refined-mask
run
`refined_subject_masks_smart_finalizer_goodcopbadcop_prejuly_masks_wave01_20260712_v001`
and crop run `crop_2026-06-04_20-44-17`. The compact arrays contain 141 scalar
angle channels, two vector channels, and seven QA channels. Geometry
coordinates and crop output are 512 by 512. The corresponding camera asset is
4512 by 4512 HEVC at 100 fps with 143,447 frames.

## Validation

The mounted-volume probe selected the run above and resolved camera frame 100
as one exact detection with both eye axes and both gaze vectors valid. The
final exact-tree probe spent 3,803 ms discovering and validating the archive
and 156 ms loading the cold 256-row geometry window. The macOS production
smoke passed frames 100 through 160. It published 38 exact
eye-geometry presentations with 38 detections and 114 labels, resolved 156
frame requests from the geometry-window cache, reported zero missing, failed,
or discarded results, and bounded the presentation cache at 16 and pending
queue at seven. Worst eye resolve time was 127.9 ms. The camera reached frame
160 with zero PTS error and zero late presentations. The complete macOS preset
passed all 24 tests, including the neutral TensorStore fixture and offscreen
Metal pixel test.

The cumulative source also configured and built in an isolated NVIDIA worktree
on `ws1`. The configure retained CUDA 12.4 with effective architectures 80 and
86, TensorRT 10.0.1.6, OpenCV 4.10.0 with SFM, the NVIDIA FFmpeg stack,
OpenGL/GLEW, and the maintained CUDA/NVDEC application. The 217-step build
completed `redgui`, all three CUDA translation units, the portable contracts,
and the eye probe/test. All 13 portable CTest targets passed.

The server-local production probe selected the same run and resolved frame 100
with two valid eyes and two valid gaze vectors. Archive discovery took 803 ms
and its cold 256-row geometry read took 58.0 ms. The maintained loader also
selected the same 143,305 ROI rows and 143,447 frame rows with default
representation `eye_frame`. With its existing eager optional-overlay
validation flag, it verified all 143,305 crop placements, loaded both refined
ellipse-axis sets and all four sampled contour components, and passed the
authenticated frames 100 through 160 GUI smoke on an RTX A6000 with 70
presentations in 0.593 seconds.

Normal NVIDIA startup deliberately defers the large refined ellipse/contour
read to its background optional-overlay worker. The ordinary smoke also passed
with 71 presentations in 0.592 seconds but ended before that worker published;
the eager run is therefore the eye-renderer gate. This confirms that Phase 5F
adds portable contracts without changing the maintained CUDA/OpenGL playback
path or its deferred startup policy.
