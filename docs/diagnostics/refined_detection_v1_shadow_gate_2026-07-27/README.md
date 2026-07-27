# Refined Detection V1 Shadow Gate

Date: 2026-07-27

Status: consumer gate passed; immutable Crimson commit binding pending

## Inputs

- Palette handoff payload digest:
  `1fce7956f7f24bf6588900263366ca1c15d060bfdf3bddd4ac1f6c609bdaaf82`
- Refined run: `refined_detect_shadow_crimson_20260727_v1`
- Canonical companion: `detect_canonical_shadow_crimson_20260727_v1`
- Palette comparator rule commit: `77f47b5a`
- Crimson base commit: `34ff3c38229450b4e6bbe9b16fed99e9ca966197`

The Crimson worktree was dirty during this run, so the result is bound to the
handoff and recorded source tree but not yet to an immutable Crimson commit.

## Comparator Decision

For Zarr v3 group nodes only, absent `consolidated_metadata`, JSON `null`, and
the exact empty inline envelope are equivalent:

```json
{"kind":"inline","must_understand":false,"metadata":{}}
```

Normalization removes only those three representations before exact JSON
comparison. Non-empty, malformed, wrong-kind, extra-field, and changed sibling
metadata fail. Array nodes must not contain `consolidated_metadata`; array
declarations and all other fields remain exact.

## Result

The unchanged Palette handoff passed:

- 28 refined declarations and 11 exact instance-side handles;
- three refined and two canonical direct-group comparisons;
- zero source-audit handles and one retained offset read;
- 23,287 frames, 22,926 refined rows, and 361 empty frames;
- exact uniqueness for `instance_key`, `refined_row_id`, and raw source rows;
- 22,926 overlay boxes with no dropped rows;
- 97 rapid seeks, 145 cancellations, and zero stale publications;
- one 1,169,226-byte resident snapshot; and
- zero additional TensorStore UI-field reads for the resident probe.

The real snapshot contains no manual or multi-instance frames. The deterministic
repository suite separately covers frame counts `[2, 0, 1, 3]`, raw plus manual
rows, two manual additions, overlapping boxes, and same-class rows.

Structured evidence is in `result.json`.
