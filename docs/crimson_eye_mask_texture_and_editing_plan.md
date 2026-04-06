# Crimson Eye Mask Texture And Editing Plan

Date anchored: 2026-03-09.

## Purpose

Document a practical plan for:

1. replacing the current point-scatter eye-mask overlay with a texture-backed overlay in Crimson, and
2. preparing for future direct mask painting/editing against refined eye-mask runs without breaking the canonical run contract.

For the broader runtime direction that full-frame editing should be first-class
and Crop Preview should become a derived view instead of the primary runtime
surface, see
[crimson_live_crops_and_full_frame_editing_plan.md](./crimson_live_crops_and_full_frame_editing_plan.md).

## Current State

Crimson currently renders eye-mask pixels by:

- loading refined `masks_roi` rows from Palette Zarr,
- expanding nonzero pixels into sparse `pixel_indices`, and
- drawing them as `ImPlot::PlotScatter(...)` markers in the full-frame overlay.

This works, but it is expensive:

- there is CPU cost to decode each chunk into sparse pixel lists,
- there is per-frame CPU cost to rebuild scatter coordinate vectors,
- there is render cost from plotting many markers per eye.

Ellipse overlays are cheap because they only need a small amount of fitted geometry.

## Important Clarification

`masks_roi` is already the canonical bitmap.

The current loader converts that canonical bitmap into a sparse render representation (`pixel_indices`) for display. A future texture path should treat `masks_roi` as the source image and avoid introducing a second canonical mask representation.

## Goals

- Keep ellipse overlays cheap and enabled by default.
- Make raster eye-mask display much smoother when enabled.
- Avoid changing the Palette refined-eye-mask run contract just for Crimson rendering.
- Preserve a clean path toward future manual mask painting/editing.

## Non-Goals

- Do not redesign the refined eye-mask contract to replace `masks_roi`.
- Do not store OpenGL-specific texture artifacts in Zarr.
- Do not make rendering caches part of scientific/provenance data.

## Recommendation

### Decision 1: Keep `masks_roi` Canonical

For both rendering and future editing:

- read from `refined_eye_masks_runs/<run>/masks_roi`
- write back to `masks_roi` when edits are committed
- treat all other geometry as derived from the bitmap

This is the cleanest long-term model because it aligns render, review, and edit behavior with the actual contract output.

### Decision 2: Replace Scatter Rendering With Texture Overlay

For `Show eye mask pixels`, do not draw per-pixel scatter points.

Instead:

- read the dense mask plane for each eye,
- build an alpha/RGBA image for the eye mask,
- upload it to an OpenGL texture,
- draw one textured quad over the ROI bounds.

This should cut draw cost substantially and eliminate the `pixel_indices -> xs/ys -> PlotScatter` path.

### Decision 3: Do Not Precompute Render-Only RGBA Bitmaps In Palette Refinement

Do not add a new refined-run dataset just to store pre-tinted or OpenGL-ready bitmaps.

Reasons:

- it duplicates `masks_roi`
- it bakes UI/render concerns into analysis output
- it complicates future editing by introducing two bitmap representations
- the actual bottleneck is likely Crimson’s current sparse conversion + scatter rendering, not lack of a pre-colored bitmap on disk

If we later need a persistent render cache, it should be optional and clearly derivative, not canonical.

## Texture Overlay Architecture

### Loader Boundary

The eye-mask loader should expose dense per-eye mask planes, not only sparse pixel indices.

Preferred shape in memory:

- `rows x cols` byte plane per eye
- values `0/1` or `0/255`

Possible loader evolution:

- keep existing chunked read path from `masks_roi`
- cache dense bitmap slices for a small number of recently used ROI chunks
- derive textures from dense slices on demand

Avoid:

- rebuilding `pixel_indices` for the main overlay path

### Renderer Boundary

For each visible eye mask:

1. resolve ROI placement from eye-mask lineage
2. obtain the dense bitmap plane
3. create or reuse a GPU texture for that mask plane
4. draw it with alpha blending over the ROI rectangle

Expected rendering model:

- one textured quad per eye
- nearest-neighbor or carefully chosen filtering
- tint left/right eyes in shader or CPU-side RGBA conversion

### Cache Model

Start simple.

Recommended first cache:

- key: `(eye_mask_run_name, roi_index, eye, color_mode)`
- value: uploaded GL texture + source version metadata

Eviction:

- small LRU cache
- clear cache when eye-mask run changes

Later, if needed:

- cache dense chunk planes separately from textures
- atlas multiple eye masks into one texture

## Future Direct Mask Editing

### Is Direct Painting Possible?

Yes.

In fact, the texture approach fits future painting well because the same dense bitmap used for rendering can become the editable working buffer.

The important design rule is:

- edit the canonical `masks_roi` bitmap
- recompute derived outputs after commit

### Proposed Editing Model

At edit time:

1. load one ROI’s left/right mask planes into editable CPU buffers
2. show them as textures in Crimson
3. apply brush/erase/fill operations to those dense planes
4. preview immediately by re-uploading the edited texture
5. on save, write the changed ROI rows back into `masks_roi`

This is a good fit for idempotent writes because:

- the canonical data is already a fixed-shape bitmap array
- equality is simple bytewise comparison of the edited ROI plane against the stored ROI plane
- no-op saves are easy to detect

### What Must Be Recomputed On Save

If Crimson edits a refined eye mask ROI, the following should be treated as derived and refreshed:

- `contours_left` / `contours_right`
- `contour_left_ptr` / `contour_left_len`
- `contour_right_ptr` / `contour_right_len`
- `ellipse_params`
- `ellipse_success`
- `feret_axes_major`
- `feret_axes_minor`
- any metrics derived from the refined mask area or shape
- any reason/status fields that depend on postprocess validity

This strongly suggests a future manual-write path should use a contract-aligned writer, not ad hoc direct bitmap writes from UI code alone.

## Recommendation For Edit Semantics

### Canonical Write Target

If manual mask editing is added, write only to:

- `refined_eye_masks_runs/<run>`

Do not mutate raw eye-mask runs in place.

### Write Policy

Use row-scoped, idempotent writes:

- compare current stored ROI bitmap to edited ROI bitmap
- if identical, write nothing
- if changed, update only that ROI row (or the minimal affected slice)
- recompute derived arrays only for affected ROI rows where possible

### Metadata Policy

Manual edits should eventually record:

- review/update timestamp
- editor identity if available
- manual edit reason/status
- optional edit method metadata (`brush`, `polygon`, `fill`, etc.)

This should likely be formalized in a future eye-mask manual-write contract shared across Palette/Crimson.

## Palette Refinement Implications

Should Palette refinement create a special precomputed bitmap for Crimson?

Recommendation: no.

Palette refinement should continue to produce:

- canonical mask bitmaps
- derived scientific/analysis outputs needed by downstream tools

It should not produce UI-specific render textures.

If Crimson needs faster rendering, Crimson should:

- read the canonical bitmaps directly,
- cache dense planes and/or GPU textures locally,
- avoid sparse scatter rendering.

That separation keeps analysis output stable and keeps UI optimizations local to the UI.

## Phased Plan

### Phase 1: Texture Overlay Refactor

- add dense eye-mask plane access in loader/cache
- stop building `pixel_indices` for the main full-frame overlay path
- render eye masks as textured quads
- keep ellipse overlays unchanged
- preserve the current `Show eye mask pixels` toggle semantics

Definition of done:

- eye-mask pixel overlay no longer uses `PlotScatter`
- playback is visibly smoother with raster overlay enabled

### Phase 2: Crop Preview Alignment

- use the same texture-backed mask path in crop preview
- ensure crop preview and full-frame overlay share the same canonical mask bitmap source

Definition of done:

- eye-mask display is consistent across main view and crop preview

### Phase 3: Edit-Ready Internal Model

- introduce editable dense ROI mask buffers
- add dirty tracking and bytewise no-op detection
- define clear “preview vs committed” state

Definition of done:

- Crimson can hold an edited mask ROI in memory without yet writing it

### Phase 4: Manual Write Contract

- define a refined-eye-mask manual write contract
- specify canonical write targets and required derived recomputes
- implement row-scoped idempotent write path

Definition of done:

- one edited ROI can round-trip safely to Zarr and reload with matching overlays and derived geometry

### Phase 5: Painting UX

- brush/erase tools
- undo/redo
- left/right eye targeting
- optional contour/ellipse snap aids

Definition of done:

- users can directly correct refined masks inside Crimson with predictable writeback behavior

## Open Questions

- Should Crimson cache dense CPU bitmaps, GPU textures, or both?
- Is nearest-neighbor or linear filtering visually better for the main-frame overlay?
- Can derived recompute for manual edits stay ROI-local, or do some metrics require broader run refresh?
- Should eye-mask manual edits live directly in the refined run, or in a manual subgroup with promotion semantics?

## Suggested Next Docs

1. `docs/crimson_eye_mask_manual_write_contract.md`
2. `docs/crimson_eye_mask_texture_overlay_checklist.md`
3. `docs/crimson_eye_mask_editor_ux_plan.md`

## Bottom Line

Use textures for rendering, but keep `masks_roi` as the only canonical bitmap.

That gives Crimson a clean performance improvement now and preserves the right architecture for future direct mask painting and idempotent refined-eye-mask writes.
