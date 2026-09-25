# Palette Refined-Detect Row Identity Handoff

<!-- handoff-meta
status: active
last_updated: 2026-04-24
-->

## Purpose

This is a handoff for the next Crimson agent working on bounding-box edits against
Palette refined detections.

Crimson UI Monolith already reads and writes the current sparse refined-detect
surface:

- `refined_detect_runs/<run>/instances/`

The remaining high-priority gap is identity preservation. The current write path
can regenerate `refined_row_ids`, which makes add/delete/edit sessions look like a
full row-identity churn to downstream consumers. The next agent should preserve
stable row identity through load, edit, preview, and write.

## Design Rules

- `refined_row_ids` are logical artifact row identities, not biological identity,
  track identity, arena identity, or physical array positions.
- Physical rows may be frame-sorted for fast rendering. Consumers should still
  treat `refined_row_ids` as the stable identity for a refined detection row.
- `source_detect_row_index` links a refined row back to the raw detect row when
  one exists. Manual additions should use `-1`.
- Deleted rows should be omitted from the new `instances` arrays, but their old
  `refined_row_ids` must not be reused.
- Edited rows should keep their existing `refined_row_ids`.
- Newly added rows should receive new stable IDs after existing IDs are preserved.
- Do not resurrect the legacy `manual_review_latest` subgroup write path unless a
  compatibility task explicitly requires it.

## Current Crimson Risk

Playback and rendering already have the right shape: flat sparse arrays plus
`frame_offsets` allow fast per-frame slices without searching appended data.

The risk is that identity is not carried through the editor:

- `InterpolationRunData`, `ZarrDetectionData`, and `FrameDetections` carry boxes,
  scores, classes, source labels, and frame offsets, but not stable row identity.
- `loadFlattenedRun` reads and sorts detection rows by frame, but currently does
  not preserve optional identity arrays alongside those rows.
- `ZarrBBoxEditState` is frame-local and positional, so a row is effectively
  identified as "box index in this frame" instead of by `refined_row_id`.
- `manual_detect_payload_preview` rebuilds full row arrays from frame-local state
  without carrying existing `refined_row_ids` or `source_detect_row_index`.
- `writeManualRefinedDetections` writes `instances/`, but currently allocates
  row IDs from `(frame_id << 32) | ordinal` and initializes
  `source_detect_row_index` as `-1`.

That means a small edit, add, or delete can break downstream row matching even
when most boxes are unchanged.

## Implementation Plan

1. Carry identity in the read model.
- Add `refined_row_ids` and `source_detect_row_indices` to `InterpolationRunData`.
- Add matching fields to `ZarrDetectionData`.
- Add matching fields to `FrameDetections`.
- Keep these arrays aligned with `frame_indices`, boxes, scores, classes,
  `source_kind_codes`, and reason strings.

2. Preserve identity in `loadFlattenedRun`.
- Read optional `instances/refined_row_ids`.
- Read optional `instances/source_detect_row_index`.
- Validate that present identity arrays have one value per detection row.
- When sorting into frame-major order, sort identity arrays in the same
  permutation as boxes and labels.
- If identity arrays are missing, keep rendering functional but mark identity as
  unavailable instead of inventing stable IDs silently.

3. Carry identity into edit state.
- When opening a frame override, copy each loaded box's `refined_row_id` and
  `source_detect_row_index`.
- For moved or resized boxes, preserve the existing IDs.
- For deleted boxes, omit the row from the outgoing payload.
- For added boxes, use a temporary sentinel such as `refined_row_id = -1` and
  `source_detect_row_index = -1`.

4. Preserve identity in payload preview.
- Extend the manual detect payload preview model with `refined_row_ids` and
  `source_detect_row_index`.
- Rebuild frame-major sparse arrays from edit state while preserving all
  nonnegative existing `refined_row_ids`.
- Keep `frame_offsets` and `frame_counts` as the render/read acceleration path.

5. Allocate only genuinely new row IDs in the writer.
- Before rewriting `instances/`, inspect existing `instances/refined_row_ids` and
  compute the max existing nonnegative ID.
- Reuse all provided nonnegative row IDs.
- Allocate new IDs only for rows whose preview identity is `-1`.
- Do not reuse IDs from deleted rows.
- Continue writing `row_sort_order = ["frame_indices", "refined_row_ids"]`.
- Write `source_detect_row_index` from the preview payload, not as all `-1`.

6. Decide how to handle raw-source linkage metadata.
- Preferred behavior: update `source_detections` mappings for raw-backed rows
  whose accepted/refined status changes.
- Minimum safe behavior: add validation or a clear status marker indicating that
  `source_detections` linkage may be stale when Crimson rewrites `instances/`.
- Do not claim coherent source linkage if old `resolved_refined_row_id` values can
  point at regenerated or missing rows.

## Acceptance Tests

The next agent should add or update tests for these cases:

- Move-only edit: all existing `refined_row_ids` remain identical after save.
- Add-only edit: all existing IDs remain identical and exactly one new ID appears.
- Delete-only edit: remaining IDs are preserved and the deleted ID is not reused.
- Mixed add/delete/edit: unchanged and edited rows preserve IDs; new rows get new
  IDs; omitted rows disappear.
- Reordered physical rows: frame-sorted output still preserves logical IDs.
- Raw-backed row: `source_detect_row_index` is preserved unless the row is a
  manual addition.

## Out Of Scope

- Biological identity, `track_id`, `subject_id`, or `arena_id` semantics.
- Replacing sparse arrays with a dense `(frame, slot)` representation.
- Recomputing crop, keypoint, or downstream model outputs in Crimson.
- Full Palette staleness propagation. Crimson should preserve row identity so
  Palette can make precise downstream invalidation decisions.

## Files To Inspect First

- `src/zarr_loader.h`
- `src/zarr_loader.cpp`
- `src/zarr_loader_detections.cpp`
- `src/zarr_bbox_edit.cpp`
- `src/manual_detect_payload_preview.cpp`
- `src/zarr_loader_write.cpp`
- `docs/crimson_bbox_editing_todo.md`

## Agent Prompt

Implement stable refined-detect row identity preservation for Crimson bbox edits.

Focus on `refined_detect_runs/<run>/instances/`. Preserve existing
`refined_row_ids` and `source_detect_row_index` through read, edit, preview, and
write. Allocate new row IDs only for newly added boxes. Do not restore the legacy
`manual_review_latest` subgroup workflow.

Give findings first if you discover mismatches between current docs, current code,
and Palette's sparse `instances` contract.
