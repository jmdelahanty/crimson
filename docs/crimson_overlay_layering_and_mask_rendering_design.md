# Crimson Overlay Layering And Mask Rendering Design

Date anchored: 2026-04-29.

## Purpose

Define the intended draw order for full-frame Crimson overlays and document the
performance implications of rendering multiple refined mask components, axes,
headings, keypoints, and edit affordances on the same frame.

This is a display design only. It does not change the Palette Zarr contract or
make any render cache canonical.

For a more introductory explanation of cache misses, chunked Zarr access
patterns, working sets, and prefetch tradeoffs, see
[crimson_zarr_cache_miss_design_notes.md](./crimson_zarr_cache_miss_design_notes.md).

## Problem

Refined subject masks can now expose several semantic components for the same
ROI:

- `subject_body`
- `eye_left`
- `eye_right`
- `swim_bladder`

When all components are enabled, the draw order matters. The body mask should
not hide the eye or swim-bladder masks, and mask fills should not obscure
keypoints or editing handles. The order should also be deterministic and should
not depend on `mask_labels` array order in the Zarr run.

## Layer Model

Crimson should treat the main camera view as a fixed stack of semantic layers.
Within a layer, stable ordering by detection index is fine. Across layers, the
order should be explicit.

Recommended full-frame stack, bottom to top:

1. Base video frame
2. Detection boxes and passive context geometry, when enabled
3. Refined subject mask fills
4. Refined subject mask contours
5. Eye axes, angle labels, and orientation beams
6. Heading arrows
7. Keypoint skeleton edges and markers
8. Active editing affordances, selected points, drag handles, and modal tool UI
9. Diagnostic text overlays

The key rule is that mask fills are background context. Keypoints and active
editing handles are interaction targets, so they must remain visually dominant.

## Mask Component Layering

Mask components need their own semantic rank independent of storage order.

Recommended component fill order:

1. `subject_body`
2. `swim_bladder`
3. `eye_left`
4. `eye_right`
5. unknown/future components, after known components unless explicitly ranked

Rationale:

- `subject_body` is the largest context layer and should use the lowest alpha.
- `swim_bladder` and eye masks are internal structures and should sit on top of
  the body fill.
- Left/right eye ordering rarely matters because they do not usually overlap,
  but keeping a fixed order avoids flicker or label-order surprises.

Recommended component contour order:

- draw contours after fills
- draw same-component contours above their fill
- draw body contour after internal fills if the goal is a clear outer outline
- draw internal contours after body contour if the goal is maximum component
  visibility

For the near term, body and swim bladder can remain fill-only because the
feeding canary has no precomputed contours for those components.

## Current Crimson State

As of this design note:

- unified refined subject masks are preferred over legacy refined eye masks
- the full-frame mask overlay can render `subject_body`, `swim_bladder`,
  `eye_left`, and `eye_right` as texture-backed fills
- component contours are read as optional derived caches from
  `components/<component>/contours/{ptr,len,points_xy}` when available
- eye axes and angle labels are drawn from refined subject eye geometry and eye
  angle runs when available
- keypoint markers are drawn after the mask overlay, so keypoints already appear
  above mask fills

The remaining cleanup is to make this layer order explicit in code instead of
being an accidental result of call order and component iteration.

## Performance Considerations

Layered rendering means draw submissions must happen in order, but that does
not by itself make the overlay slow. GPUs are designed to render ordered
transparent quads and line/point primitives. The likely bottlenecks are
elsewhere.

Primary performance costs:

- Zarr chunk reads when a mask chunk is not already cached
- CPU conversion from dense mask planes into the current display cache
- GPU texture creation and upload on cache miss
- scatter fallback, if texture creation fails
- excessive ImPlot calls if every tiny overlay primitive is submitted
  separately
- alpha blending over large full-frame regions, especially if many detections
  overlap

Costs that should usually be small:

- drawing one textured quad per visible mask component
- drawing a few axis line segments per eye
- drawing keypoint markers for one or a small number of fish
- deterministic semantic sorting of four known mask components

For the common one-fish full-frame view, four mask component quads plus axes,
headings, and keypoints should be acceptable if texture cache hits are high.

## Important Distinction: Order vs Cache Work

Draw order controls visual stacking. Cache work controls latency.

Even with perfect draw ordering, the UI can hitch if a frame change forces:

1. a Zarr chunk read,
2. dense mask scan,
3. CPU display-cache generation,
4. OpenGL texture upload,
5. then draw submission.

The design goal should be:

- make ordering deterministic every frame
- keep cache generation incremental and bounded
- reuse GPU textures across frames whenever the same ROI/component/source is
  visible

## Recommended Implementation Shape

Introduce explicit overlay layer ranks:

```cpp
enum class CameraOverlayLayer {
    BaseVideo,
    PassiveDetections,
    MaskFill,
    MaskContour,
    EyeGeometry,
    Heading,
    Keypoints,
    ActiveEditing,
    Diagnostics,
};
```

For mask components, introduce semantic component ranks:

```cpp
int maskComponentRank(std::string_view label) {
    if (label == "subject_body") return 10;
    if (label == "swim_bladder") return 20;
    if (label == "eye_left") return 30;
    if (label == "eye_right") return 31;
    return 100;
}
```

The renderer can either:

- draw each overlay family in fixed function-call order, or
- build a small list of render commands with layer ranks and submit them sorted.

The fixed-call-order approach is simpler and probably sufficient now. A render
command list becomes useful if more modules start contributing overlays that
need to interleave.

## Mask Rendering Policy

Filled masks:

- draw with texture-backed quads
- avoid scatter except as a debug or fallback path
- use low alpha for `subject_body`
- use stronger alpha for internal components
- keep keypoints above all filled masks

Contours:

- use contract-compatible contour arrays when present
- add bitmap-derived contour fallback later for body/swim bladder and edit
  previews
- draw contours as line geometry, not as dense point clouds

Axes and angle labels:

- draw after mask fills and contours
- keep line width modest so axes do not overpower keypoints
- consider making angle labels optional if they conflict with editing

## Performance Guardrails

Near-term guardrails:

- keep texture cache keys scoped by source path, ROI, component/channel, shape,
  and source version/run identity
- avoid rebuilding textures when only draw order or visibility toggles change
- cap texture cache size and evict least-recently-used entries
- draw body fill first and low alpha to minimize perceived occlusion
- keep scatter fallback visible but not the normal path

Future guardrails:

- replace `pixel_indices` with row spans/RLE as the primary CPU display cache
- prefetch adjacent mask chunks when scrubbing sequentially
- add lightweight timing counters for:
  - mask chunk read time
  - CPU decode/cache-build time
  - texture upload count per frame
  - overlay draw time
- degrade gracefully by disabling fills before disabling keypoints/editing
  affordances if frame time is too high

## Open Questions

- Should contours be a global display mode (`fill`, `contour`, `both`) or a
  per-component mode?
- Should eye axes and angle labels be tied to eye-mask visibility, or remain
  independently toggleable?
- Should body contours be drawn above or below internal component contours once
  body contours are available?
- What cache size is appropriate for high-density recordings with many fish per
  frame?

## Proposed Next Step

Make the current implementation match the design explicitly:

1. enforce fixed component rank for refined subject mask fills
2. keep mask fill rendering before headings and keypoints
3. move heading arrows above mask fills but below keypoint markers if needed
4. leave keypoints and active editing affordances as top interaction layers
5. add small debug counters if visual performance becomes questionable
