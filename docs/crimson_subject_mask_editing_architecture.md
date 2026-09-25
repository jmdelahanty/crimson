# Crimson Refined Subject-Mask Editing Architecture

## Scope

Crimson owns the interactive editor:

- target selection
- paint/erase tools
- preview masks
- undo/redo state
- component visibility
- display order

Palette owns persisted refined subject-mask mutation:

- writing the final binary mask payload into the selected component row
- resolving the component channel from `mask_labels`
- checking `available_channels`
- refreshing metrics, reason state, contours, and row revisions
- validating the row-local result before reporting success

Crimson should not depend on Paintera code, and production Crimson should not
depend on a fixed local Palette checkout path.

For the planned brush mechanics, preview-mask mutation, and stroke-level undo
model, see
[crimson_subject_mask_painting_plan.md](./crimson_subject_mask_painting_plan.md).

## Save Boundary

The preferred save boundary is a Palette-owned command or service. The current
command contract is:

```bash
palette-write-refined-subject-mask-edit \
  --zarr-path <analysis.zarr> \
  --refined-run <run> \
  --component-name <subject_body|swim_bladder|eye_left|eye_right> \
  --roi-index <row> \
  --mask-path <binary-mask.npy|png|raw> \
  --mask-shape <HxW> \
  --reason crimson_refined_subject_mask_edit \
  --validate
```

The command returns JSON with fields such as `ok`, `status`,
`row_revision_before`, `row_revision_after`, `edit_applied`, `mask_changed`,
`contour_points`, and `updated_at_utc`.

The older two-step bridge, where Crimson writes pixels and Palette separately
syncs metadata, is only a development fallback. It is not the production
contract because a failed sync can leave pixels and derived metadata out of
step.

## Crimson Interfaces

The first implementation slice adds these boundaries:

- `SubjectMaskEditSession`
  - active archive path
  - active refined run
  - ROI row index
  - active component name and resolved channel
  - original dense binary mask
  - preview dense binary mask
  - dirty state
- `SubjectMaskWritebackClient`
  - backend-independent save request and response interface
- `PreviewOnlyWritebackClient`
  - default backend
  - refuses saves with a clear preview-only result
- `PaletteCommandWritebackClient`
  - configured through `PALETTE_WRITEBACK_CMD`
  - writes a temporary raw binary mask payload
  - calls the configured command without shell expansion
  - parses JSON stdout into a backend-independent result
- `PaletteServiceWritebackClient`
  - placeholder for a future endpoint

The loader now exposes a dense row read for refined subject-mask components:

```cpp
readRefinedSubjectMaskComponentRow(roi_index, component_name, row, error)
```

This helper only succeeds when the active mask source is
`refined_subject_masks_runs/<run>/masks_roi`; legacy eye masks are not an edit
target for this path.

## Current UI State

Frame Inspect labels the tab as Subject Masks when Crimson is reading unified
`refined_subject_masks_runs` data. That panel has a current-frame target table
with detection index, ROI row, available components, and preview dirty state.
Selecting a row/component loads the dense mask row into
`SubjectMaskEditSession`, and the camera view highlights the selected ROI
component. When the Subject Masks tab is active, clicking a visible mask in the
camera canvas also selects the topmost component under the cursor using the
same priority as the draw order: eyes, swim bladder, then body. Legacy fallback
data remains labeled as Eye Masks.

The current preview slice also exposes preview tools for the selected target:
brush, lasso, and polygon. Brush mode shows the current circular footprint over
the active ROI and paints or erases circular stamps. Lasso records a
click-drag path and fills it on mouse release. Polygon mode records clicked
vertices and fills on double-click or the explicit Apply Polygon button.
All tools mutate only `SubjectMaskEditSession::previewMask()`, operate in
ROI-pixel coordinates, draw dotted shape drafts before applying fills, and render
the dirty preview mask above the persisted mask layer. Input is active only from
the Subject Masks tab while playback is paused.

Save is deliberately disabled in the UI because the default backend is
`PreviewOnly`.

## Remaining Work

1. Add undo/redo state around preview mask mutations.
2. Add preview-specific contour/diff rendering instead of only the full dirty
   preview fill.
3. Add an explicit backend setting for `PreviewOnly` vs configured Palette
   command/service.
4. Wire save through `SubjectMaskWritebackClient`, then refresh the touched
   row/component after successful saves.
5. Keep failed saves dirty and surface the backend error without marking the
   edit accepted.
