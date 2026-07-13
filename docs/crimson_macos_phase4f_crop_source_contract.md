# Crimson Phase 4F-B Crop Source Contract

Date: 2026-07-12

Phase 4F-B introduces the backend-neutral contract shared by geometry-only live
crops, Orange acquisition crop video, and persisted Zarr crop images. This
checkpoint does not yet parse acquisition inventory, open another decoder, or
change either renderer.

## Source Capabilities

`CropSourceCapabilities` reports three independent session capabilities:

- `LiveGeometry`: exact full-frame pixels plus valid crop geometry;
- `AcquisitionVideo`: a camera-frame-mapped dedicated crop video; and
- `PersistedZarr`: a materialized crop image indexed by the analysis data.

Capabilities are independent from per-frame readiness. For example, an
acquisition video may be a valid session source while its decoder is still
waiting for the exact requested video frame.

The caller must choose an explicit preference:

```text
PreferLiveGeometry
PreferAcquisitionVideo
PreferPersistedZarr
```

Persisted Zarr remains the final compatibility fallback in the live- and
acquisition-preferred orders, but callers can prefer it explicitly for
compatibility comparisons. The caller also chooses whether temporary
preferred-source unavailability should wait or allow a lower-priority source:

```text
WaitForPreferred
AllowFallback
```

This prevents accidental source oscillation during normal decoder latency.
Invalid identity or contradictory metadata never falls through to another
source, even when fallback is allowed.

## Frame Selection

Every request carries one Crimson camera-frame identity. Source readiness is
exact:

- live geometry requires `exact_full_frame == camera_frame` and usable crop
  geometry for the same camera frame;
- acquisition video requires its resolved camera identity to match the request
  and the mapped and decoded video frames to be equal; and
- persisted Zarr requires a matching resolved camera identity and a crop index
  whose pixels are available.

Selection reports one of:

```text
Selected
NoCapableSource
MissingFrame
MissingGeometry
AwaitingExactFrame
OutOfRange
InvalidState
```

A selected result preserves the camera identity, source kind, source-local
frame or row index, optional geometry, and blank-frame state. Backend-native
surface handles are intentionally absent.

## Geometry

`CropFrameGeometry` records:

- camera and optional one-based recording identity;
- full-frame dimensions;
- crop-output dimensions;
- crop rectangle in full-frame pixels;
- optional detection rectangle in full-frame pixels; and
- explicit geometry, detection, and blank availability flags.

Validation rejects non-finite, non-positive, out-of-frame, escaped-detection,
and contradictory blank/detection geometry. Geometry-only live crops do not
require a detection flag; an edited or otherwise explicit ROI remains valid.

The portable transform retains scaling:

```text
x_crop = (x_full - crop_x) * output_width / crop_width
y_crop = (y_full - crop_y) * output_height / crop_height
```

Both points and rectangles use this transform.

An acquisition blank frame remains a selected exact video frame without usable
geometry. A geometry-only source cannot produce a live crop for that frame and
reports `MissingGeometry` instead.

## Build Boundary

The contract is compiled as `crimson_crop_contracts`. Its public header uses
only C++17 standard-library types and contains no CUDA, OpenGL, Metal,
AVFoundation, TensorStore, or ImGui types.

The source is explicitly removed from the NVIDIA application's glob because it
belongs to the dedicated library. The portable test target is explicit in the
macOS build preset.

## Automated Validation

`crop_source_contract_tests` covers:

- scaled point and rectangle transforms;
- invalid bounds, non-finite values, and detection containment;
- geometry-only exact selection and missing geometry;
- exact acquisition-video identity and mapped blank frames;
- preferred-source waiting versus explicit fallback;
- persisted-image fallback and pixel readiness;
- negative, stale, mismatched, and out-of-range identities; and
- contradictory blank/detection metadata that must not silently fall back.

macOS validation:

```text
cmake --build --preset build-macos-arm64-release -j 8
ctest --preset test-macos-arm64-headless
11/11 passed
```

NVIDIA-host validation first compiled the same source and test with GCC using
`-std=c++17 -Wall -Wextra -Werror`; the test passed. A cumulative isolated
worktree then configured with CUDA 12.4, TensorRT 10.0.1.6, OpenCV 4.10, and the
NVIDIA FFmpeg stack. `redgui` linked and all four portable CTests passed. The
known OpenCV/FFmpeg version-family linker warnings remain unchanged. No NVIDIA
application runtime code was changed in this checkpoint.

## Next Checkpoint

Phase 4F-C should add the acquisition repository and adapter:

- generic recording-root-relative archive path resolution;
- Zarr v2/v3 acquisition stream attribute discovery;
- structured crop CSV parsing and version 1 identity validation;
- production archive probes; and
- conversion of repository results into this crop-source contract.

Backend adapters can then connect geometry-only full-frame surfaces,
AVFoundation crop-video decode, and persisted analysis images without changing
the shared identity or geometry semantics.
