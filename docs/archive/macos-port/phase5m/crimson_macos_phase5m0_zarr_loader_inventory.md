# Crimson Phase 5M.0 Zarr Loader Inventory

Date: 2026-07-17

Status: complete. This is the characterization baseline for Phase 5M.1. It
describes the maintained read-only behavior after removing global C++
`-Ofast`/`-ffast-math` flags. No polar repository or portable polar type is
introduced here.

Lifecycle: **archived historical baseline**. The completed Phase 5M
contract and acceptance record supersede it as active guidance.

## Boundary

`ZarrDetectionLoader` is a compatibility facade over roughly 22,000 lines in
`src/zarr_loader*.{h,cpp}`. It combines archive discovery, TensorStore reads,
schema compatibility, eager and deferred caches, scientific lookup, UI-facing
view models, background workers, review state, and writes. Phase 5M does not
replace that facade wholesale. It extracts the chaser-distance polar read path
as a pilot while leaving every unrelated responsibility owned by the legacy
loader until a feature-level contract exists.

The direct-include baseline is 28 source files. The authoritative list is
`cmake/CrimsonZarrLoaderDependencyPolicy.cmake`; it is checked on every build
and by CTest through `cmake/CheckZarrLoaderDependencies.cmake`. A new source
file under `src/` cannot include `zarr_loader.h` unless the policy is
deliberately changed. No `src/platform/macos` file is in the allowlist.

## Polar Read Path

### Selection

`ZarrDetectionLoader::loadChaserDistancePolarData` in
`src/zarr_loader_stimulus.cpp` owns selection and loading.

1. Read attributes from `analysis/chaser_distance_runs` using Zarr v3
   `zarr.json` or Zarr v2 `.zattrs`.
2. Select the first string attribute in this order: `latest_complete`,
   `latest_completed`, `latest_success`, `latest`.
3. If no attribute selects a run, enumerate filesystem directories containing
   run-level `frames/camera_frame_id` and `distances/distance_mm`, sort their
   names lexicographically, and select the last name.
4. Under the selected run, repeat the same attribute precedence at
   `egocentric_bearing` to select a component.
5. If component attributes do not select one, enumerate component directories
   containing all three `per_chaser/bearing_deg`,
   `per_chaser/distance_mm`, and `per_chaser/valid` arrays, sort
   lexicographically, and select the last name.
6. A missing run, missing component, missing required array, empty frame or
   chaser index, or shape mismatch leaves the polar dataset unavailable.

The synthetic fixture proves both attribute precedence and lexicographic
fallback. This fallback is compatibility behavior, not a recommendation for
the new repository.

### Schema And Metadata

| Meaning | Preferred path | Compatibility fallback | Accepted form |
|---|---|---|---|
| Camera frame identity | `<component>/frames/camera_frame_id` | `<run>/frames/camera_frame_id` | Eager `int64` vector |
| Chaser identity by column | `<component>/per_chaser/chaser_index` | `<run>/chasers/chaser_index` | Eager `int32` vector |
| Egocentric bearing | `<component>/per_chaser/bearing_deg` | None | Rank-2 float32 or float64, converted to float |
| Distance | `<component>/per_chaser/distance_mm` | None | Rank-2 float32 or float64, converted to float millimeters |
| Validity | `<component>/per_chaser/valid` | None | Rank-2 bool, converted to bytes |
| Coordinate frame | Run attribute `coordinate_frame` | Empty string | String, carried but not validated |
| Angle convention | Component attribute `angle_convention` | `parameters.angle_convention` | String, carried but not validated |
| Component colors | `summary.chaser_color_hex` | None | Object keyed by chaser index; `#RRGGBB` or `#RRGGBBAA` |

All three matrices must have exactly `camera_frame_id.size()` rows and
`chaser_index.size()` columns. The loader eagerly materializes both matrices,
the validity matrix, frame and chaser vectors, one resolved color per column,
an `unordered_map<int64_t, size_t>` frame index, and the global radial maximum.
This is `O(rows * chasers)` memory and startup I/O. There is no bounded frame
read or read-ahead boundary in the legacy polar path.

### Lookup And Scientific Semantics

- The frame index is built with `unordered_map::emplace`; for duplicate camera
  frame IDs, the first row wins.
- A point is emitted only when its validity cell is true and both bearing and
  distance are finite. Removing global fast-math restored this source-level
  IEEE behavior and is guarded by the NaN fixture.
- Point order is column order. `chaser_index` carries identity independently of
  that order.
- The global radial maximum is the largest positive, finite distance whose
  validity cell is true. If none exists, it becomes `1.0 mm`.
- Colors resolve by chaser index in this order: stimulus protocol metadata,
  component summary hex color, fixed eight-color fallback palette. Stimulus
  protocol colors use `color_r`, `color_g`, `color_b`, and optional `color_a`
  fields clamped to `[0,1]`.
- Once any polar dataset is loaded, `getChaserDistancePolarFrame` sets
  `available=true` before lookup. Consequently, an absent camera frame and a
  present row containing no valid points are indistinguishable. Phase 5M.1
  must replace this with explicit unavailable, missing, valid-empty, ready,
  unsupported, and failed states.

The maintained renderer adds five percent headroom to the global maximum,
clamps each distance to that display radius, and maps a bearing with
`theta=(-90-bearing)*pi/180`. Its labels therefore place `0` at front,
positive bearings at anatomical left, `-90` at right, and `+/-180` behind.
The renderer does not inspect the stored convention strings; Phase 5M.1 must
validate or normalize them before scene construction.

## Consumer Flow

```text
analysis/chaser_distance_runs
        |
        v
ZarrDetectionLoader eager polar cache
        |
        +-> overlay_debug_panel: availability, controls, provenance text
        |
        +-> camera_view_frame_context_builder: exact current camera-frame lookup
                |
                v
        CameraViewWindowContext concrete loader frame
                |
                v
        camera_view_stimulus_overlay: ImGui geometry, labels and readout
```

`src/red.cpp` owns the persistent show/width/opacity/labels/readout control
state and passes it into those consumers. The concrete
`ZarrDetectionLoader::ChaserDistancePolarFrame` type crosses the frame-context
and renderer boundary. Phase 5M.3 removes that concrete dependency after both
storage adapters agree on the Phase 5M.1 contract.

## Responsibility Migration Ledger

| Legacy responsibility | Current implementation and consumers | Portable work already present | Migration state / next owner |
|---|---|---|---|
| Archive open and root metadata | `zarr_loader.cpp`; session loading in `media_session_loader.h` and `red.cpp` | `zarr/archive_context.h` and TensorStore archive context | Partial. Keep facade until remaining read paths use archive context. |
| Raw, refined, clipped, and interpolated detections | `zarr_loader.cpp`, `zarr_loader_detections.cpp`; Frame Inspect, camera overlays, review filters | Clipped detection repository and frame/overlay contracts | Partial. Read selection and review/mutation remain legacy. |
| Detection review and bounding-box mutation | `zarr_loader_write.cpp`, `zarr_bbox_edit.h`, `review_frame_state.h`, Frame Inspect | None for writes | Deferred until storage/edit contracts stabilize. |
| Crop image and ROI metadata | `zarr_loader.cpp`, `zarr_persisted_crop_provider.h`, `live_crop_image_provider.h` | Acquisition-crop and analysis-crop-geometry repositories | Partial. Legacy provider and edit-coupled crop workflows remain. |
| Refined keypoints and headings | `zarr_loader_eye_keypoint.cpp`, `refined_keypoint_repository.h`; review and camera overlays | Keypoint overlay repository and scene adapter | Partial. Read-only presentation extracted; review/cache update and writes remain. |
| Refined subject and eye masks | `zarr_loader_eye_keypoint.cpp`; chunk/prefetch workers, editor, camera overlays | Subject-mask and eye-geometry overlay repositories/adapters | Partial. Legacy caches, QC, and edit session remain. |
| Subject shape | `zarr_loader_subject_shape.cpp`; Frame Inspect and camera overlay | Subject-shape repository/adapter | Partial. Loader selection and QC navigation remain. |
| Tail kinematics | `zarr_loader_tail_kinematics.cpp`; Frame Inspect and analysis timeline | Portable analysis-series timeline | Partial. Loader selection, QC navigation, and unrepresented fields remain. |
| Eye angles and gaze | `zarr_loader_eye_keypoint.cpp`; eye panels, timeline, camera overlay | Eye-angle timeline and eye-geometry repositories | Partial. Loader representation metadata and QC navigation remain. |
| Movement, track, speed, swim bouts, and bout metrics | `zarr_loader_movement.cpp`; analysis timeline and camera trail | Analysis-series and swim-bout timeline repositories | Partial. Discovery, lazy loading, selection, and camera trail types remain. |
| Stimulus alignment and corrected/legacy frame mapping | `zarr_loader_stimulus.cpp`; playback, timeline, frame-context builder | Stimulus repository and stimulus-context timeline | Partial. Compatibility facade remains; mapping semantics feed Phase 5N. |
| Stimulus events and steps | `zarr_loader_stimulus.cpp`; timeline and camera overlays | Stimulus-context timeline covers stable timeline data | Partial. Remaining camera overlays are Phase 5N. |
| Chaser state, behavior, transforms, and bounding boxes | `zarr_loader_stimulus.cpp`; camera overlay and stimulus UI | No complete portable feature contract | Unmigrated. Extract only when its consuming feature is scheduled. |
| Chaser-distance polar inset | `zarr_loader_stimulus.cpp`; overlay controls, frame-context builder, ImGui overlay | Characterization probe and dependency guard | Phase 5M pilot. Contract is next in 5M.1. |
| Aggregate loader-owned UI view models | Nested public types in `zarr_loader.h`; 28 allowlisted direct consumers | Feature-level contracts introduced in earlier Phase 5 work | Shrink on contact. No new direct consumers allowed. |
| Refined keypoint/detection writes and edit-session publication | Loader write path plus repository/edit helpers | No stable cross-platform mutation contract | Explicitly out of Phase 5M/5N. |

The allowlist groups the remaining concrete consumers. It is a ceiling, not a
target count: migrated files should be removed from it rather than replaced by
new allowlist entries.

## Characterization Evidence

`tools/chaser_distance_polar_legacy_probe.cpp` compiles the actual maintained
loader on the Linux/NVIDIA target. Its self-test creates Zarr v3 TensorStore
fixtures and verifies:

- latest-complete attribute precedence and lexicographic filesystem fallback;
- run/component provenance and carried coordinate/angle metadata;
- exact row/chaser shape and global radial maximum;
- protocol, component-summary, and fixed-palette color precedence;
- first-row-wins duplicate frame lookup;
- finite/valid point filtering; and
- the legacy missing-frame versus valid-empty ambiguity.

CTest registers this as `chaser_distance_polar_legacy_characterization` on
NVIDIA builds. The dependency policy is registered as
`zarr_loader_dependency_check` on every platform.

The production descriptor and five exact-frame samples are stored in
`docs/reference/phase5m/legacy_polar_goodcopbadcop.json`. The GoodCopBadCop
archive selected:

- run `goodcopbadcop_chaser_distance_v1_20260617`;
- component
  `track_offline_goodcopbadcop_tk_hyst4_low2_latch_s005_id_0_smoothed`;
- 140,035 rows and two chaser columns;
- coordinate frame `arena_relative_canvas_px`;
- the declared mathematical-y-up, counterclockwise, positive-anatomical-left
  convention; and
- global radial maximum `76.72962188720703 mm`.

Frames 0, 56, and 140034 are available-but-empty under the legacy return type.
Frame 1024 contains `(c0, 42.59596 mm, +99.85936 deg)` and
`(c1, 52.74303 mm, +43.18930 deg)`. Frame 7024 contains
`(c0, 39.91535 mm, -154.33348 deg)` and
`(c1, 39.24881 mm, +134.40074 deg)`.

The probe opened the production archive read-only. Its root modification time
was `1784089667` before and after the probe.

## Build Semantics Correction

The first synthetic NaN fixture exposed that NVIDIA C++ targets were globally
compiled with `-Ofast -ffast-math`. Under that contract the compiler may assume
finite operands, so the loader's explicit `std::isfinite` guard admitted an
IEEE NaN point. CMake now relies on ordinary Release optimization and retains
only the existing x86 SSSE3 baseline; no global unsafe floating-point mode is
enabled. The earlier one-file `-fno-fast-math` exception became redundant and
was removed. CUDA math policy is unchanged.

This correction is intentionally broader than the polar code because finite
and NaN checks appear throughout scientific metadata, geometry, and review
paths. The synthetic characterization proves the corrected behavior through
the same target flags as the maintained loader.

## Acceptance Results

- Mac arm64 Release configured and built successfully; all 37 headless tests
  passed, including the cross-platform loader dependency check.
- The isolated CUDA 12.4 Linux/NVIDIA tree rebuilt all 190 affected targets
  without fast-math. All 28 tests passed, including
  `chaser_distance_polar_legacy_characterization` and the dependency check.
- The authenticated Linux/NVIDIA GoodCopBadCop playback smoke passed from
  frame 0 through frame 300: presented frame 300, 351 presentations, and
  `2.99279 s` elapsed.
- The production polar probe completed successfully and the archive root
  modification time remained `1784089667` before and after.
- Windows runtime validation remains the existing user-approved deferral; no
  Windows result is inferred from Linux source compatibility.

## Next Checkpoint

Phase 5M.1 defines the backend-neutral descriptor, point, exact-frame sample,
availability/error states, provenance, convention validation, radial policy,
and color provenance. It must treat the observed legacy behavior as evidence,
not copy the missing/empty ambiguity or silently accept unsupported coordinate
conventions.
