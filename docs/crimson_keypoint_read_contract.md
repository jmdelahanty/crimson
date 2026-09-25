# Crimson Keypoint Read Contract

Purpose: define the read-only contract Crimson should use to load keypoint
data from Palette Zarr archives.

Date anchored: 2026-02-10.

Detect analog: `docs/crimson_detect_bbox_read_contract.md`

## Scope

- Read keypoint positions for overlay/inspection.
- Support both raw keypoint runs and refined keypoint sources.
- Do not define write behavior (manual write contract is separate).

## Primary Source Paths

1. Raw keypoint source:
- `keypoints_runs/<run_name>`

2. Refined keypoint source (preferred when available):
- `refined_keypoints_runs/<run_name>`
- Note: refined keypoints are a **flat run** (no subgroups like
  `manual/interpolated/filtered`). Manual edits are applied as
  in-place overwrites on the same run group.

## Quick Reference

| Source | Can show keypoints | Can show quality/reason labels | Can show geometry metrics |
|---|---|---|---|
| `keypoints_runs/<run>` | Yes | No (raw; no quality_labels or reason) | Partial (triangle_area, triangle_angles present) |
| `refined_keypoints_runs/<run>` | Yes | Yes, from `quality_labels` and `reason` | Yes (triangle_area, min_angle, usable_keypoints, etc.) |

## Run Selection Rules

### Raw keypoint default

1. Use `keypoints_runs.attrs["latest"]` when present.
2. If missing, choose newest run name lexicographically.
3. If `keypoints_runs` missing, report "no keypoint runs" (do not crash).

### Refined keypoint preferred source

1. Use `refined_keypoints_runs.attrs["latest"]` when present.
2. If missing, choose newest run name lexicographically.
3. If `refined_keypoints_runs` missing, fall back to raw `keypoints_runs/<run>`.

This matches Palette runtime resolution in:
- `src/fisheye/tune/keypoint_review.py`
- `src/fisheye/utils/set_keypoint_review_status.py`

Note: unlike refined detect runs which have `manual -> interpolated -> filtered`
subgroup resolution, refined keypoints are flat — there is no subgroup chain.

## Required Arrays (for any selected keypoint group)

- `frame_indices` (`int32`, shape `(n_rois,)`)
- `keypoints_norm` (`float64`, shape `(n_rois, n_keypoints, coord_dims)`) **or** `keypoints_img` (`float64`, shape `(n_rois, n_keypoints, coord_dims)`)
- `heading` (`float64`, shape `(n_rois,)`)
- `frame_counts` (`int32`, shape `(n_frames,)`)

Keypoint layout is schema-driven. Readers should treat `keypoint_labels`,
`pose_schema.edges`, heading metadata, and `derived_metrics_schema` as
authoritative when present rather than assuming a fixed 3-keypoint order or
hard-coded derived geometry semantics.

## Optional Arrays

### Common to both raw and refined

- `keypoints_roi` (`float64`, shape `(n_rois, n_keypoints, coord_dims)`) — coordinates in ROI pixels
- `keypoints_img` (`float64`, shape `(n_rois, n_keypoints, coord_dims)`) — coordinates in full-image pixels
- `keypoints_norm` (`float64`, shape `(n_rois, n_keypoints, coord_dims)`) — normalized `[0,1]`
  - current Crimson edit/write paths require `coord_dims >= 2` and operate on XY
- `confidence` (`float64`, shape `(n_rois,)`) — overall keypoint score
- `keypoint_confidences` (`float64`, shape `(n_rois, k)`) — per-keypoint confidences when present
- `detection_source` (`int8`, shape `(n_rois,)`) — `0=real`, `1=interpolated`
- `heading_finite` (`bool`, shape `(n_rois,)`) — strict `isfinite(heading)`
- `heading_usable` (`bool`, shape `(n_rois,)`) — downstream heading gate (`success_gate && detection_source==0 && heading_finite`)
- `triangle_area` (`float64`, shape `(n_rois,)`) — legacy 3-landmark triangle area in pixels²
- `triangle_angles` (`float64`, shape `(n_rois, 3)`) — legacy 3-landmark angles in degrees
- `min_angle` (`float64`, shape `(n_rois,)`) — minimum legacy triangle angle (degrees)
- `n_rois` (`int32`, shape `(n_frames,)`) — legacy alias of `frame_counts`
- `detection_indices` (`int32`, shape `(n_rois,)`, optional) — index into `crop_runs/<run>/roi_images`
- `effective_threshold` (`float64`, shape `(n_rois,)`, optional) — per-ROI threshold used
- `effective_se2_radius` (`float64`, shape `(n_rois,)`, optional) — search radius applied

### Raw only

- `detection_success` (`bool`, shape `(n_rois,)`) — true if keypoints converged
- `triangle_angles_raw` (`float64`, shape `(n_rois, 3)`) — angles in candidate (blob-size) order
- `n_keypoints` (`int32`, shape `(n_frames,)`) — successful keypoints per frame

### Refined only

- `quality_labels` (`int8`, shape `(n_rois,)`) — see Quality Labels below
- `reason_bytes` (`uint8`, shape `(n_rois, width)`) — null-terminated UTF-8 reason labels (preferred Crimson/native path)
- `reason` (`string`, shape `(n_rois,)`) — pipe-delimited tags (secondary/fallback text view)
- `flip_corrected` (`bool`, shape `(n_rois,)`) — true if left/right eyes were swapped
- `usable_keypoints` (`bool`, shape `(n_rois,)`) — confidence + geometry valid
- `confidence_valid` (`bool`, shape `(n_rois,)`) — all per-keypoint confidences >= threshold
- `geometry_valid` (`bool`, shape `(n_rois,)`) — triangle angle/area pass thresholds
- `refined_success` (`bool`, shape `(n_rois,)`) — refinement executed successfully
- `source_success` (`bool`, shape `(n_rois,)`) — source keypoint detection succeeded
- `retune_id` (`int32`, shape `(n_rois,)`) — batch retune parameter set label (`-1` = none)
- `failure_indices` (`int32`, shape `(n_failures,)`) — ROI indices where source keypoints failed

### String-Dtype Compatibility Note

Current Palette writes keypoint `reason` as Zarr v3 string dtype. Some C++
TensorStore builds cannot parse this dtype directly.

Reader requirements in Crimson:
1. Treat `reason` as optional for rendering; do not hard-fail if parsing fails.
2. Fall back to `quality_labels` and other numeric validity arrays
   (`confidence_valid`, `geometry_valid`, `usable_keypoints`) for display/filtering.
3. Emit a single compatibility warning per run (not per frame/ROI) when `reason`
   cannot be loaded.

This keeps keypoint inspection usable even when string arrays are not available
to the native reader path.

## Reason Decode (Refined Keypoints)

Reason labels are encoded in two equivalent forms:
1. `reason_bytes` (preferred): null-terminated UTF-8 rows (`uint8[n, width]`)
2. `reason` (secondary): string array

The decoded labels use **pipe-delimited multi-tag** strings. For example:
- `"flip_corrected|geometry_issue"`
- `"manual_correction"`
- `"detection_failed"`
- `"low_confidence|geometry_issue"`

Reader behavior:
1. Resolve labels in this order:
   - `reason_bytes`
   - `reason`
   - fallback to `detection_source` mapping (`0=clean`, `1=interpolated`) if both reason forms are unavailable
2. Split on `|`, trim whitespace from each tag.
3. Display each tag individually in UI (e.g., as chips/badges).
4. Do not hard-fail on unknown tags; display them as-is.

Known tags:
- `flip_corrected`: left/right eyes were swapped during refinement
- `geometry_issue`: triangle angle or area outside thresholds
- `manual_correction`: keypoints were manually placed by an operator
- `detection_failed`: source keypoint detection did not converge
- `low_confidence`: per-keypoint confidence below threshold
- `confidence_missing`: no confidence data available
- `fish_present_no_keypoints`: fish visible but keypoints not extractable
- `detection_issue`: underlying detection problem (flagged for detect retune)

Compared to detect: detect uses a single `reason` string per row (e.g., `"clean"`,
`"interpolated"`); keypoints use pipe-delimited multi-tag strings because a single
ROI can have multiple simultaneous quality issues.

## Quality Labels (Refined Keypoints)

`quality_labels` semantics:
- `0`: clean
- `4`: source_failed (source keypoint detection did not converge)
- `6`: flip_corrected (left/right eyes were swapped)

These are complementary to `reason` tags:
- `quality_labels` provides a single numeric summary for quick filtering.
- `reason` provides the full set of tags explaining all quality issues.

## Coordinate Spaces

Keypoints exist in three coordinate spaces:

| Array | Space | When to use |
|---|---|---|
| `keypoints_roi` | ROI pixels (crop patch) | Rendering keypoints on cropped ROI images |
| `keypoints_img` | Full-image pixels | Overlay on full-resolution camera frames |
| `keypoints_norm` | Normalized `[0,1]` | Resolution-independent display, coordinate conversion |

Conversion from ROI to full-image pixels:
- `keypoints_img = keypoints_roi + roi_coordinates_full` (from `crop_runs/<run>`)

Conversion from full-image pixels to normalized:
- `keypoints_norm = keypoints_img / [full_width, full_height]`

For overlay in Crimson:
- If working with full-resolution frames, use `keypoints_img`.
- If working with normalized coordinates, use `keypoints_norm` and scale by display dimensions.
- `heading` is in degrees; NaN when heading is unavailable.

## Heading Semantics and NaN Handling

Palette computes heading from `(bladder, eye_left, eye_right)` for each
successful keypoint row in both raw and refined flows. However, heading may
still be non-finite (`NaN`) when geometry is degenerate or invalid (for
example, zero-length head vector or non-finite points).

Important heading fields:
- `heading_finite` is a strict `isfinite(heading)` check.
- `heading_usable` is the run-eligibility gate for downstream heading use:
  - raw runs: `detection_success && detection_source==0 && heading_finite`
  - refined runs: `refined_success && detection_source==0 && heading_finite`

Legacy comparison (`heading_valid` -> new fields):
- Prior `heading_valid` tracked `success_gate && detection_source==0` only.
- `heading_usable` equals that prior gate **plus** finite-heading requirement (`&& heading_finite`).
- `heading_finite` is exposed separately so UI can distinguish “eligible but heading NaN” from other failures.

Reader requirements in Crimson:
1. Treat non-finite heading as "unknown heading" (do not crash).
2. If heading is NaN, skip heading-arrow/orientation rendering for that ROI.
3. Keep keypoint overlay visible even when heading is missing.
4. Optionally, recompute heading from keypoints at read time as a UI-only
   fallback; if recomputation is non-finite, keep heading unknown.

## Type Tolerance

Crimson should accept:
- `keypoints_*` as `float32` or `float64`
- `frame_indices` as any integer dtype coercible to `int64`
- `heading` as `float32` or `float64`
- `quality_labels` as `int8` or any integer dtype coercible to `int8`

Do not hard-fail on dtype width alone; coerce at load.

## Consistency Checks

For selected group:
- `len(frame_indices) == keypoints_roi.shape[0]` (== `n_rois`)
- if `heading` present: `len(heading) == n_rois`
- if `confidence` present: `len(confidence) == n_rois`
- if `keypoint_confidences` present: `keypoint_confidences.shape == (n_rois, 3)`
- if `quality_labels` present: `len(quality_labels) == n_rois`
- if `reason` present: `len(reason) == n_rois`
- if `frame_counts` present: `sum(frame_counts) == n_rois` (advisory; allow mismatch with warning)

## Metadata Hints (Optional)

Helpful attrs to read when present:

Run-level attrs:
- `source_keypoints_run`, `source_crop_run`, `source_detect_run`, `source_refined_run`
- `method`, `parameter_source`, `parameters`
- `keypoint_labels` (default: `["bladder", "eye_left", "eye_right"]`)
- `keypoint_confidence_labels`
- `triangle_angle_order` (canonical keypoint order for angles)
- `summary_statistics` (dict with `refine` and optional `postprocess` snapshots)
- `keypoint_review_status` (review metadata dict on refined runs)
- `keypoint_signature`, `keypoint_review_signature`
- `retune_params` (mapping retune_id → parameter set)

Parent-level attrs:
- `refined_keypoints_runs.attrs["latest"]`
- `refined_keypoints_runs.attrs["keypoint_review_status_latest"]`

Root-level attrs:
- `source_video_metadata` (width, height, fps, frames)

## Expected Failure Modes

- Missing `keypoints_runs` and `refined_keypoints_runs`: return a structured
  "no keypoints available" status.
- Empty arrays (`n_rois=0`): valid, render no keypoints.
- Missing optional arrays (`confidence`, `quality_labels`): still valid.
- NaN keypoint coordinates: valid for ROIs where detection failed; skip overlay
  for those ROIs.
- Missing `keypoints_img` with `keypoints_roi` present: reconstruct from
  `keypoints_roi + roi_coordinates_full` if crop run is available; otherwise
  warn and skip.

## Related Documents

- `docs/crimson_detect_bbox_read_contract.md`
- `docs/crimson_derived_metrics_contract.md`
- `docs/crimson_keypoint_manual_write_contract.md`
- `docs/crimson_keypoint_review_acceptance_contract.md`
- `zarr_structure.md` (lines 153–182: raw keypoints, lines 301–348: refined keypoints)
