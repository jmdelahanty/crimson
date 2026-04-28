# Crimson Keypoint Schema Status

Purpose: summarize what has already been implemented in Crimson for schema-sized
keypoint editing and derived metric contracts, where the system stands now, and
what should happen next in Crimson and Palette.

Date anchored: 2026-04-23.

## Goal

The original problem was that Crimson could read schema-sized keypoint runs for
display, but the manual edit/write path was still effectively hard-coded to the
legacy 3-keypoint fish workflow.

The broader follow-on problem was that some derived arrays, especially triangle
geometry outputs, were still defined implicitly by label heuristics rather than
by an explicit run contract.

The target state is:
- Crimson can read and write schema-sized keypoint rows for refined runs.
- Heading semantics remain explicit and schema-driven.
- Derived metric semantics can eventually be declared by Palette at the run
  level instead of inferred by Crimson.

## What Has Been Done

### 1. Schema-sized keypoint saves were implemented in Crimson

The manual keypoint save path in Crimson has been generalized from fixed
3-point payloads to variable-length keypoint vectors.

Implemented areas:
- crop editor save payload is now variable-length
- full-frame editor save payload is now variable-length
- refined keypoint repository write API now accepts variable-length keypoint
  rows
- write-time validation now checks against the target run's actual
  `keypoints_roi` shape rather than a hard-coded count
- `keypoints_roi`, `keypoints_img`, and `keypoints_norm` writes now use the
  run's `n_keypoints` and `coord_dims`
- when `coord_dims > 2`, Crimson preserves trailing dimensions instead of
  destroying them during XY edits

This closed the earlier gap where Crimson could inspect schema-sized keypoints
but could not save them end to end.

### 2. Keypoint contracts were updated to reflect schema-sized rows

The keypoint read and manual write contracts now describe:
- schema-sized `keypoints_*` arrays
- schema-driven layout rather than a fixed `[bladder, eye_left, eye_right]`
  assumption
- the current distinction between heading semantics and legacy geometry
  semantics

Relevant docs:
- `docs/crimson_keypoint_read_contract.md`
- `docs/crimson_keypoint_manual_write_contract.md`

### 3. A generalized derived metrics contract was drafted

A new metadata contract was added:
- `docs/crimson_derived_metrics_contract.md`

The intent is to avoid a top-level `quality_metrics` abstraction and instead
split the problem into:
- entity schema: what each row stores
- derived metrics schema: what numeric/vector outputs are computed from that row
- quality gates: optional boolean/status outputs derived from those metrics

The proposed run-level attr is:

```json
{
  "derived_metrics_schema": {
    "schema_version": 1,
    "entity_kind": "keypoint_roi",
    "metrics": [],
    "quality_gates": []
  }
}
```

This is intentionally broader than keypoints and is meant to also cover detect
boxes and masks later.

### 4. The current Crimson implementation was compile-checked

The schema-sized keypoint save refactor was built successfully in Crimson.

Important nuance:
- this validation covered the Crimson-side compile state
- it does not mean Palette already emits the new generalized metric schema
- Crimson now consumes the keypoint `triangle_3pt` subset of
  `derived_metrics_schema`, but not the full generic cross-modality contract

## Where We Are Now

### Crimson state

Crimson is now in a better position than before:
- it can already read schema-sized keypoint runs for display
- it can now save schema-sized manual keypoint edits to refined runs
- it can preserve extra point dimensions during XY edits
- it still honors schema-driven heading behavior through
  `pose_schema.metadata.heading_computation`
- when a refined keypoint run declares a compatible `derived_metrics_schema`,
  Crimson uses it to select the triangle inputs and to evaluate the
  `geometry_valid` quality gate

### Current limitation: geometry arrays are still legacy

The triangle geometry outputs are still legacy 3-point arrays:
- `triangle_area`
- `triangle_angles`
- `min_angle`
- `geometry_valid`

When `derived_metrics_schema` contains a compatible `keypoint_roi` /
`triangle_3pt` metric targeting those arrays, Crimson resolves the three
geometry inputs from that metric's selectors. Otherwise Crimson keeps the
fallback logic:
1. use `heading_computation.dependent_indices` when it forms a valid 3-point triad
2. otherwise fall back to swim/bladder + left + right label matching
3. otherwise fall back to `[0, 1, 2]` only for true 3-keypoint runs

This keeps old runs working while allowing new runs to declare the triangle
semantics explicitly.

### Derived metrics contract state

`derived_metrics_schema` is partially implemented in Crimson for refined
keypoint manual writes. It is not yet a fully deployed end-to-end runtime
feature.

That means:
- the schema is specified in docs
- keypoint docs now acknowledge it
- Palette has been confirmed to backfill it on at least one refined keypoint run
- Crimson consumes the documented `triangle_3pt` metric shape and
  `geometry_valid` quality gate when present
- Crimson also accepts the observed backfill spelling variants:
  `point_xy`, metric-level `selectors`, `output.array`,
  `evaluation="all_conditions"`, and symbolic ops such as `is_finite`, `>=`,
  and `<=`
- detect and mask derived metrics remain contract ideas rather than implemented
  shared semantics

### Overall status

The current system is in a transitional but usable state:
- schema-sized keypoint editing is implemented in Crimson
- the docs now describe the intended generalized direction
- Crimson has minimal keypoint derived-metric consumption
- the remaining gap is Palette production plus real-data validation, not the
  basic schema-sized save path

## Known Gaps and Risks

### 1. Geometry validity still depends on a resolvable 3-point triad

If a schema-sized keypoint run does not expose either a compatible
`derived_metrics_schema` triangle selector or a usable legacy geometry triad,
Crimson can still save the keypoint row and heading, but:
- geometry metrics remain `NaN`
- `geometry_valid` remains `False`
- downstream behavior may still reflect the legacy geometry assumptions

### 2. `derived_metrics_schema` is only partially runtime-authoritative

For refined keypoint triangle geometry, Crimson prefers
`derived_metrics_schema` when present. For other metric families and modalities,
the contract is still documentation-first.

### 3. Heading and geometry are still split across two mechanisms

This is intentional for now:
- heading semantics live under `pose_schema.metadata.heading_computation`
- derived numeric metrics are intended to live under
  `derived_metrics_schema`

That separation is reasonable, but it means the system is not yet unified under
a single runtime metadata reader.

### 4. Detect and mask work is still ahead

The generalized derived metrics contract was designed to cover:
- keypoints
- detect boxes
- masks

But only keypoints have been analyzed closely enough so far to define the
initial rollout path.

## Recommended Next Steps

### Next in Palette

Palette should be the first producer of `derived_metrics_schema`.

Recommended first rollout:
1. emit `derived_metrics_schema` on refined keypoint runs
2. declare the current eye-triangle metric explicitly
3. declare `geometry_valid` as a quality gate using the existing
   `summary_statistics.refine.*` thresholds
4. keep legacy arrays and thresholds exactly where they already live

This step removes the need for Crimson to guess triangle semantics from labels.

### Next in Crimson

Crimson should next:
1. validate the `triangle_3pt` schema path against a real Palette run
2. keep the current legacy fallback path for older runs that lack the schema
3. keep `pose_schema.metadata.heading_computation` as the authority for heading
4. broaden `derived_metrics_schema` consumption only after the keypoint path is
   proven against emitted data

### After keypoints

After the keypoint rollout is proven:
1. define bbox-derived metrics for detect runs as needed
   - examples: area, aspect ratio, validity gates
2. define mask-derived metrics for mask runs as needed
   - examples: area, centroid, validity gates
3. only add modality-specific sibling schemas when the entity structure itself
   needs one
   - `pose_schema` already exists for keypoints
   - a future `mask_schema` may make sense
   - detect boxes likely do not need a new entity schema beyond their existing
     coordinate contract

### Validation work that should still happen

The remaining validation should be operational, not just compile-level:
1. produce a Palette run with emitted `derived_metrics_schema`
2. load it in Crimson and verify display/edit/save behavior
3. verify that old runs without the schema still behave correctly via fallback
4. add regression coverage for schema-sized keypoint saves and schema-driven
   metric resolution

## Bottom Line

The hard part that was blocking Crimson manual editing has already been done:
Crimson can now save schema-sized refined keypoint edits instead of being stuck
on a fixed 3-point write contract.

Where the project stands now is mostly a contract integration phase:
- the Crimson-side save path is generalized
- the docs now define the intended shared derived metrics model
- Crimson consumes the initial keypoint triangle subset
- Palette still needs to emit the new metadata for real-data validation
- broader detect/mask metric consumption remains future work
