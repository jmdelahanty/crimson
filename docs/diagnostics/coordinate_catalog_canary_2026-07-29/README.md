# Coordinate Catalog Mounted-Archive Gate

Date: 2026-07-29

Status: **PASS**

## Verdict

Crimson accepts the selector-ineligible coordinate-catalog canary at Palette
checkpoint `1dd7a8f9589dcbf8831b385a5ce36c41b428b85e`.

The mounted macOS gate validates canonical detection v3, refined detection v2,
and geometry-only crop v2 through Crimson's real TensorStore repositories. No
consumer-side contract blocker remains for a future explicit production
activation of these manifest versions. This checkpoint did not modify a
selector, registry, writer default, production archive, or the canary.

## Identity

- Crimson implementation commit:
  `ce478c7d13d2f870e6c711308090e28364872602`
- Palette artifact-producing commit:
  `7a710276beea3037a457f4bfbb5be9f0525de0dc`
- Palette handoff checkpoint:
  `1dd7a8f9589dcbf8831b385a5ce36c41b428b85e`
- Handoff file SHA-256:
  `21ccf119dfe7910e6c2cce7b027a9318e7b3c8cec4702363fbc3d4460775c4d3`
- Handoff canonical-payload digest:
  `c4c6b186386ea8711b7d5517ae369d49d187b26228e0d6c2c6cc6228bab98692`
- Structured evidence SHA-256:
  `9918615e142a1f946eb98865f46e264cacff23a2885e008ce0030d87efc6fd7d`

The artifact-producing and handoff-checkpoint Palette commits are recorded
separately because the immutable handoff embeds the former while the user
provided the latter as the final pushed checkpoint.

## Results

| Surface | Frames | Rows | Manifest | Coordinate catalog | Offset reads |
| --- | ---: | ---: | --- | --- | ---: |
| Canonical detection v3 | 23,287 | 22,938 | valid | valid | 1 |
| Refined detection v2 | 23,287 | 22,926 | valid | valid | 1 |
| Geometry-only crop v2 | 23,287 | 22,926 | valid | valid | 1 |

Additional checks passed:

- Handoff raw-file and canonical-payload digests.
- Direct/consolidated group equivalence and exact consolidated array schemas.
- Exact typed TensorStore opens without dtype probing for coordinate-aware
  paths.
- All 13 crop array declarations and the full crop CSR frame index.
- Canonical, refined, crop-policy, pixel-authority, and cross-stage lineage
  digests.
- Lazy refined source-audit behavior (`source_audit_handle_opens=0`).
- Persisted crop row-zero `instance_key`, ROI origin, ROI extent, and ROI box.
- Source-normalized bbox and ROI-to-source-camera coordinate transforms.
- No production-state mutation.

The ROI-to-source transform was exact. The normalized bbox projection differed
from Palette's persisted float32 projection by `0.0001092553` pixel because
Crimson promotes the stored float32 coordinates to double before scaling. This
passes the cross-language tolerance of `0.001` pixel and is not a semantic
coordinate mismatch.

The observed open times (`383 ms` canonical, `366 ms` refined, `731 ms` crop)
are diagnostic values from one mounted-volume pass. They are not a performance
gate and were not used as acceptance criteria.

## Consumer Changes

- Canonical v3 and crop v2 manifests now validate fail closed, including their
  frozen coordinate-catalog identities.
- Canonical descriptors retain manifest and coordinate-validation provenance.
- Coordinate-aware crop runs use exact dtypes and consolidated declarations;
  the legacy probe-based adapter remains isolated for old runs without v2
  manifests.
- Crop descriptors retain crop-policy, refined-source, and pixel-authority
  digests.
- Crop resolution exposes persisted row identity and ROI-local bbox geometry.
- The backend-neutral coordinate contract now maps ROI-local half-open XYXY
  boxes into source-camera continuous pixels.

## Verification

The mounted gate was run as:

```bash
build/macos-arm64-release/coordinate_catalog_canary_gate \
  /Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/coordinate_catalog/integration/20260128_coordinate_catalog_crimson_20260728_v1/handoff_manifest.json \
  21ccf119dfe7910e6c2cce7b027a9318e7b3c8cec4702363fbc3d4460775c4d3 \
  docs/diagnostics/coordinate_catalog_canary_2026-07-29/evidence.json
```

The complete macOS headless preset passed 60/60 tests. Contract tests include
recomputed-digest tampering of canonical dimensions and crop pixel authority.

Structured evidence: [evidence.json](evidence.json)
