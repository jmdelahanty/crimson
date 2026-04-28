# Crimson Derived Metrics Contract

Purpose: define a shared metadata contract for declaring derived metric
semantics on Palette runs so Crimson can recompute, load, and display those
metrics without modality-specific label heuristics.

Date anchored: 2026-04-11.

## Scope

This contract applies to any run/group that stores row-aligned entities and
derived arrays computed from them, including:
- keypoint ROI runs
- detect bbox runs
- refined/manual detect groups
- mask ROI runs
- future ROI- or detection-aligned review surfaces

This contract is metadata-only. It does not replace the underlying entity
storage format.

Current status:
- this document defines the target shared contract
- current Crimson implementations may still use modality-specific logic when the
  schema is absent or not yet consumed generically

Examples:
- `pose_schema` still defines keypoint labels/edges.
- bbox arrays still define box coordinates in their existing format.
- mask arrays still define bitmap layout and pixel semantics.

## Design Goal

Do not model all modality-specific quality semantics as a single
`quality_metrics` block.

That is too narrow for numeric quantities like area/aspect ratio/centroid and
too broad for modality-specific entity structure.

Instead, split the problem into:
- entity schema: what a row stores
- derived metrics schema: what numeric metrics are computed from that row
- quality gates: optional boolean/status outputs derived from those metrics

Recommended attr name:
- `attrs["derived_metrics_schema"]`

Recommended sibling schemas:
- `pose_schema` for keypoints
- future `mask_schema` if mask structure needs explicit declaration
- existing bbox coordinate conventions remain in the run contract

## Contract Shape

Recommended run attrs payload:

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

Fields:
- `schema_version`: integer contract version
- `entity_kind`: semantic row type, for example:
  - `keypoint_roi`
  - `detect_bbox`
  - `mask_roi`
  - `bbox_roi`
- `metrics`: list of numeric/vector derived metric definitions
- `quality_gates`: optional list of boolean/status gate definitions built from
  metrics and/or existing arrays

## Metric Definition

Each metric definition should describe:
- what source array it reads
- how it selects inputs from that source
- what outputs it populates

Recommended shape:

```json
{
  "name": "eye_triangle",
  "kind": "triangle_3pt",
  "source": {
    "array": "keypoints_roi",
    "value_kind": "points_xy",
    "selectors": {
      "labels": ["swim_bladder", "eye_left", "eye_right"]
    }
  },
  "outputs": [
    {"array": "triangle_area", "value_kind": "scalar"},
    {"array": "triangle_angles", "value_kind": "vector", "length": 3},
    {"array": "min_angle", "value_kind": "scalar"}
  ]
}
```

Recommended metric fields:
- `name`: stable logical name
- `kind`: computation family, for example:
  - `triangle_3pt`
  - `angle_3pt`
  - `distance_2pt`
  - `bbox_area`
  - `bbox_aspect_ratio`
  - `mask_area_pixels`
  - `mask_centroid`
  - `mask_iou`
  - `custom`
- `source.array`: source array path relative to the run/group
- `source.value_kind`: input interpretation, for example:
  - `points_xy`
  - `bbox_cxcywh_norm`
  - `mask_bitmap`
- `source.selectors`: modality-specific selectors, usually one of:
  - `labels`
  - `indices`
  - `channels`
  - `mask_value`
- `parameters`: optional computation parameters when the `kind` needs them
- `outputs`: arrays populated by this metric

## Quality Gate Definition

Quality gates are optional. Use them for booleans like `geometry_valid`,
`confidence_valid`, `usable_keypoints`, `bbox_valid`, or `mask_valid`.

Recommended shape:

```json
{
  "name": "geometry_valid",
  "output_array": "geometry_valid",
  "combine": "all",
  "conditions": [
    {
      "metric": "min_angle",
      "op": "gte",
      "threshold_attr": "summary_statistics.refine.min_triangle_angle",
      "default": 10.0
    },
    {
      "metric": "triangle_area",
      "op": "gte",
      "threshold_attr": "summary_statistics.refine.min_triangle_area",
      "default": 100.0
    },
    {
      "metric": "triangle_area",
      "op": "lte",
      "threshold_attr": "summary_statistics.refine.max_triangle_area",
      "optional": true
    }
  ]
}
```

Recommended gate fields:
- `name`: logical gate name
- `output_array`: boolean array written for the row set
- `combine`: `all` or `any`
- `conditions`: comparisons against metric outputs or existing arrays

Recommended condition fields:
- `metric` or `array`: source value name
- `op`: `gte`, `lte`, `gt`, `lt`, `eq`, `neq`, `isfinite`
- `threshold_attr`: run attr path to read threshold from
- `default`: fallback threshold value
- `optional`: if true, skip this condition when threshold attr is absent

## Storage Rule

The schema declares semantics only. It does not require nested storage for the
metric outputs themselves.

Derived arrays stay where the modality contract already places them, for
example:
- keypoints: `triangle_area`, `triangle_angles`, `min_angle`, `geometry_valid`
- detect: `bbox_area_norm`, `bbox_aspect_ratio`, `bbox_valid`
- masks: `mask_area_pixels`, `mask_centroid_xy`, `mask_valid`

## Modality Examples

### Keypoints

Use `pose_schema` for labels/edges and `derived_metrics_schema` for triangle or
distance metrics.

Example:

```json
{
  "derived_metrics_schema": {
    "schema_version": 1,
    "entity_kind": "keypoint_roi",
    "metrics": [
      {
        "name": "eye_triangle",
        "kind": "triangle_3pt",
        "source": {
          "array": "keypoints_roi",
          "value_kind": "point_xy"
        },
        "selectors": {
          "labels": ["swim_bladder", "eye_left", "eye_right"]
        },
        "outputs": [
          {"array": "triangle_area", "value_kind": "scalar"},
          {"array": "triangle_angles", "value_kind": "vector", "length": 3},
          {"array": "min_angle", "value_kind": "scalar"}
        ]
      }
    ],
    "quality_gates": [
      {
        "name": "geometry_valid",
        "output": {"array": "geometry_valid", "value_kind": "bool"},
        "evaluation": "all_conditions",
        "conditions": [
          {
            "metric": "eye_triangle",
            "output": "min_angle",
            "op": ">=",
            "threshold_attr": "summary_statistics.min_triangle_angle",
            "default": 10.0
          },
          {
            "metric": "eye_triangle",
            "output": "triangle_area",
            "op": ">=",
            "threshold_attr": "summary_statistics.min_triangle_area",
            "default": 100.0
          }
        ]
      }
    ]
  }
}
```

### Detect BBoxes

Boxes do not need a skeleton. The metric schema simply declares formulas over
the bbox representation already used by the run.

Example:

```json
{
  "derived_metrics_schema": {
    "schema_version": 1,
    "entity_kind": "detect_bbox",
    "metrics": [
      {
        "name": "bbox_area_norm",
        "kind": "bbox_area",
        "source": {
          "array": "bbox_norm_coords",
          "value_kind": "bbox_cxcywh_norm"
        },
        "outputs": [
          {"array": "bbox_area_norm", "value_kind": "scalar"}
        ]
      },
      {
        "name": "bbox_aspect_ratio",
        "kind": "bbox_aspect_ratio",
        "source": {
          "array": "bbox_norm_coords",
          "value_kind": "bbox_cxcywh_norm"
        },
        "outputs": [
          {"array": "bbox_aspect_ratio", "value_kind": "scalar"}
        ]
      }
    ]
  }
}
```

### Masks

Masks also should not be forced into keypoint-style semantics.

Example:

```json
{
  "derived_metrics_schema": {
    "schema_version": 1,
    "entity_kind": "mask_roi",
    "metrics": [
      {
        "name": "mask_area_pixels",
        "kind": "mask_area_pixels",
        "source": {
          "array": "masks_roi",
          "value_kind": "mask_bitmap",
          "selectors": {
            "mask_value": 1
          }
        },
        "outputs": [
          {"array": "mask_area_pixels", "value_kind": "scalar"}
        ]
      },
      {
        "name": "mask_centroid_xy",
        "kind": "mask_centroid",
        "source": {
          "array": "masks_roi",
          "value_kind": "mask_bitmap",
          "selectors": {
            "mask_value": 1
          }
        },
        "outputs": [
          {"array": "mask_centroid_xy", "value_kind": "vector", "length": 2}
        ]
      }
    ]
  }
}
```

## Reader/Writer Expectations

When `derived_metrics_schema` is present:
1. Palette and Crimson should treat it as authoritative for derived metric
   semantics.
2. Writers should recompute only the metrics whose inputs changed.
3. Readers should use the schema to label/display derived arrays when useful.

When `derived_metrics_schema` is absent:
1. Existing modality-specific behavior remains valid.
2. Legacy heuristics may still be used for compatibility.
3. New code should prefer emitting the schema rather than adding more
   heuristic label matching.

## Keypoint-Specific Compatibility

Current Crimson keypoint editing already has a schema seam for heading under:
- `pose_schema.metadata.heading_computation`

Heading should remain separate from `derived_metrics_schema` for now.

Reason:
- heading is a directional semantic used directly by UI/edit logic
- triangle metrics are quality-oriented derived quantities

Keypoint runs may use both:
- `pose_schema.metadata.heading_computation`
- `derived_metrics_schema`

## Summary Statistics

`derived_metrics_schema` does not replace `summary_statistics`.

Use:
- `derived_metrics_schema` to define how row-level values are computed
- `summary_statistics` to store aggregated counts, thresholds, and snapshots

Thresholds may still live under existing attrs like:
- `summary_statistics.refine.min_triangle_area`
- `summary_statistics.refine.min_triangle_angle`

## Adoption Recommendation

Recommended rollout:
1. Add `derived_metrics_schema` first for refined keypoint runs.
2. Teach Crimson to prefer it over keypoint label heuristics.
3. Later add bbox and mask metric declarations independently.
4. Keep legacy fallbacks until old runs without the schema are no longer common.

## Bottom Line

Use a run-level `derived_metrics_schema`, not a modality-wide `quality_metrics`
blob.

That keeps the contract general enough for boxes and masks while still letting
keypoint runs reference skeleton labels cleanly through their own entity schema.
