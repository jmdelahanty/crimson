# Palette Coordinate Catalog Cross-Language Review

Date: 2026-07-28

Palette checkpoint:
`154d788892aa8264d309258cc507d0d82a06e622`

Verdict: **ACCEPT** for a selector-ineligible coordinate-aware canary.
Production selection remains blocked until the archive-level gate passes.

## Accepted Contract

Crimson accepts the exact coordinate-catalog envelope at
`<run>/zarr.json.attributes.run_manifest.payload.coordinate_contract`:

- schema `palette.persisted_coordinate_catalog` version 1;
- digest algorithm `sha256_canonical_json_v1`;
- exact envelope fields with strict finite JSON;
- a document using `palette.array_coordinate_catalog` version 1; and
- one complete binding from every coordinate-bearing array contract to one
  declared surface.

The cross-language fixture was generated directly from the frozen Palette
builders. Crimson reproduces all three document digests:

| Stage | Manifest version | Bindings / surfaces | Catalog digest |
| --- | ---: | ---: | --- |
| canonical detection | 3 | 3 / 3 | `337613bd6e5f283eef9d6a89c14766d50c5b6863dea584f7568b90bb1d936733` |
| refined detection | 2 | 6 / 3 | `75656615ecd32a215f6b4148a01c9ef75e96b8d7aa6bf9fb8a7d21757fa7a2ed` |
| geometry-only crop | 2 | 7 / 7 | `e9ce640761ee1de4a6edd72695968bd66ae2fcbdd09d7d2c902450904f6ddfec` |

The digest is both recomputed and compared with the frozen stage digest. A
modified document therefore remains invalid even if a writer recomputes the
inner catalog digest and enclosing run-manifest payload digest.

## Vocabulary Mapping

Palette's `source_camera_image_px` domain maps to Crimson's explicit runtime
name `source_camera_continuous_pixels`. This is a naming adapter, not a numeric
conversion. Both represent continuous source-camera pixel geometry with a
top-left origin, +x right, and +y down.

Crimson accepts these mappings:

- normalized source-camera coordinates scale by exact source width/height;
- direct source-camera pixel surfaces pass through unchanged;
- ROI-local surfaces require the exact rowwise crop placement; and
- extraction extents are non-positional measurements.

Integer `roi_coordinates_full` values remain extraction indices in the array
contract. Their associated surface describes the continuous geometric location
of that integer origin after decoding. Crimson does not reinterpret
`roi_sizes_full` as a point, and it combines origin and extent only through the
strict integer extraction-window contract.

## Crimson Implementation

The backend-neutral Zarr layer now provides:

- strict canonical JSON SHA-256 validation;
- frozen stage identity and structural validation;
- duplicate/missing binding and surface rejection;
- exact binding and surface resolution; and
- refined run-manifest v2 validation with a descriptor flag proving that the
  catalog was validated.

Refined v1 remains unchanged. Canonical v1/v2 and crop v1 remain explicit
compatibility paths. Crimson does not infer a new catalog from old array names.

Tests use the producer-generated fixture at
`tools/fixtures/palette_coordinate_catalogs_154d7888.json`. They cover all
three digests, binding resolution, non-square source dimensions, ROI placement,
wrong-stage catalogs, extra fields, non-finite JSON, nested digest tampering,
and simultaneous nested/outer digest recomputation.

## Canary Request

Palette may now publish one immutable, selector-ineligible canary containing:

1. One canonical detection v3 run.
2. One refined detection v2 run bound to the canonical run.
3. One geometry-only crop v2 run bound to the refined run.
4. Inline consolidated metadata with direct/consolidated equivalence already
   validated by Palette.
5. Exact run names, archive paths for `/groups` and `/Volumes/johnsonlab`, a
   handoff manifest, and its SHA-256.
6. Source-camera width/height, source recording/video association, and exact
   crop policy and manifest digests.
7. At least one declared sample for normalized detection conversion and one
   rowwise ROI-to-source conversion, preferably using non-square dimensions.

The canary should contain empty and multi-row frames when practical, but the
existing deterministic `[2, 0, 1, 3]` tests remain the authoritative
multi-subject consumer gate. No selector, registry, writer default, or
production archive should change.

## Remaining Archive Gate

On receipt, Crimson will:

- open every canary array at its exact declared dtype without probing;
- require direct/consolidated metadata equivalence;
- validate the outer run manifest and nested catalog before exposing a run;
- verify the live source dimensions and crop descriptor against the catalog;
- transform declared samples into `source_camera_continuous_pixels`;
- reject recomputed semantic tampering and explicit invalid runs without
  fallback; and
- confirm that legacy adapters remain isolated.

Canonical v3 and crop v2 production consumption are intentionally not activated
before this archive-level evidence exists.
