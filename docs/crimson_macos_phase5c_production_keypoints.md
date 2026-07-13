# Crimson Phase 5C Production Keypoints and Headings

Date: 2026-07-13

Phase 5C connects the macOS Metal camera view to Crimson's production
keypoint metadata. Skeleton edges, labeled keypoint markers, refined-quality
styles, and heading arrows now come from the same analysis Zarr used by the
maintained NVIDIA application. The metadata is resolved against the exact
decoded and presented camera frame before Metal receives a scene.

This remains a bounded Phase 5 checkpoint. Subject masks and contours, text
rasterization, editing, picking, and the remaining application surfaces stay
in later slices.

## Source Precedence

The TensorStore repository preserves the maintained loader's source order:

1. select `refined_keypoints_runs` when it has a supported latest-run pointer;
2. otherwise select `keypoints_runs`;
3. within the selected run prefer `keypoints_img`, then `keypoints_roi`, then
   `keypoints_norm`; and
4. load keypoint labels and skeleton edges from the run attributes and
   `pose_schema.edges`.

Supported latest-run pointers are checked in maintained order: `latest`,
`latest_completed`, `latest_complete`, and `latest_success`. Explicit run
requests may include either group prefix.

## Repository Contract

`KeypointOverlayRepository` is a backend-neutral, read-only contract. It
preloads metadata once when an archive opens, indexes every row by camera
frame, and resolves all detections for one requested frame into full-image
coordinates. It has no TensorStore, ImGui, Metal, OpenGL, CUDA, decoder, or
Python dependency.

The contract carries:

- camera frame, detection index, and source crop row identity;
- image, ROI, or normalized-ROI keypoint coordinates;
- normalized full-frame detection geometry and ROI placement;
- stored heading validity and angle;
- detection and keypoint interpolation provenance; and
- refined usability and flip-correction state.

`source_crop_row_ids` is the primary keypoint-to-crop join when present. Both
camera frame and detection index are validated against that referenced crop
row. Legacy runs without row IDs use same-position rows when they are aligned,
then exact frame/detection matching, and finally maintained per-frame row
order. A malformed explicit row identity fails repository construction rather
than drawing metadata on the wrong detection.

ROI coordinates add the source crop offset. Normalized ROI coordinates also
use the declared `roi_size`, with detection-box dimensions as the maintained
fallback. Image-space coordinates require no conversion. Detection boxes are
clamped to the full camera dimensions at resolution time.

## Scene and Metal Path

`makeKeypointOverlaySceneInput` is the portable adapter between repository
metadata and the Phase 5B read-only scene. It produces no detections for a
missing resolution and gives mismatched surface/metadata frames a stale
`FrameIdentity`, which the scene builder and both presentation backends
withhold.

The macOS shell resolves the repository only for the decoded camera frame that
is about to be presented. The resulting identity is:

```text
surface view 0, presented camera frame, overlay view 0, resolved camera frame
```

The adapter enables headings, skeletons, and markers but not its stored boxes.
Phase 5B continues to draw the selected crop detection box separately, so the
same box is not submitted twice. The keypoint overlay is independent of crop
pixel selection and therefore works with either acquisition crop video or live
geometry crop presentation.

## Deterministic Coverage

`keypoint_overlay_repository_tests` creates a Zarr v3 fixture through
TensorStore. Its refined rows are deliberately out of crop-row order, proving
that explicit source crop identities control placement. The test covers:

- refined-over-raw run precedence and explicit raw selection;
- image-over-ROI-over-normalized coordinate precedence;
- multiple detections in one camera frame;
- ROI and normalized coordinate conversion;
- detection box and interpolation provenance;
- refined usability and flip-correction state;
- exact, missing, out-of-range, and invalid-dimension resolution;
- skeleton, marker, and heading scene generation; and
- stale-frame scene withholding without a GUI.

The production probe loaded the June 14 GoodCopBadCop refined run with 120,221
rows, five keypoint labels, and six skeleton edges. Camera frame 1024 resolved
one detection with five finite keypoints and one valid heading.

## macOS Validation

The Apple Silicon release build completed and the full macOS preset passed 22
of 22 tests. The production live-geometry smoke passed camera frames 1024
through 1324 with zero camera skew, 235 keypoint-scene presentations, and 235
resolved detections. Crop presentation recorded 117 paired updates and zero
deferrals. Interactive review found the live-geometry playback and overlays
visually correct.

The acquisition-exact smoke also passed with zero camera skew, 222 keypoint
presentations, and 222 detections. It was visibly choppy: the crop coordinator
recorded only 16 paired updates and 114 deferrals, with eight crop seeks and no
ordinary follow requests. Each stream already has an independent decode
worker. The evidence points to the current six-frame exact-crop request policy:
main-camera catch-up skips trigger crop seeks, and exact-only presentation then
withholds late crop frames. A future playback checkpoint should keep the small
crop decoder running with bounded read-ahead and reserve seeks for real user
discontinuities. This is not attributed to Phase 5C overlay work.

## NVIDIA Validation

The cumulative source must still build the maintained NVIDIA application and
portable repository/scene tests and pass the production playback smoke before
this checkpoint is committed.
