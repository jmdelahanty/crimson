# Crimson Phase 5M.1 Portable Polar Contract

Date: 2026-07-17

Status: complete. This checkpoint defines and tests the backend-neutral
chaser-distance polar boundary. Storage adapters and renderer integration remain
Phase 5M.2 and Phase 5M.3 work.

Lifecycle: **archive-ready completed checkpoint**. The contract is maintained
by source and tests; later Phase 5M checkpoints closed the stated follow-on work.

## Boundary

`src/chaser_distance_polar.h` is the only contract a migrated polar consumer
needs. It contains no TensorStore, Zarr, `ZarrDetectionLoader`, Metal, CUDA,
OpenGL, AVFoundation, or UI types. The abstract
`ChaserDistancePolarRepository` exposes an immutable dataset descriptor and an
exact-camera-frame lookup. Phase 5M.2 storage adapters will implement that
interface on each maintained storage path.

The descriptor carries:

- dataset availability, source group, selected run and component, and the
  selection rule used for each;
- row and chaser counts;
- typed distance units plus raw and validated coordinate/angle conventions;
- the dataset-wide valid maximum and its resolved display scale; and
- an explicit error when the dataset cannot be consumed.

Each frame sample carries the requested camera frame and, when present, the
exact source camera frame, all descriptor provenance and conventions, source
and discarded point counts, usable points, and an availability/error result.
Each point carries chaser identity, distance, bearing, source validity, RGBA,
and color provenance. A ready descriptor requires the expected polar source
group and explicit run/component selection provenance.

## Availability Contract

The six states are deliberately non-overlapping:

| State | Meaning |
| --- | --- |
| `dataset_unavailable` | The polar group or required dataset is absent. |
| `unsupported_metadata` | Required identity, dimensions, units, or conventions cannot be interpreted safely. |
| `exact_frame_missing` | The dataset is usable, but it has no row for the requested camera frame. |
| `valid_frame_empty` | The exact row exists, but it contains no scientifically usable points. |
| `ready` | The exact row exists and contains at least one usable point. |
| `read_failed` | Opening or reading failed, or an adapter returned a non-exact frame. |

A repository must never substitute a neighboring frame. A returned source
frame different from the requested frame is a contract failure, not a missing
frame or a valid empty frame. Returning points without any source frame is also
a read failure.

## Scientific Normalization

The initial contract accepts the characterized production metadata exactly:

- distance unit: millimeters;
- coordinate frame: `arena_relative_canvas_px`; and
- angle convention: arena positions are converted from y-down to mathematical
  y-up, heading is counterclockwise from +x, zero is in front, and positive
  egocentric bearing is anatomical left.

Surrounding ASCII whitespace is ignored. No undeclared aliases are inferred.
Unsupported or missing metadata becomes `unsupported_metadata` before scene
construction. Finite bearings are normalized to `[-180, 180)`.

Adapters submit source validity with every point. Canonicalization publishes
only points whose validity bit is true, chaser index is nonnegative, distance is
finite and nonnegative, and bearing is finite. It retains source and discarded
counts. Chaser indices are identities, not matrix column offsets, so sparse
nonnegative values are preserved independently of the descriptor's column
count. Therefore an exact row whose points are all invalid is represented as
`valid_frame_empty` while an absent row remains `exact_frame_missing`.

## Presentation Policy Carried by the Contract

The radial scale uses the dataset-wide maximum of valid finite distances and
adds the characterized 5 percent display headroom. Missing, non-finite, or
nonpositive maxima use a `1 mm` unit fallback before headroom. The contract
records which policy supplied the scale and clamps normalized radii to the
display range.

Color resolution is deterministic and records its provenance:

1. valid stimulus-protocol RGBA;
2. valid component-summary RGBA; or
3. the characterized fixed eight-color palette indexed by chaser.

Invalid RGBA candidates do not outrank a valid lower-precedence source.

## Verification

`tools/chaser_distance_polar_contract_tests.cpp` covers the production
GoodCopBadCop descriptor and exact-frame values, all six availability states,
run/component and color provenance, convention acceptance and rejection,
bearing normalization, radial scaling, invalid point filtering, exact-frame
enforcement, and the abstract repository interface.

The existing Zarr-loader dependency guard remains at its 28-file baseline and
the portable source contains no backend or renderer dependencies. The macOS
arm64 Release app rebuilt and passed 40/40 tests. The isolated CUDA 12.4
Linux/NVIDIA app rebuilt and passed 29/29 tests, then passed the authenticated
GoodCopBadCop 0:300 production playback smoke:

```text
[PlaybackSmoke] PASS start_frame=0 end_frame=300 presented_frame=300 presented_slot=0 view_idx=0 presented_count=350 elapsed_s=2.99341
```

The first-party Mac and Linux compile commands use ordinary Release `-O3`
without `-Ofast` or `-ffast-math`. Repository-controlled TensorStore dependency
builds strip bundled `dav1d`'s upstream fast-math option, and the provided
OpenCV build helpers explicitly disable OpenCV and CUDA fast-math options.
Externally supplied prebuilt libraries retain their own independent build
provenance and do not propagate compile flags into Crimson targets.
