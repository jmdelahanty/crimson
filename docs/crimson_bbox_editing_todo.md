# Crimson BBox Editing TODO

Date anchored: 2026-02-09.

## Goal

Allow users to select and drag existing detection bounding boxes in Crimson, then persist approved edits to a manual refined-detect subgroup later.

## Scope

- In scope:
  - UI selection of existing boxes on the current frame
  - drag-to-move editing (position only) as phase 1
  - visual feedback for selected/edited boxes
  - frame-local edit buffering
  - future write-out path to Palette-compatible manual refined-detect data
- Historical out-of-scope (phase 1 only):
  - creating/deleting boxes
  - resize/rotate handles
  - multi-camera synchronized editing
  - bulk edit tools

## Baseline (Before Phase 1)

- Boxes are loaded from active Zarr dataset via `ZarrDetectionLoader`.
- Boxes are rendered as plot lines in `src/red.cpp`.
- No persistent "box object" UI state exists yet (no selection, no dragging).

## Current Status (2026-02-09)

- Phase 1 is implemented in-memory in Crimson UI:
  - click-to-select bbox
  - drag-to-move bbox (position only)
  - visual markers for selected (`[S]`) and modified (`[M]`) boxes
  - frame-local unsaved edit buffering
- Phase 2.5 is implemented in-memory in Crimson UI:
  - `N` toggles draw mode, click-drag creates a new box
  - `Del` removes the selected box
  - added boxes are marked as `[A]`
- Guardrail implemented: source/live detection dataset (`RawDetect`) is read-only in the editor.
- Remaining work is Phase 2 lifecycle UX, Phase 3 Zarr persistence, and review-acceptance metadata integration.

## Contract Incorporation Plan (From Mirrored Docs)

Source contracts:
- `docs/crimson_detect_bbox_read_contract.md`
- `docs/crimson_refined_detect_manual_contract.md`
- `docs/crimson_detect_review_acceptance_contract.md`

Planned incorporation order:

1. Read-path semantics alignment.
- Keep `RawDetect` read-only for editing (already implemented).
- Show clean/interpolated labels only for refined groups:
  - prefer `reason`
  - fallback to `detection_source` (`0=real->clean`, `1=interpolated`)
- Add frame-level quality badge lane from:
  - `detect_runs/<run>/quality_reports/<quality_run>/quality_flags`
  - with `-1/0/2/3/4` meanings per read contract.

2. Edit lifecycle UX completion (Phase 2).
- Add explicit apply/discard controls for current frame and all pending.
- Add unsaved-change guard before dataset/archive switches.
- Keep playback guardrails for drag/create flows.

3. Manual writeback (Phase 3) using manual contract field names.
- Write manual subgroup at:
  - `refined_detect_runs/<latest>/<manual_group>`
- Required arrays:
  - `frame_indices`, `bbox_norm_coords`, `scores`, `class_ids`,
    `frame_counts`, `n_detections`, `frame_mapping`
- Recommended arrays:
  - `detection_source`, `reason`, `retune_id`
- Required attrs/pointers:
  - subgroup attrs (`storage_layout`, `column_fields`, `field_names`,
    `detection_source_type`, `detection_source_path`, `source_refined_run`,
    `source_variant`, `manual_review_timestamp`)
  - run pointer: `manual_review_latest`
  - review status pointers: `detect_review_status`,
    `detect_review_status_latest`

4. Review acceptance integration.
- Add a Crimson-side action path to write acceptance metadata payload
  (state/method/intended_use/reviewer/notes/resolved_group/preference_chain).
- Enforce fail-closed checks from acceptance contract before marking approved.
- Record command+result in Crimson action log for traceability.

## Immediate Next Steps (Execution Order)

1. Phase 2 closure (UI lifecycle and safety).
- Add pending-edit counters and apply/discard actions in one panel path.
- Add unsaved-change confirmation before archive/dataset switches.
- Keep frame-advance behavior deterministic while drag/draw is active.

2. Phase 3 payload builder in memory (no write yet).
- Build a deterministic detection-row assembly pass from current edit buffer:
  - move -> output row with updated bbox
  - add -> output row as manual row
  - delete -> omit row
- Assign labels per contract:
  - unchanged observed rows: `clean`
  - unchanged interpolated rows: `interpolated`
  - operator-created/edited rows: `manual`
- Recompute `frame_counts` / `n_detections` / `frame_mapping` from assembled rows.

3. Phase 3 write path and validation.
- Write manual subgroup arrays + attrs at `refined_detect_runs/<latest>/<manual_group>`.
- Set `manual_review_latest`, `detect_review_status`, and `detect_review_status_latest`.
- Reload and assert manual resolution preference (`manual -> interpolated -> filtered -> raw`).

4. Acceptance action flow.
- Add explicit "Mark Detect Review" action with required fields.
- Fail closed on invalid approval payloads (per acceptance contract).
- Log final payload + outcome in Crimson action history.

## Phased Plan

### Phase 1: Selection + Drag (In-Memory Only)

1. Add editor state in `src/red.cpp`.
- Track selected detection per frame/view.
- Track drag-active flag and mouse-to-box offset.
- Track temporary edited boxes keyed by frame.

2. Add hit-testing on plotted boxes.
- Use plot mouse coordinates to find nearest/containing box.
- Prefer topmost/smallest box when overlaps occur.

3. Add drag behavior.
- Left-click selects a box.
- Click+drag moves selected box.
- Clamp moved box to image bounds.
- Keep width/height fixed for phase 1.

4. Add visual feedback.
- Selected box uses distinct style/color.
- Edited-but-unsaved boxes are marked clearly.

5. Add basic controls.
- `Esc`: clear selection.
- `Shift+R`: reset current-frame edits to loaded values.

### Phase 2: Edit Lifecycle UX

1. Add edit session indicators.
- Show "editing" state in side panel.
- Show per-frame edited count.

2. Add apply/discard controls.
- Apply frame edits to session buffer.
- Discard all pending edits.

3. Playback guardrails.
- Pause or freeze frame while dragging to prevent desync.

### Phase 2.5: Add/Delete Completeness (Implemented Before Writeback)

Rationale for doing this before Phase 3:

1. Stabilize the save model first.
- Writeback should handle all edit types together: move, add, and ideally delete.

2. Avoid reworking persistence twice.
- If writeback ships before create/delete tools, row assembly and canonical
  `reason_bytes` assignment likely need refactors.

3. Fit manual semantics cleanly.
- New boxes can be labeled `"manual"` in `reason_bytes` under the current contract.

Recommended implementation slice:

1. Add "Draw New Box" mode (`N`) with click-drag placement and `Esc` cancel.
2. Add "Delete selected box" (`Del`) so manual curation is complete.
3. Mark added boxes visually (for example `[A]`) in the same in-memory edit buffer.
4. Keep writeback deferred until move/add/delete behavior is in place.

### Phase 3: Persist to Zarr Manual Refined-Detect

1. Build save payload from edited detections.
- Recompute `frame_indices`, `bbox_norm_coords`, `frame_counts`.
- Handle deleted detections by omitting removed rows from rebuilt arrays.
- Preserve/propagate `scores`, `class_ids`, and `detection_source` where applicable.
- Populate `reason` labels per manual contract semantics:
  - `clean` for observed/kept detections
  - `interpolated` for interpolated detections
  - `manual` for explicit manual rows (for example newly drawn/edited rows)

2. Write manual subgroup under latest refined run.
- Target `refined_detect_runs/<latest>/<manual_group>` (default `manual`).
- Include required arrays/attrs from manual contract.
- Set manual subgroup attrs consistently, including:
  - `detection_source_type = "manual"`
  - `detection_source_path = "refined_detect_runs/<latest>/<manual_group>"`
  - `source_refined_run = <latest>`
  - `source_variant = "interpolated"` or `"filtered"` (base used)

3. Update refined pointers/status.
- Set `manual_review_latest`.
- Set `detect_review_status`.
- Set parent `detect_review_status_latest`.

4. Reload and verify.
- Reload active dataset after save.
- Confirm manual group becomes active source when present.

## Acceptance Criteria

### Phase 1 Done When

- User can click a drawn bbox to select it.
- User can drag selected bbox and see immediate visual update.
- Moving one frame away and back preserves in-memory edits (until discard/reload).
- No crashes/regressions when frame has zero detections.

### Persistence Done When

- Saved edits appear in `refined_detect_runs/<latest>/manual` (or configured group).
- Loader resolves manual subgroup as primary when available.
- Palette helper resolution semantics remain compatible.

## Risks / Watchouts

- Coordinate transform mismatch (plot Y inversion vs image-space Y).
- Selection ambiguity when boxes overlap heavily.
- Editing while playback advances frames can cause accidental mis-edits.
- Need deterministic mapping from displayed box -> detection row for saving.

## Open Questions

1. For edited rows that were originally interpolated, should output `reason` become `manual` (current plan) or preserve `interpolated` with separate edit provenance?
2. Should Crimson always write to subgroup name `manual`, or support timestamped/manual-named groups and update `manual_review_latest` accordingly?
3. Should detect-review approval in Crimson require `reviewer` for `approved` by default (strict policy), or be optional in UI?
4. If no refined run exists, should Crimson block writeback (current contract-safe default) or add a future flow to initialize a refined run skeleton?
