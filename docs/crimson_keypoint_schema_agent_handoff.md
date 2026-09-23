# Crimson Keypoint Schema Agent Handoff

Purpose: give the next Crimson agent a concrete starting point for finishing
the schema-sized keypoint and derived-metrics work already underway in this
repo.

Date anchored: 2026-04-28.

Implementation note, 2026-04-28:
- Crimson now has a narrow consumer for `derived_metrics_schema` in
  `src/refined_keypoint_repository.cpp`.
- The implemented subset covers `entity_kind="keypoint_roi"` metrics with
  `kind="triangle_3pt"`, `source.array="keypoints_roi"`, selectors by labels or
  indices, and the existing legacy outputs `triangle_area`, `triangle_angles`,
  and `min_angle`.
- When a `geometry_valid` quality gate is present, Crimson evaluates it from
  the computed triangle metrics; otherwise it keeps the legacy
  `summary_statistics.refine.*` threshold fallback.
- Crimson accepts the backfilled Palette schema shape observed in
  `/nvme1/recordings/2026-01-28T23-15-10Z_arena_2_Feeding/zarr/2026-01-28T23-15-10Z_arena_2_Feeding_analysis.zarr`:
  metric-level `selectors`, `source.value_kind="point_xy"`, gate
  `output.array`, `evaluation="all_conditions"`, and symbolic ops such as
  `is_finite`, `>=`, and `<=`.
- Runtime behavior can now be validated against that backfilled run; older runs
  still need to keep working through fallback logic.

## Read First

Before changing code, read these documents in order:

1. `docs/crimson_keypoint_schema_status.md`
2. `docs/crimson_derived_metrics_contract.md`
3. `docs/crimson_keypoint_manual_write_contract.md`
4. `docs/crimson_keypoint_read_contract.md`

These describe:
- what has already been implemented
- what remains incomplete
- the intended `derived_metrics_schema` contract
- the current fallback behavior that still exists in Crimson

## Problem Summary

Crimson previously had an asymmetry:
- it could read schema-sized keypoint runs for display
- but it still saved manual keypoint edits through a legacy fixed-3-keypoint
  write path

That write-path limitation has already been addressed locally.

The remaining problem is now narrower:
- the initial Crimson consumer exists, and there is now a real backfilled
  Palette run available for validation
- old runs still need to keep working through the current fallback logic

## Current Local State

This repo is dirty. Inspect it first with:

```bash
git status --short
git diff -- docs/crimson_keypoint_manual_write_contract.md docs/crimson_keypoint_read_contract.md src/gui/crop_keypoint_editor.cpp src/gui/crop_keypoint_editor.h src/gui/full_frame_keypoint_edit_overlay.cpp src/refined_keypoint_repository.cpp src/refined_keypoint_repository.h
```

At the time of this handoff, the key schema-related work in progress includes:
- `docs/crimson_keypoint_manual_write_contract.md`
- `docs/crimson_keypoint_read_contract.md`
- `src/gui/crop_keypoint_editor.cpp`
- `src/gui/crop_keypoint_editor.h`
- `src/gui/full_frame_keypoint_edit_overlay.cpp`
- `src/refined_keypoint_repository.cpp`
- `src/refined_keypoint_repository.h`
- `docs/crimson_derived_metrics_contract.md`
- `docs/crimson_keypoint_schema_status.md`

Also assume there may be unrelated dirty files in the workspace. Do not revert
other peoples' changes unless you have a very specific reason and have verified
they conflict with the current task.

## What Has Already Been Done

The following Crimson-side work is already in progress and should be preserved
unless you find a real bug:

1. schema-sized keypoint save payloads
   - crop editor save payload is variable-length
   - full-frame editor save payload is variable-length

2. schema-sized refined keypoint writes
   - manual write API accepts variable-length keypoint rows
   - write-time validation checks the target run's actual
     `keypoints_roi.shape`
   - `keypoints_roi`, `keypoints_img`, and `keypoints_norm` are written using
     the run's `n_keypoints` and `coord_dims`
   - extra point dimensions are preserved when `coord_dims > 2`

3. docs updated for schema-sized keypoint rows
   - read and write contracts no longer describe the save path as fixed
     `(3, 2)`

4. generalized derived metrics contract drafted
   - see `docs/crimson_derived_metrics_contract.md`

There was also a prior successful Crimson-side compile validation for `redgui`.

## What Still Needs To Be Finished

Primary objective:

Validate and harden Crimson's initial `derived_metrics_schema` support for
keypoint-derived geometry semantics while retaining the current legacy fallback
behavior for older runs that do not have the schema.

More concretely:

1. inspect the current geometry resolution logic in
   `src/refined_keypoint_repository.cpp`
2. inspect any relevant loader/schema parsing seams, especially around:
   - `src/keypoint_heading_utils.cpp`
   - `src/keypoint_heading_utils.h`
   - `src/zarr_loader_eye_keypoint.cpp`
   - `src/zarr_loader.h`
3. verify Crimson resolves triangle-geometry semantics from
   `derived_metrics_schema` when present
4. verify that resolved metric definition is used for the current legacy
   triangle outputs:
   - `triangle_area`
   - `triangle_angles`
   - `min_angle`
   - `geometry_valid`
5. preserve the current fallback path when the schema is absent

## Important Semantic Rules

Do not collapse heading and geometry into one mechanism.

Keep this split:
- heading semantics remain under `pose_schema.metadata.heading_computation`
- derived numeric geometry semantics move toward `derived_metrics_schema`

When `derived_metrics_schema` is absent, keep the current compatibility logic:
1. use `heading_computation.dependent_indices` if it yields a valid 3-point triad
2. otherwise fall back to swim/bladder + left + right label matching
3. otherwise fall back to `[0, 1, 2]` only for true 3-keypoint runs

## Likely Code Areas To Touch

Most likely:
- `src/refined_keypoint_repository.cpp`
- `src/refined_keypoint_repository.h`

Possibly:
- `src/keypoint_heading_utils.cpp`
- `src/keypoint_heading_utils.h`
- `src/zarr_loader_eye_keypoint.cpp`
- `src/zarr_loader.h`

Docs to update if behavior changes:
- `docs/crimson_keypoint_manual_write_contract.md`
- `docs/crimson_keypoint_read_contract.md`
- `docs/crimson_keypoint_schema_status.md`

## Constraints

1. do not undo the schema-sized save refactor unless you find a concrete bug
2. do not break old refined keypoint runs that lack `derived_metrics_schema`
3. do not assume Palette already emits the new schema
4. do not rewrite unrelated dirty files
5. do not make detect or mask runtime changes in this pass unless they are
   necessary for shared parsing infrastructure

## Suggested Work Sequence

1. inspect current diffs and understand the local schema-sized write work
2. inspect the current geometry resolution path in the repository
3. inspect the `derived_metrics_schema` doc and map it to the minimal runtime
   support needed for keypoint triangle metrics
4. validate keypoint geometry resolution from `derived_metrics_schema`
5. keep the legacy fallback path for old runs
6. update docs to match the actual runtime state if behavior changes
7. build and verify

## Suggested Verification

Start with:

```bash
git status --short
git diff --check
```

If the local build environment is available, use:

```bash
cmake -S . -B /tmp/crimson-ui-monolith-build -G Ninja -DOpenCV_DIR=/usr/lib/x86_64-linux-gnu/cmake/opencv4 -DCMAKE_PREFIX_PATH=/usr/lib/x86_64-linux-gnu -DCMAKE_IGNORE_PATH=/opt/orange
cmake --build /tmp/crimson-ui-monolith-build --target redgui -j2
```

If possible, also validate behavior against a real run that contains:
- schema-sized keypoint arrays
- a refined keypoint run
- ideally a `derived_metrics_schema` payload

If that runtime validation is not available, say so explicitly in the final
report.

## Acceptance Criteria

The task is complete when:

1. Crimson still supports schema-sized manual keypoint saves
2. Crimson resolves keypoint triangle geometry semantics from
   `derived_metrics_schema` when present
3. old runs without `derived_metrics_schema` still work through fallback logic
4. docs reflect the runtime state accurately
5. the final report clearly distinguishes:
   - what was already present in the repo
   - what you changed
   - what remains blocked on Palette emitting `derived_metrics_schema`

## Bottom Line

Do not restart this work from scratch.

The schema-sized save path has already been generalized locally. Crimson now has
the first keypoint `derived_metrics_schema` reader as well. The remaining
Crimson-side job is to validate and harden the contract integration phase:
- keep `derived_metrics_schema` authoritative for keypoint triangle geometry
  when present
- keep backward-compatible fallback behavior
- leave heading semantics under `pose_schema.metadata.heading_computation`
