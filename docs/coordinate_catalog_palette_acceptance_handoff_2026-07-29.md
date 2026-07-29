# Coordinate Catalog Palette Acceptance Handoff

Date: 2026-07-29

Status: **consumer accepted; production activation remains Palette-owned**

## Decision

Crimson accepts Palette's persisted coordinate-catalog contract and mounted
selector-ineligible canary for these versioned surfaces:

- canonical detection manifest v3;
- refined detection manifest v2; and
- geometry-only crop manifest v2.

No further Crimson contract or archive-compatibility change is required before
Palette begins its separately controlled activation process for newly
published snapshots. This decision does not itself authorize a selector,
registry, writer-default, or existing-archive mutation.

## Immutable Identity

- Crimson implementation commit:
  `ce478c7d13d2f870e6c711308090e28364872602`
- Crimson evidence commit:
  `4100719a2608334fa6f935ce9b40b40b0e3739c4`
- Crimson branch: `codex/ui-monolith-20260404181029`
- Palette artifact-producing commit:
  `7a710276beea3037a457f4bfbb5be9f0525de0dc`
- Palette final handoff checkpoint:
  `1dd7a8f9589dcbf8831b385a5ce36c41b428b85e`
- Palette handoff file SHA-256:
  `21ccf119dfe7910e6c2cce7b027a9318e7b3c8cec4702363fbc3d4460775c4d3`
- Crimson structured evidence SHA-256:
  `9918615e142a1f946eb98865f46e264cacff23a2885e008ce0030d87efc6fd7d`

## Accepted Evidence

The mounted macOS gate passed:

- strict canonical-v3, refined-v2, and crop-v2 manifest validation;
- the three frozen coordinate-catalog identities;
- direct/consolidated group equivalence and exact array declarations;
- exact typed TensorStore opens without fallback dtype probing;
- one retained canonical offset read and one retained refined offset read;
- all 13 crop declarations and the complete crop CSR frame index;
- refined-source, crop-policy, and pixel-authority lineage bindings;
- lazy refined source-audit behavior;
- the persisted row-zero crop identity, origin, extent, and ROI box;
- normalized-detection and ROI-to-source coordinate samples; and
- no production-state mutation.

The normalized projection's maximum cross-language error was
`0.0001092553` pixel after Crimson promoted persisted float32 coordinates to
double for presentation math. It passed the frozen `0.001`-pixel tolerance.
The ROI-to-source transformation was exact.

## Activation Boundary

Palette may now record the consumer gate as satisfied and decide when its
versioned producers should emit these manifests for newly published immutable
snapshots. Palette should preserve its normal fail-closed publication,
readback, selector, registry, and rollback lifecycle.

Crimson retains legacy compatibility adapters for archives without these
manifest versions. It does not infer coordinate contracts from legacy names.
After Palette publishes the first production-eligible coordinate-aware runs,
the remaining integration check is a normal selected-session application smoke,
not another storage-contract or physical-layout experiment.

## Evidence

- [Mounted gate report](diagnostics/coordinate_catalog_canary_2026-07-29/README.md)
- [Structured evidence](diagnostics/coordinate_catalog_canary_2026-07-29/evidence.json)
- [Static cross-language review](diagnostics/coordinate_catalog_cross_language_review_2026-07-28.md)
- [Shared coordinate contract](crimson_shared_coordinate_contract_v1.md)
