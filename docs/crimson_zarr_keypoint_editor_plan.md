# Crimson Zarr Keypoint Editor Plan

Date anchored: 2026-04-04.

## Purpose

Document the plan for turning Crimson's current Zarr keypoint read/overlay path
into a real refined-keypoint review and edit workflow that is compatible with
Palette's current contracts and write behavior.

This plan is intentionally separate from the legacy CSV-based `Labeling Tool`.

For the UI/workflow layer that should replace the old `Labeling Tool` feel
while staying Zarr-native, see
[crimson_zarr_keypoint_review_window_plan.md](./crimson_zarr_keypoint_review_window_plan.md).

For the runtime direction that full-frame editing should be primary and crop
views should be derived from the full frame rather than persisted crop images,
see
[crimson_live_crops_and_full_frame_editing_plan.md](./crimson_live_crops_and_full_frame_editing_plan.md).

## Core Conclusion

Crimson currently has two different keypoint workflows:

1. Legacy manual labeling:
   - stores labels under `labeled_data/`
   - uses timestamped CSV snapshots
   - operates on `keypoints_map`
   - is not the canonical Zarr refinement workflow

2. Zarr-native refined keypoint viewing:
   - reads `refined_keypoints_runs/<run>` when available
   - falls back to `keypoints_runs/<run>`
   - exposes refined quality and review metadata in the UI
   - does not yet provide a contract-aligned in-app keypoint writer

If the goal is to streamline the Zarr workflow, Crimson should treat the CSV
`Labeling Tool` as legacy and build a dedicated refined-keypoint editor on top
of the existing Zarr loader/UI path.

## Current State

### Legacy Labeling Tool

The current `Labeling Tool` window:

- saves via `save_keypoints(...)`
- loads via `find_most_recent_labels(...)` and `load_keypoints(...)`
- writes timestamped CSV snapshots under `labeled_data/`
- triangulates current in-memory labels with `reprojection(...)`

This is useful for older manual workflows, but it is not Zarr-native and should
not be the foundation for refined keypoint review/editing.

### Existing Zarr Keypoint Read Path

Crimson already has a meaningful Zarr refined-keypoint read path:

- `ZarrDetectionLoader::loadKeypointHeadingData(...)` prefers
  `refined_keypoints_runs/<run>` and falls back to `keypoints_runs/<run>`.
- The loader already reads refined metadata such as:
  - `quality_labels`
  - `reason`
  - `usable_keypoints`
  - `flip_corrected`
  - `refined_success`
  - `keypoint_review_status`
- The main scene and Frame Debug window already show:
  - keypoint overlays
  - refined-vs-raw run status
  - quality/reason summary

This means the read side is already pointed at the right data model. The write
side is the missing piece.

## Source Of Truth

The write/edit behavior should be anchored to:

- `~/gitrepos/contracts/palette-crimson/keypoint_manual_write.md`
- `~/gitrepos/contracts/palette-crimson/keypoint_review_acceptance.md`
- `~/gitrepos/contracts/palette-crimson/keypoint_read.md`
- `~/gitrepos/palette/src/fisheye/tune/keypoint_failure_review.py`
- `~/gitrepos/palette/src/fisheye/tune/keypoint_review.py`
- `~/gitrepos/palette/src/fisheye/utils/set_keypoint_review_status.py`
- `~/gitrepos/palette/src/fisheye/shared/detect_reason_codec.py`

These are the current compatibility targets. Crimson should mirror them rather
than inventing a parallel schema.

## Contract Findings

### Refined Keypoints Are Flat Runs

Unlike refined detect data, refined keypoints are not stored in
`manual/interpolated/filtered` subgroups.

Manual keypoint edits must mutate:

- `refined_keypoints_runs/<run>`

in place.

Crimson must not:

- recreate the refined run group
- write edits into `keypoints_runs/<run>`
- invent a subgroup chain for keypoints

### Manual Keypoint Saves Are Row-Scoped

The canonical write target is an ROI row, not a frame snapshot.

For a manual correction to ROI `roi_idx`, Crimson must write:

- `keypoints_roi[roi_idx]`
- `keypoints_img[roi_idx]`
- `keypoints_norm[roi_idx]`
- `heading[roi_idx]`

Then it must recompute the derived row fields:

- `triangle_area`
- `min_angle`
- `triangle_angles`
- `confidence`
- `keypoint_confidences`
- `refined_success`
- `flip_corrected`
- `quality_labels`
- `confidence_valid`
- `geometry_valid`
- `usable_keypoints`
- `heading_finite`
- `heading_usable`

### Reason Columns Must Stay Synchronized

Reason tags are not "nice to have" metadata. They are part of the current
downstream contract.

Crimson must keep:

- `reason_bytes`
- `reason`

synchronized using Palette-compatible encoding and update rules.

For manual correction, Crimson must append `manual_correction` and append
`geometry_issue` only when the recomputed geometry is invalid.

### Manual Save And Review Acceptance Are Separate Actions

There are two distinct write operations:

1. Manual edit save:
   - mutates refined keypoint arrays and derived values
   - refreshes postprocess summary
   - may mark downstream eye-mask runs stale

2. Review acceptance:
   - metadata-only
   - writes `keypoint_review_status`
   - writes or reuses `keypoint_signature`
   - copies to `keypoint_review_signature`
   - updates `refined_keypoints_runs.attrs["keypoint_review_status_latest"]`

Crimson should keep these as separate explicit actions in the UI.

### No-Op Behavior Matters

Crimson should follow value-based idempotency:

- compare old vs new ROI values with NaN-aware equality
- only write fields that actually changed
- do not mark downstream eye-mask runs stale if the effective ROI data did not
  change

This is important for compatibility and to avoid unnecessary churn.

## Architectural Decisions

### Decision 1: Do Not Extend The Legacy Labeling Tool

Do not retrofit the CSV `Labeling Tool` into the refined-keypoint editor.

Reasons:

- it is built around frame snapshots and `keypoints_map`
- it persists to `labeled_data/` rather than refined Zarr runs
- it has a different mental model than Palette's row-scoped review/edit flow

The Zarr-native workflow should be a separate first-class path.

### Decision 2: Keep `FrameDetections` Detection-Centric

`FrameDetections` should remain mostly about detection identity and box-level
data.

It may carry:

- boxes
- scores
- class IDs
- detection source/reason
- stable row identity needed to join downstream data

It should not keep growing into a catch-all transport object for:

- keypoint edit state
- eye-mask payloads
- heading render models
- other stage-specific overlay products

Current `FrameDetections` is already too wide. The keypoint editor should not
make that worse.

### Decision 3: Add Explicit ROI Identity For Keypoint Writes

Crimson needs a stable mapping from the currently selected on-screen detection
to the refined keypoint row to write back.

Today the loader already exposes eye-mask ROI indices, but there is no explicit
keypoint ROI identity surfaced as a first-class field on detection results.

Before building the editor, add explicit row identity such as:

- `keypoint_roi_index`

or a more general detection-to-ROI join key.

This is an identity field, not extra overlay payload.

### Decision 4: Split Downstream Overlay Models From Detection Rows

Use a composition model instead of one mega struct:

- `FrameDetections`
- `FrameKeypointOverlayModel`
- `FrameHeadingOverlayModel`
- `FrameEyeMaskOverlayModel`

If needed, a higher-level feature-specific composition object can join them for
rendering, but the base detection transport should stay narrow.

## Proposed Crimson Architecture

### 1. Repository / Writer Layer

Add a dedicated refined-keypoint repository/writer facade.

Recommended surface:

- `loadFrameKeypointReviewData(frame_id, dataset)`
- `writeManualCorrection(roi_idx, keypoints_roi, options)`
- `markFishPresentNoKeypoints(roi_idx, options)`
- `markDetectionIssue(roi_idx, options)`
- `refreshPostprocessSummary()`
- `writeReviewStatus(run_name, payload)`

Responsibilities:

- resolve `refined_keypoints_runs/<latest>`
- read/write only the refined run
- recompute row-level geometry and quality fields
- keep reason columns synchronized
- mark downstream eye-mask runs stale only on effective change
- update postprocess summary
- write review acceptance metadata

This should be separate from `ZarrDetectionLoader`'s current broad read surface.

### 2. Feature State

Add feature-local state for refined keypoint review/editing:

- selected detection / selected ROI
- active keypoint handle
- transient drag/edit state
- dirty flag for current ROI
- save/apply status
- review status controls

This state should not live in the legacy `Labeling Tool`.

### 3. UI Landing Zone

Build the refined-keypoint editor on top of the existing Zarr review UI:

- Frame Debug: selection, review metadata, save/status actions
- main camera overlay: show current refined keypoints
- Crop Preview: editable ROI-local keypoint interaction surface

This keeps the workflow aligned with the current Zarr inspection path instead
of introducing a second disconnected editor.

### 4. Review And Save Separation

UI should expose two explicit actions:

- `Save Keypoint Edit`
- `Set Review Status`

`Save Keypoint Edit` mutates arrays.

`Set Review Status` only writes review metadata.

Do not implicitly mark a run approved as a side effect of a manual save.

## Recommended UX Flow

1. Load Zarr archive.
2. Frame Debug identifies refined keypoint run and review status.
3. User selects a detection in the main view or crop preview.
4. Crimson resolves explicit `keypoint_roi_index`.
5. Crop Preview shows the ROI-local refined keypoints.
6. User drags points or chooses:
   - manual correction
   - fish present, no keypoints
   - detection issue
7. Crimson writes the row-scoped refined update.
8. Crimson refreshes postprocess summary.
9. User explicitly sets review status when ready.

## First Implementation Slices

### Phase 1: Data Seam Cleanup

- Add explicit keypoint ROI identity to detection-aligned results.
- Keep `FrameDetections` detection-centric; do not embed more edit payload into
  it than necessary.
- Add tests proving selected detection -> `roi_idx` mapping is stable.

### Phase 2: Writer Facade

- Add a refined-keypoint writer facade with no GUI dependency.
- Implement:
  - manual correction write
  - fish-present/no-keypoints write
  - detection-issue write
- Mirror Palette row update order and NaN-aware no-op logic.
- Add fixture-backed tests against miniature Zarr archives.

### Phase 3: Summary + Review Status

- Implement postprocess summary refresh logic compatible with Palette's
  `keypoint_review.py`.
- Implement review-status write compatible with
  `set_keypoint_review_status.py`.
- Add tests for:
  - `keypoint_review_status`
  - `keypoint_signature`
  - `keypoint_review_signature`
  - parent `keypoint_review_status_latest`

### Phase 4: UI Editor

- Add a dedicated refined-keypoint review/editor panel/workflow.
- Reuse Crop Preview for ROI-local editing.
- Add explicit actions:
  - save correction
  - mark no keypoints
  - mark detection issue
  - approve / needs review / pending / reject

### Phase 5: Legacy Workflow De-Emphasis

- Keep the old CSV `Labeling Tool` for backward compatibility if needed.
- Make the Zarr-native refined-keypoint path the primary operator workflow.
- Avoid routing new keypoint-review features through `labeled_data/`.

## Validation Checklist

After Crimson writes a manual correction:

1. Only `refined_keypoints_runs/<run>` is mutated.
2. Raw `keypoints_runs/<run>` is untouched.
3. ROI row values match recomputed geometry/quality fields.
4. `reason_bytes` and `reason` remain synchronized.
5. `summary_statistics.postprocess` matches current run contents.
6. Eye-mask stale markers are only written when effective ROI values changed.

After Crimson writes review acceptance:

1. `keypoint_review_status` exists on the refined run.
2. `keypoint_signature` exists.
3. `keypoint_review_signature` exists and matches the signature.
4. Parent `keypoint_review_status_latest` points to the reviewed run.

## Risks

- If Crimson writes only some arrays and skips derived fields, Palette will read
  inconsistent refined rows.
- If Crimson rewrites `reason` but not `reason_bytes`, downstream readers may
  diverge.
- If Crimson couples the editor to the legacy `Labeling Tool`, the app will end
  up with two incompatible manual-keypoint mental models.
- If `FrameDetections` keeps absorbing downstream stage payloads, the current
  monolith problem will just move into a new transport type.

## Recommendation

Do not spend the next keypoint-focused effort on the CSV `Labeling Tool`.

The highest-value path is:

1. add explicit refined keypoint ROI identity,
2. build a contract-aligned refined-keypoint writer,
3. attach it to the existing Zarr review/crop-preview UI,
4. keep review acceptance as a separate explicit action.
