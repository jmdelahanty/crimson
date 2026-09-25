# Crimson Phase 5A Overlay Contract

Date: 2026-07-13

Phase 5A freezes the maintained camera and Crop Preview overlay behavior before
the Metal overlay port begins. It adds a portable coordinate, identity, layer,
and acceptance contract. It does not move playback ownership, alter source
selection, or add Metal overlay rendering.

## Scope

The parity baseline is the maintained NVIDIA/OpenGL/ImPlot camera view in
`src/gui/camera_view_window.cpp` and the Crop Preview in
`src/gui/crop_preview_window.cpp` and `src/gui/crop_keypoint_editor.cpp`.
Phase 5A covers overlays inside those image surfaces, their inset surfaces, and
their direct edit/review interactions. Other application panels are part of
later Phase 5 surface migration.

The portable contract is `src/overlay_scene_contract.h`. It has no ImGui,
ImPlot, OpenGL, Metal, CUDA, TensorStore, or video-decoder dependency. The
existing `CropFrameGeometry` remains authoritative for full-frame-to-crop
scaling; the overlay contract adds only the heading-normalized crop transform.

## Coordinate Contract

Canonical overlay coordinates are image pixels with a top-left origin, +x to
the right, and +y downward. Rectangles are `(x, y, width, height)` in that
space. The ImPlot adapter continues to convert source y to
`image_height - y`; Metal must consume the canonical convention directly or
perform an equivalent vertex transform.

`SourceViewportTransform` maps a visible source rectangle to display pixels and
back. It is the common definition for full view, zoom, pan, clipped geometry,
label anchors, and inverse hit testing. Stroke widths, point radii, handles,
and text sizes remain display-pixel quantities and therefore do not grow when
the image is zoomed.

Crop overlays use crop-local top-left coordinates. Full-frame geometry first
passes through `CropFrameGeometry::fullFrameToCrop`, including output scaling.
Heading normalization rotates around the unrotated crop center and maps into a
square output using:

```text
rx =  cos(a) * dx + sin(a) * dy + output_side / 2
ry = -sin(a) * dx + cos(a) * dy + output_side / 2
```

## Frame Identity

An overlay may composite only when its camera view and camera frame exactly
match the presented image surface. Invalid, stale, future, or cross-view data
must be withheld; it must never be reused merely because it is the most recent
available overlay. Mapped stimulus and acquisition-crop media additionally
retain their Phase 4 exact source-frame settlement rules.

This is deliberately stricter than a visual best effort. Paused stepping and
seeks must settle the image and all attached overlays on the requested camera
identity before presentation.

## Camera Inventory

The base camera image is drawn first. YOLO contours and the selected
full-frame keypoint editor precede the canonical middle sequence. The shared
`kCameraOverlayLayerOrder` is now consumed directly by the maintained NVIDIA
camera loop:

```text
bounding boxes -> bounding-box draft -> chaser -> movement trail ->
keypoint heading -> movement label -> subject masks -> subject shape ->
tail kinematics -> subject-mask picking -> keypoints
```

The post-sequence insets and legacy input are drawn last.

| Overlay | Source data and frame authority | Geometry / output | Interaction | Metal at 5A |
| --- | --- | --- | --- | --- |
| YOLO detections | Per-view boxes, labels, class IDs for `current_frame_num` | Full-frame rectangles and labels | Review only | Missing |
| Full-frame refined keypoint edit | Selected detection and heading spec for exact camera frame | Skeleton, markers, labels, stored/candidate heading, handles | Drag while paused; save/reset/state actions | Missing |
| Detection boxes | Zarr detections plus clean/interpolated/manual/added/selected provenance | Full-frame rectangles and labels | Selection/edit provenance | Missing |
| Bounding-box draft | `FullFrameRectEditState` for exact camera frame | Draft rectangle and handles | Draw, resize, move under edit policy | Missing |
| Chaser | Projected boxes, states, target and camera calibration | Footprints, points, target lines, glyphs, labels | Review only | Missing |
| Movement trail | Frame-indexed movement history | Polyline / points in full-frame coordinates | Toggle, duration, valid-only filter | Missing |
| Keypoint heading | Detection keypoints and heading computation | Arrow and label | Toggle; candidate changes during edit | Missing |
| Movement label | Exact movement sample and detection association | Position marker and text metrics | Toggle | Missing |
| Subject mask components | Body, swim bladder, left/right eye ROI masks | Filled raster/scatter fallback, contours, selection highlight | Component toggles | Missing |
| Eye geometry | Eye/body axes derived from exact masks | Visual cones, overlaps, gaze rays, arcs, angle labels | Independent toggles | Missing |
| Mask edit draft | Exact selected ROI and edit session | Brush cursor, polygon/lasso draft, edited mask preview | Pick, brush, erase, polygon, lasso while paused | Missing |
| Subject shape | Exact derived subject-shape row | Component contours, axes, landmarks, centerline, spline/debug samples | Per-component toggles | Missing |
| Tail kinematics | Exact subject shape and kinematics row | Samples, segments, angle vectors, deflection and invalid-state color | Per-feature toggles | Missing |
| Detection keypoints | Exact detection details | Skeleton edges, markers, labels; selected edited detection skipped | Toggle and select | Missing |
| Stimulus event | Events resolved for camera frame | Cached text annotation | Review only | Missing |
| Stimulus step direction | Step resolved for camera frame | Direction indicator | Review only | Missing |
| Stimulus video inset | Camera-to-stimulus mapping and exact decoded source frame | Image inset | Visibility / size controls | Present as separate Phase 4 panel, not yet camera inset parity |
| Chaser-distance polar inset | Exact camera-frame polar data | Polar plot inset | Visibility controls | Missing |
| Active ROI inset | Exact selected ROI, source texture, masks, keypoints and heading | Crop inset with optional heading normalization | Visibility / normalization controls | Missing |
| Legacy manual keypoints | Legacy map keyed by exact camera frame and view | Skeleton, markers and derived box | Keyboard labeling/navigation/delete | Missing |

`subject_mask_picking` occupies an ordering slot even though it is hit testing,
not a visible primitive. This preserves the rule that mask selection occurs
after mask/shape/tail drawing and before ordinary keypoint drawing.

## Crop Preview Inventory

The unrotated crop order is image, skeleton edges, keypoint markers, stored
heading, then an optional candidate heading. The heading-normalized review crop
uses image, skeleton edges, keypoint markers, and a zero-degree stored heading.
`kCropOverlayLayerOrder` freezes the primitive order shared by both variants.

| Surface / overlay | Source and identity | Geometry | Interaction | Metal at 5A |
| --- | --- | --- | --- | --- |
| Crop image | Selected live geometry, acquisition video, or persisted Zarr source for exact camera frame | Crop-local top-left | Source selection | Present |
| Skeleton edges | Refined keypoints for displayed crop ROI and camera frame | Crop-local lines | Review only | Missing |
| Keypoint markers | Same exact ROI row | Crop-local points and labels | Drag handles while paused on unrotated crop | Missing |
| Stored heading | Stored heading plus computed keypoint origin | Crop-local arrow | Toggle | Missing |
| Candidate heading | Dirty edited keypoints and heading dependency | Dashed crop-local arrow | Appears during paused edit | Missing |
| Heading-normalized crop | Same exact crop row and heading | Rotated square crop-local surface | Review only; refresh after save | Missing |
| Detection rectangle | `CropFrameGeometry::full_frame_detection` | Full-frame to crop-local rectangle | Review only | Present only on macOS as a Phase 4 diagnostic aid; not NVIDIA parity baseline |

Crop handle hit testing uses display pixels for its radius, converts the pointer
through the inverse crop transform, and clamps edits to crop bounds. Editing is
disabled while playback is running. Dirty rotated overlays intentionally wait
for save/refresh rather than displaying stale transformed keypoints.

## Deterministic Fixture

`tests/fixtures/overlay_scene_fixture.h` is fixture version 1. It fixes:

- view 1 and camera frame 137, including stale-frame and cross-view failures;
- a 960x540 source viewport mapped into a 1280x720 display rectangle;
- a partially clipped source rectangle and its expected display bounds;
- a 320x160 full-frame crop scaled to a 640x320 crop output;
- a detection rectangle transformed into that crop; and
- a 30-degree heading normalization into a 320x320 output.

Expected values are literals, not values generated by the implementation under
test. `overlay_scene_contract_tests` also freezes both layer-name sequences,
invalid input behavior, inverse transforms, and the acceptance thresholds. It
is a portable, headless CTest and requires no recording or Zarr store.

Later Phase 5 fixture versions must add representative primitives for every
inventory row, toggled-off cases, labels, mask rasters, edited states, full-view
and zoom/pan render targets. Fixture changes require an explicit contract
revision; backend-specific output updates alone are not acceptable.

## Phase 5 Acceptance

These thresholds are frozen before Metal overlay implementation:

- camera view/frame identity, mapped media identity, enablement, label text,
  primitive count, and layer order must match exactly;
- source-space geometry may differ by at most 0.25 source pixel;
- display-space anchors may differ by at most 0.5 display pixel;
- inverse hit-testing may differ by at most 0.5 source pixel;
- non-antialiased raster channels may differ by at most 3/255;
- binary mask intersection-over-union must be at least 0.995;
- at least 99.5% of vector reference coverage must lie within one display pixel,
  with no vector outlier beyond two display pixels;
- text compares exact content, visibility, layer, and anchor, not backend font
  rasterization; and
- edit/review actions must resolve the same view, frame, detection/ROI,
  keypoint/component, and persisted data mutation.

Anti-aliased image comparisons use the vector coverage rule rather than a raw
per-pixel equality test. Every render fixture runs at fixed display dimensions
in full-view and zoom/pan configurations. No Metal overlay path may require a
synchronous full-resolution GPU-to-CPU readback.

## Phase 5A Gate

Phase 5A is complete when the shared contract and fixture pass on macOS and the
maintained NVIDIA build, the NVIDIA camera consumes the shared layer order, and
this inventory has no unclassified camera or Crop Preview overlay. Actual
Metal overlay drawing begins in the next checkpoint.

## Validation

The macOS Apple Silicon release preset built the native application and the new
`overlay_scene_contract_tests` target. The complete headless/portable preset
passed 18 of 18 tests.

The cumulative source state was also configured in a fresh detached NVIDIA
worktree with CUDA 12.4 for architectures 80/86, OpenCV 4.10, TensorRT
10.0.1.6, NVIDIA FFmpeg, and the prebuilt TensorStore stack. `redgui` and the
portable overlay contract test built from a clean build directory; the test
passed. Existing CUDA/TensorRT/FFmpeg deprecation and FFmpeg version-family
linker warnings remained non-fatal.

The authenticated NVIDIA GUI smoke used the June 14 GoodCopBadCop production
Zarr and passed `0:300` with exact final presented frame 300, 350
presentations, and 2.99268 seconds elapsed. This confirms the maintained camera
still runs after moving its canonical middle layer order into the shared
contract.
