# Crimson Keypoint Manual Write Contract

Purpose: define exactly what Crimson must write so Palette recognizes manual
keypoint edits as the active refined source.

Date anchored: 2026-02-10.

Detect analog: `docs/crimson_refined_detect_manual_contract.md`

## Scope

This contract is for updating:
- `refined_keypoints_runs/<run>` (in-place overwrite on the flat run)
- review pointers/status on the refined run

It is intentionally aligned with current Palette behavior in
`fisheye.tune.keypoint_failure_review` and `fisheye.tune.keypoint_review`.

For acceptance/status policy after inspection, see:
- `docs/crimson_keypoint_review_acceptance_contract.md`

Key structural difference from detect: refined keypoints are a **flat run**
(no `manual/interpolated/filtered` subgroups). Manual edits are **in-place
overwrites** on the existing refined run arrays. There is no `resolved_group`
or `manual_review_latest` pointer.

## Target Run

Given `run_name = refined_keypoints_runs.attrs["latest"]`:

- Target: `refined_keypoints_runs/<run_name>`
- Edits overwrite individual ROI rows within existing arrays.
- The run group itself is not recreated; only affected ROI indices are updated.

## Per-ROI Write: Required Arrays

When saving a manual keypoint correction for ROI index `roi_idx`:

1. `keypoints_roi[roi_idx]` (`float64`, shape `(3, 2)`) — corrected keypoints in ROI pixels
2. `keypoints_img[roi_idx]` (`float64`, shape `(3, 2)`) — full-image pixels:
   - `keypoints_img = keypoints_roi + roi_coordinates_full[roi_idx]`
3. `keypoints_norm[roi_idx]` (`float64`, shape `(3, 2)`) — normalized:
   - `keypoints_norm = keypoints_img / [full_width, full_height]`
4. `heading[roi_idx]` (`float64`, scalar) — recomputed from corrected points:
   - Use `_compute_heading_from_points(bladder, eye_left, eye_right)` logic
   - If geometry is degenerate/non-finite, heading may be `NaN`; this is allowed.

Keypoint order: `[bladder, eye_left, eye_right]`.

## Geometry Recomputation Requirement

After writing corrected keypoint positions, the writer **must** recompute all
derived geometry arrays. This mirrors
`compute_geometry_metrics()` in `fisheye.refinement.keypoint_quality` and the
`save_current()` logic in `keypoint_failure_review.py:459`.

### Step 1: Compute triangle metrics

```
metrics = compute_geometry_metrics(keypoints_roi[roi_idx])
# Returns: area (float), angles (3,), min_angle (float), max_angle (float), edge_lengths (3,)
```

The geometry is computed from the three keypoint vertices:
- Triangle area via cross-product method
- Interior angles via law of cosines
- Edge lengths via Euclidean distance

If any vertex contains NaN, all metrics become NaN.

### Step 2: Write geometry arrays

- `triangle_area[roi_idx]` = `metrics.area`
- `min_angle[roi_idx]` = `metrics.min_angle`
- `triangle_angles[roi_idx]` = `metrics.angles` (shape `(3,)`)

### Step 3: Evaluate quality flags

Threshold values are read from `refined.attrs["summary_statistics"]["refine"]`:
- `confidence_threshold` (default: `0.3`)
- `min_triangle_angle` (default: `10.0` degrees)
- `min_triangle_area` (default: `100.0` pixels²)
- `max_triangle_area` (optional; no upper bound if absent)

Compute:
- `geometry_valid = (min_angle >= min_triangle_angle) and (area >= min_triangle_area) and (max_triangle_area is None or area <= max_triangle_area)`
- `confidence_valid = all(keypoint_confidences[roi_idx] >= confidence_threshold)`
  - For manual corrections, set `keypoint_confidences[roi_idx] = [1.0, 1.0, 1.0]`
  - Set `confidence[roi_idx] = 1.0`
- `usable_keypoints = confidence_valid and geometry_valid`

### Step 4: Write quality/status arrays

- `refined_success[roi_idx]` = `True`
- `flip_corrected[roi_idx]` = `False`
- `quality_labels[roi_idx]` = `0` (clean)
- `confidence_valid[roi_idx]` = computed above
- `geometry_valid[roi_idx]` = computed above
- `usable_keypoints[roi_idx]` = computed above
- `heading_finite[roi_idx]` = `isfinite(heading[roi_idx])`
- `heading_usable[roi_idx]` = `refined_success and detection_source == 0 and heading_finite`

These fields are now explicit:
- `heading_finite` is strict finiteness of `heading`.
- `heading_usable` is the downstream gate (`refined_success && detection_source==0 && heading_finite`).

Legacy comparison:
- Prior `heading_valid` represented `refined_success && detection_source==0`.
- `heading_usable` is stricter: prior gate **and** `heading_finite`.

### Step 5: Update reason tag

Read existing reason label for `roi_idx` (prefer `reason_bytes`, fallback `reason`), then:
1. Split on `|`, remove tags: `detection_failed`, `low_confidence`,
   `confidence_missing`, `fish_present_no_keypoints`, `detection_issue`
2. Remove any existing `manual_correction` tag (to avoid duplication)
3. Append `manual_correction`
4. If `geometry_valid` is false, append `geometry_issue`
5. Deduplicate and join with `|`
6. Write the updated label to `reason_bytes[roi_idx]` using null-terminated
   UTF-8 with zero padding.
7. Do not create or synchronize a variable-length `reason` array.

Example: `"detection_failed|low_confidence"` → `"manual_correction"`
Example: `"flip_corrected"` → `"flip_corrected|manual_correction"`

Compatibility note:
- `reason_bytes` is the sole persisted reason authority for current writes.
- Required attrs are `reason_encoding="utf8-null-terminated"`,
  `reason_authority="reason_bytes"`, the actual `reason_bytes_width`,
  `reason_bytes_null_terminated=true`, and
  `reason_fallback_order=["reason_bytes","detection_source"]`.
- Historical `reason` remains a read-only fallback. When editing a legacy
  reason-only run, materialize every effective row label into `reason_bytes`
  first and retire `reason` only after that canonical write succeeds.

## "No Keypoints" Marking

When an operator determines a fish is visible but keypoints cannot be placed:

- `keypoints_roi[roi_idx]` = NaN
- `keypoints_img[roi_idx]` = NaN
- `keypoints_norm[roi_idx]` = NaN
- `heading[roi_idx]` = NaN
- `confidence[roi_idx]` = NaN
- `keypoint_confidences[roi_idx]` = NaN
- `triangle_area[roi_idx]` = NaN
- `min_angle[roi_idx]` = NaN
- `triangle_angles[roi_idx]` = NaN
- `refined_success[roi_idx]` = `False`
- `flip_corrected[roi_idx]` = `False`
- `quality_labels[roi_idx]` = `0`
- `confidence_valid[roi_idx]` = `False`
- `geometry_valid[roi_idx]` = `False`
- `usable_keypoints[roi_idx]` = `False`
- `heading_finite[roi_idx]` = `False`
- `heading_usable[roi_idx]` = `False`
- `reason[roi_idx]`: append `fish_present_no_keypoints` tag

## Post-Write Summary Update

After all manual corrections are applied, refresh
`summary_statistics.postprocess` on the refined run. This is equivalent to
`_update_postprocess_summary()` in `keypoint_review.py:137`.

Required fields in `summary_statistics.postprocess`:
- `total_rois`: total keypoint ROIs
- `source_success`: count of `source_success == True`
- `refined_success`: count of `refined_success == True`
- `remaining_failures`: `total_rois - refined_success`
- `success_rate_percent`: `refined_success / total_rois * 100`
- `usable_keypoints`: count of `usable_keypoints == True`
- `confidence_valid`: count of `confidence_valid == True`
- `geometry_valid`: count of `geometry_valid == True`
- `flip_corrected`: count of `flip_corrected == True`
- `heading_finite`: count of `heading_finite == True`
- `heading_usable`: count of `heading_usable == True`
- `detection_source_counts`: value counts of `detection_source` as `{"0": n, "1": m}`
- `reason_counts`: tag counts from pipe-split `reason` array
- `retune_id_counts`: value counts of `retune_id` as `{"-1": n, "0": m, ...}`
- `retune_total`: sum of retune_id_counts excluding key `"-1"`
- `manual_corrections`: `reason_counts.get("manual_correction", 0)`

Write structure:
```json
{
  "summary_statistics": {
    "refine": { ... },
    "postprocess": { ... },
    "postprocess_updated_utc": "2026-02-10T..."
  }
}
```

Important:
- `set_keypoint_review_status` is metadata-only and does **not** recompute
  `summary_statistics.postprocess`.
- If Crimson writes arrays directly (without Palette manual-review UI), it must
  also refresh postprocess summary before setting final approval status.

## Attrs to Write

After corrections and summary update:

- `keypoint_review_status`: review payload (see acceptance contract)
- `keypoint_signature`: provenance signature (build if absent)
- `keypoint_review_signature`: copy of `keypoint_signature` at review time

The `keypoint_signature` is built from:
```json
{
  "signature_version": 1,
  "source_keypoints_run": "...",
  "source_crop_run": "...",
  "source_detect_run": "...",
  "source_refined_run": "...",
  "parameter_source": "...",
  "parameters_hash": "<sha256>"
}
```

Parent pointer update:
- `refined_keypoints_runs.attrs["keypoint_review_status_latest"] = <run_name>`

## Write Safety Rules

1. **Never mutate `keypoints_runs/<run>` in-place.** Raw keypoints are read-only.
2. Edits go to `refined_keypoints_runs/<run>` only.
3. Writes are **in-place per-ROI overwrites** — the run is not recreated.
4. Writes are idempotent by value: re-saving the same keypoints produces
   identical arrays.
5. All array lengths must remain consistent:
   - `keypoints_roi.shape[0] == keypoints_img.shape[0] == n_rois`
   - `len(reason) == n_rois`
   - `len(quality_labels) == n_rois`
   - `sum(frame_counts) == n_rois`
6. Do not change `frame_indices` or `frame_counts` during manual edits (ROI
   structure is fixed; only coordinates and derived arrays change).

## Validation Checks

Run after Crimson writes:

1. **Shape consistency**:
   - All ROI-level arrays share `n_rois` along axis 0.
   - `sum(frame_counts) == n_rois`.

2. **Geometry consistency**:
   - For every manually corrected ROI: `triangle_area` and `min_angle` are
     finite when `refined_success == True`.
   - `usable_keypoints == (confidence_valid and geometry_valid)`.
   - `heading` may be non-finite in degenerate geometry; this is not by itself
     a write failure.

3. **Reason tag consistency**:
   - Every manually corrected ROI has `manual_correction` in its reason tags.
   - No corrected ROI retains `detection_failed` in its reason tags.

4. **Summary freshness**:
   - `summary_statistics.postprocess_updated_utc` is recent.
   - `summary_statistics.postprocess.manual_corrections` matches actual count.

5. **Review status**:
   - `keypoint_review_status` exists on the refined run.
   - `keypoint_review_status_latest` points to this run.

## Implementation Note for Crimson Agent

When possible, mirror Palette's existing semantics from:
- `src/fisheye/tune/keypoint_failure_review.py` (manual correction logic)
- `src/fisheye/tune/keypoint_review.py` (postprocess summary)
- `src/fisheye/refinement/keypoint_quality.py` (`compute_geometry_metrics`)

Do not invent alternate field names for status/pointers; Palette tooling
already consumes the names listed above.

## Related Documents

- `docs/crimson_refined_detect_manual_contract.md`
- `docs/crimson_keypoint_read_contract.md`
- `docs/crimson_keypoint_review_acceptance_contract.md`
- `zarr_structure.md` (lines 301–348: refined keypoints)
