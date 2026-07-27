# Crimson Refined Detection V1 Consumer

Date: 2026-07-27

Status: read-only consumer implemented; real Palette shadow-snapshot acceptance
passed at Crimson commit `28537f64bcae765b062374b17dd879c0a9614ade`;
legacy editing remains unchanged and production write routing remains blocked.

## Scope

Crimson now has a backend-neutral read path for Palette's frozen immutable
`palette.stage.refined_detection` v1 snapshot. The implementation extends the
existing `CanonicalDetectionRepository` interface rather than introducing a
Metal-specific detection model:

- `detection_repository_selection` owns typed refined-first selection and the
  explicit canonical-raw compatibility policy;
- `tensorstore_refined_detection_repository` is the storage adapter;
- `CanonicalDetectionBuffer` owns bounded paging, cancellation, and optional
  byte-budgeted residency;
- `canonical_detection_overlay_scene_adapter` produces shared read-only scene
  primitives; and
- platform renderers consume those primitives without interpreting Zarr or
  refined-detection schemas.

The repository therefore applies to macOS, Linux, and Windows. The macOS shell
is the first session integration, not the owner of the contract.

## Selection

The shared selector is fail closed and uses this order:

1. an explicitly requested refined v1 run;
2. an approved authoritative refined v1 run;
3. a canonical raw run only when raw fallback was explicitly permitted and no
   refined authority exists.

An invalid explicit run or a present but invalid authority is terminal. It
never falls through to raw detections.

The macOS integration exposes three deliberately distinct requests:

```text
--refined-detection-run RUN
--benchmark-refined-detection-run RUN
--detection-run RUN
```

`--refined-detection-run` requires a selector-eligible complete run.
`--benchmark-refined-detection-run` is the separate explicit API for a
selector-ineligible shadow or benchmark snapshot. `--detection-run` identifies
the raw compatibility run and permits raw fallback only when no refined
authority is available.

## Read Contract

The refined adapter:

- consumes Zarr v3 inline consolidated metadata with exact persisted dtypes;
- checks the direct run-group declaration against its consolidated declaration;
- validates the run-manifest and authoritative-selection envelope digests;
- requires all 28 full-acquisition declarations, or all 38 clipped-snapshot
  declarations, with exact shapes and dtypes;
- opens only 11 instance-side handles required for ordinary presentation;
- reads and retains `instances/frame_row_offsets` exactly once;
- leaves every `source_detections` array handle unopened;
- resolves every row in `[offsets[f], offsets[f+1])`, including empty and
  multi-detection frames;
- carries `instance_key`, `refined_row_id`, `source_detect_row_index`, source
  kind, score validity, and manual-edit state through paging, residency, and
  shared overlay primitives; and
- validates frame/offset agreement, geometry, scores, class IDs, raw/manual
  source-row semantics, within-range identity uniqueness, and within-frame
  refined-row ordering before publication.

Palette's immutable publication gate remains authoritative for whole-snapshot
relationships that require reading the lazy audit table or predecessor
evidence: complete source-audit joins, manual-key collision checks against all
source candidates, cross-snapshot key preservation, clipped per-member source
evidence, and exact direct/consolidated declaration-tree normalization. Crimson
does not repeat those full-table operations during ordinary playback.

## Compatibility Boundary

Legacy mutable refined layouts and their ordinal/`uint8_t` editing identities
are not aliases for v1. They continue through the existing compatibility and
editing paths. This work does not write arrays, change Palette selectors, or
route current editing commands into immutable snapshots.

## Acceptance

The headless fixture covers the exact `[2, 0, 1, 3]` per-frame pattern,
repeated offsets, raw plus manual detections, two manual additions in one frame,
same-class and overlapping detections, stable identity propagation, exact typed
opens, one retained offset read, lazy audit handles, resident publication,
malformed offsets, duplicate keys, selector-ineligible gating,
recomputed-envelope tampering, and terminal explicit-selection failure.

The real Palette selector-ineligible shadow-snapshot gate opens through the
benchmark-only selection policy, renders all expected rows, retains offsets
once, preserves identities through seeks and residency, publishes no stale
results, and leaves source-audit handles unopened during normal playback.
Production routing and editing remain blocked pending the separate legacy
editing-boundary review.

The portable real-shadow harness is built as
`refined_detection_shadow_gate`. It derives archive and run identities from the
Palette handoff rather than accepting independent path arguments:

```bash
build/macos-arm64-release/refined_detection_shadow_gate \
  HANDOFF_MANIFEST.json EXPECTED_CANONICAL_PAYLOAD_SHA256 OUTPUT.json
```

The Palette v1 handoff exposed a Zarr-Python group-node representation
difference. For group nodes only, Crimson now treats an omitted value, JSON
`null`, and the exact empty inline consolidated-metadata envelope as equivalent.
Every other field remains exact. Non-empty, malformed, wrong-kind, and
array-level envelopes fail closed.

The unchanged real handoff subsequently passed all consumer gates. Structured
evidence is in
`docs/diagnostics/refined_detection_v1_shadow_gate_2026-07-27/result.json`.
The gate was rebuilt and rerun from clean immutable implementation commit
`28537f64bcae765b062374b17dd879c0a9614ade`.
