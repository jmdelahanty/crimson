# Crimson Subject-Mask Painting Plan

Date anchored: 2026-04-29.

## Purpose

Document the intended Crimson brush model for refined subject-mask editing,
using Paintera as a reference for interaction design without coupling Crimson to
Paintera internals.

This is a UI/edit-preview design. It does not change the Palette Zarr contract
or the Palette-owned save boundary.

Related docs:

- [crimson_subject_mask_editing_architecture.md](./crimson_subject_mask_editing_architecture.md)
- [crimson_overlay_layering_and_mask_rendering_design.md](./crimson_overlay_layering_and_mask_rendering_design.md)
- [crimson_eye_mask_texture_and_editing_plan.md](./crimson_eye_mask_texture_and_editing_plan.md)

## Paintera Findings

Paintera's ordinary circular brush is not an anti-aliased saved-mask brush. The
saved edit is still a hard integer label mask.

The smooth feel comes from three design choices:

1. Paint into a temporary viewer-aligned mask first.
2. Sample drag movement densely, at roughly one stamp per screen pixel.
3. Stamp an integer circular disk at each sampled point.

Relevant Paintera paths:

- `PaintBrushTool.kt`
  - handles mouse press, drag, release, erase, and brush-size actions
- `PaintClickOrDragController.kt`
  - creates a `ViewerMask`
  - maps display coordinates into mask coordinates
  - samples drag paths with `numPaintCalls = draggedDistance.toInt() + 1`
  - submits the temporary mask on release
- `Paint2D.java`
  - `paintIntoViewer(...)` stamps a circular disk using ImgLib2
    `HyperSphereNeighborhood`
- `ViewerMask.kt`
  - stores the temporary viewer-space mask
  - projects it back into source/canvas space

Paintera's shape interpolation is separate from normal brush painting. It
computes signed Euclidean distance transforms on two painted slices, linearly
interpolates those distance fields, and thresholds the result. That idea may be
useful later for between-frame or between-slice propagation, but it should not be
part of the first brush tool.

## Recommended Crimson Brush Model

Crimson should use a dense binary preview mask in ROI coordinates for the active
`SubjectMaskEditSession`.

For a brush stroke:

1. On mouse down, record the active component, ROI row, brush mode, brush radius,
   and starting ROI coordinate.
2. Stamp the brush at the starting coordinate.
3. On mouse drag, convert the current camera/canvas coordinate to ROI pixel
   coordinates.
4. Compute the vector from the previous ROI coordinate to the current ROI
   coordinate.
5. Sample along that vector at no more than 1 ROI pixel spacing.
6. Stamp the brush at each sampled point.
7. Track the union dirty rectangle for all stamps.
8. Update the preview texture only over or because of that dirty rectangle.
9. On mouse release, leave the preview dirty and wait for explicit save or reset.

This gives continuous strokes even when mouse events arrive sparsely.

## Precomputed Integer Disk Footprints

A brush footprint is a cached list of offsets covered by a circular brush at a
given integer radius.

The direct test for a disk is:

```text
for dy = -radius..radius
  for dx = -radius..radius
    if dx*dx + dy*dy <= radius*radius
      paint(center_x + dx, center_y + dy)
```

Crimson should compute this once per integer radius and reuse it for every
stamp. Prefer row spans over individual pixels:

```text
radius 3:
  dy = -3: x = -1..1
  dy = -2: x = -2..2
  dy = -1: x = -2..2
  dy =  0: x = -3..3
  dy =  1: x = -2..2
  dy =  2: x = -2..2
  dy =  3: x = -1..1
```

Suggested structure:

```cpp
struct BrushSpan {
    int dy = 0;
    int x_min = 0;
    int x_max = 0; // inclusive
};

struct BrushFootprint {
    int radius_px = 0;
    std::vector<BrushSpan> spans;
};
```

Stamping then becomes row writes:

```text
for span in footprint.spans
  y = center_y + span.dy
  x0 = clamp(center_x + span.x_min, 0, width - 1)
  x1 = clamp(center_x + span.x_max, 0, width - 1)
  write mask[y][x0..x1] = foreground_or_background
```

The same footprint supports paint and erase. Paint writes `1`; erase writes `0`.

## Coordinate Policy

For the first Crimson implementation, paint in ROI mask coordinates, not full
source coordinates.

The camera view already knows how to place an ROI mask over the full camera
image. The editor should invert that same placement for picking:

- canvas/screen position
- camera image position
- ROI-local position
- integer mask pixel coordinate

If the point is outside the active ROI bounds, the stamp should be ignored. If
part of the footprint crosses the mask edge, clip spans to the mask dimensions.

This is simpler than Paintera's viewer-aligned 3D/source-space mask because
Crimson's refined subject masks are fixed 2D ROI planes.

## Preview And Rendering

Painting should mutate only `SubjectMaskEditSession::previewMask()` until save.

Render order:

1. Base video frame.
2. Persisted refined subject-mask fill/contour.
3. Active preview mask fill/contour above the persisted layer.
4. Eye axes, keypoints, handles, and edit affordances above masks.

The preview should use the same component color as the persisted component but
with a visually distinct alpha or outline so the user can tell what is unsaved.

For performance:

- keep a GPU texture for the active preview mask
- update it only when the dirty rectangle changes
- keep the dirty rectangle clipped to the ROI
- avoid rebuilding persisted mask textures during preview painting
- avoid Zarr reads during the stroke

## Undo And Stroke Transactions

Undo should operate at the stroke level, not per stamp.

Recommended approach:

- before mouse down, snapshot the changed rectangle lazily
- as stamps expand the dirty rectangle, keep either:
  - the original pixels for the final dirty rectangle, or
  - a compact list of old row spans touched by the stroke
- on mouse release, push one undo record containing:
  - component name
  - ROI row index
  - dirty rectangle
  - before pixels
  - after pixels or a redo operation

For the first implementation, a rectangle snapshot is simpler and likely fine
because ROI masks are only `512x512` in current refined subject-mask runs.

## Save Boundary

Saving remains owned by `SubjectMaskWritebackClient`.

Crimson should send the final binary preview mask to the configured backend.
Crimson should not recompute Palette metrics, contours, row revisions, or
provenance.

If save succeeds:

- reload or refresh the touched component row
- clear preview dirty state
- clear or rebase undo state

If save fails:

- keep the preview dirty
- preserve undo state
- surface the backend error

## Non-Goals

- Do not make brush edges anti-aliased in the persisted binary mask.
- Do not silently smooth boundaries after every stroke.
- Do not borrow Paintera's full 3D viewer-mask/canvas/persistence model.
- Do not write directly to Palette derived metadata from Crimson.
- Do not use Shape Interpolation as the initial brush behavior.

## Later Options

Optional future tools:

- smooth component boundary as an explicit command
- fill enclosed region from a clicked seed
- lasso or polygon selection
- distance-field interpolation between masks on neighboring frames
- brush hardness or soft preview display, while still saving binary masks

These should be separate tools or explicit commands so normal painting remains
predictable.

## Initial Implementation Checklist

1. Add brush state: radius, mode, active stroke, last ROI coordinate.
2. Add a `BrushFootprint` cache keyed by integer radius.
3. Add paint/erase mutation helpers on `SubjectMaskEditSession`.
4. Add camera-view drag handling for active subject-mask edit targets.
5. Add dirty-rectangle preview texture updates.
6. Add stroke-level undo/redo.
7. Keep save routed through `SubjectMaskWritebackClient`.
8. Add focused tests for footprint generation, clipping, drag sampling, and
   undo rectangle restoration.
