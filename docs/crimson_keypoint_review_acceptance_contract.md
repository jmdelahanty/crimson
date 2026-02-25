# Crimson Keypoint Review Acceptance Contract

Purpose: define a migration-safe, operator-first contract for approving
keypoint review status from Crimson after inspection/manual edits.

Date anchored: 2026-02-10.

Detect analog: `docs/crimson_detect_review_acceptance_contract.md`

## Scope

- In scope:
  - Writing review acceptance metadata on refined keypoint runs
  - Deterministic target-run resolution
  - Wrapper behavior for Crimson integration
- Out of scope:
  - Editing keypoint arrays (handled by manual write contract)
  - Running keypoint/refine stages
  - Registry schema changes

Related docs:
- `docs/crimson_keypoint_manual_write_contract.md`
- `docs/crimson_keypoint_read_contract.md`

## Existing Primitive

Palette already provides:
- `fisheye.utils.set_keypoint_review_status`

This contract treats that module as the source of truth for status payload
shape and attr write locations.

CLI invocation:
```
scripts/py -m fisheye.utils.set_keypoint_review_status <zarr_path> [options]
```

## Acceptance Model

Acceptance is metadata-only and append-safe:
1. Select refined run (`latest` unless explicitly specified).
2. Write `keypoint_review_status` onto selected refined run.
3. Write/update `keypoint_signature` and `keypoint_review_signature`.
4. Update parent pointer:
   - `refined_keypoints_runs.attrs["keypoint_review_status_latest"] = <run_name>`

No raw keypoint arrays are mutated.

Heading note:
- Approval should not fail solely because some rows have non-finite
  `heading` values.
- Current Palette semantics can produce `heading=NaN` for degenerate geometry
  even on rows otherwise marked successful.
- For heading-dependent approval criteria, gate on `heading_usable` (not `heading` alone).

Structural difference from detect: there is no `resolved_group` field because
refined keypoints are a flat run (no subgroups). The target is always the run
itself.

## Required Payload

Required fields in `keypoint_review_status`:
- `state`: one of `approved | pending | rejected | needs_review`
- `method`: one of `manual | algorithmic | hybrid | spotcheck`
- `intended_use`: one of `training | full_recording`
- `timestamp`: ISO UTC timestamp

Recommended fields:
- `reviewer`: operator identity
- `notes`: short rationale/context

Note: unlike the detect acceptance contract, there is no `resolved_group`
or `preference_chain` field. The flat run structure makes these unnecessary.

## Signature

On write, the utility computes or retrieves a `keypoint_signature` dict:

```json
{
  "signature_version": 1,
  "source_keypoints_run": "keypoints_2026-02-08_14-30-00",
  "source_crop_run": "crop_2026-02-08_14-00-00",
  "source_detect_run": "detect_2026-02-08_13-30-00",
  "source_refined_run": null,
  "parameter_source": "tuned",
  "parameters_hash": "<sha256>"
}
```

Write behavior:
1. If `keypoint_signature` already exists on the refined run, use it.
2. Otherwise, build it from run attrs (`source_keypoints_run`,
   `source_crop_run`, `source_detect_run`, `parameter_source`, hashed
   `parameters`).
3. Write `keypoint_signature` if newly built.
4. Copy to `keypoint_review_signature` (snapshot at review time).

This allows downstream consumers to detect if the underlying data has changed
since the review was recorded.

## Guardrails

### Primitive-enforced today (`set_keypoint_review_status`)

The current Palette utility fails with non-zero when:
1. No `refined_keypoints_runs` (or legacy `keypoints_refined_runs`) exists.
2. Explicit refined run is specified but not found.
3. No refined runs are available in the selected parent group.

### Wrapper-level policy (recommended fail-closed)

These checks are recommended for Crimson wrappers, but are not currently
enforced by the base utility:
1. Reject `state=approved` when refined run has zero ROIs.
2. Require `--reviewer` for `approved`.
3. Require `--notes` for `rejected`.

## CLI Wrapper

The existing utility `fisheye.utils.set_keypoint_review_status` already
provides CLI arguments:

- `zarr_path` (positional)
- `--refined-run <name>` (optional; default: latest)
- `--state <approved|pending|rejected|needs_review>` (default: approved)
- `--method <manual|algorithmic|hybrid|spotcheck>` (default: manual)
- `--intended-use <training|full_recording>` (default: training)
- `--reviewer <id>` (optional)
- `--notes <text>` (optional)
- `--no-latest` (do not update parent latest pointer)

The utility also handles the legacy `keypoints_refined_runs` group name
as a fallback.

## Crimson Integration Pattern

Recommended flow from Crimson:
1. Operator inspects keypoint overlays (per read contract).
2. Operator performs manual edits (if needed) per manual-write contract.
3. Post-write summary is refreshed (`summary_statistics.postprocess`).
   - If edits were not made through Palette's manual-review UI, explicitly run
     a summary refresh path before acceptance (for example keypoint-review
     audit behavior).
   - Ensure reason columns are synchronized before acceptance:
     `reason_bytes` is the required compatible representation; `reason` is
     secondary/best-effort where supported.
4. Crimson writes acceptance status with explicit intent:
   - approved training example:
     ```
     --state approved --method manual --intended-use training --reviewer <id>
     ```
   - spotcheck full-recording example:
     ```
     --state approved --method spotcheck --intended-use full_recording --reviewer <id>
     ```
5. Crimson stores command + response in its own action log.

Crimson may either:
- Call the CLI wrapper via subprocess, or
- Replicate the write logic directly (write `keypoint_review_status` attr,
  build/copy signature, update parent pointer).

If replicating, the implementation must match the field names and semantics
in `set_keypoint_review_status.py` exactly.

## Validation Checklist

After acceptance write:
1. `refined_keypoints_runs/<run>.attrs["keypoint_review_status"]` exists.
2. `keypoint_review_status["state"]` matches expected value.
3. `keypoint_review_status["timestamp"]` is recent and ISO-formatted.
4. `refined_keypoints_runs/<run>.attrs["keypoint_review_signature"]` exists.
5. Parent latest pointer updated:
   - `refined_keypoints_runs.attrs["keypoint_review_status_latest"] == <run_name>`
   (unless `--no-latest` was used).
6. `keypoint_review_signature` matches `keypoint_signature` (no data drift).
7. Non-finite heading rows (if present) are treated as expected edge cases, not
   automatic acceptance failures.

## Non-Goals / Stability Notes

- This contract does not alter keypoint/refine data models.
- This contract does not require DB migration.
- No `resolved_group` is needed (flat structure — contrast with detect's
  `manual/interpolated/filtered` subgroup chain).
- Field names must remain aligned with existing Palette readers; do not invent
  alternate keys.
- The `keypoint_review_status_latest` parent pointer serves the same role as
  `detect_review_status_latest` does for detections, allowing status reporters
  and downstream consumers to find the most recently reviewed run without
  scanning all runs.

## Related Documents

- `docs/crimson_detect_review_acceptance_contract.md`
- `docs/crimson_keypoint_read_contract.md`
- `docs/crimson_keypoint_manual_write_contract.md`
- `zarr_structure.md` (lines 301–348: refined keypoints)
