# Crimson Shared Coordinate Contract v1

Date: 2026-07-28

Status: shared runtime vocabulary and transforms implemented. Canonical and
refined detection presentation adopted. Persisted crop/keypoint migration is
deferred until their exact provenance fields are available.

## Purpose

Crimson converts persisted scientific geometry into one renderer-neutral
camera presentation space:

`source_camera_continuous_pixels`

Metal, OpenGL/CUDA, hit testing, and editing must receive the same continuous
geometry. Integer extraction indices are not presentation coordinates, and a
legacy array name is not sufficient to infer a coordinate contract.

## Independent Declarations

Every new geometry contract declares these properties independently:

| Property | v1 values |
| --- | --- |
| domain | `source_camera`, `roi` |
| units | `normalized`, `pixels` |
| numeric kind | `continuous`, `integer_index` |
| geometry | `point`, `center_size_box`, `half_open_xyxy_box`, `integer_extraction_window` |
| axes | top-left origin, +x right, +y down |
| normalization | exact width/height denominator, or not applicable |

The maintained named spaces are:

- `source_camera_continuous_pixels`;
- `roi_continuous_pixels`;
- `source_camera_normalized`;
- `roi_normalized`; and
- `source_camera_integer_pixel_indices`.

Normalized coordinates are continuous and use exact width/height edge-space
denominators. They do not use `width - 1` or `height - 1`. Consequently, a
normalized edge coordinate of `1.0` maps to the far half-open image edge at
`width` or `height`, not the final integer pixel index.

## Geometry Semantics

A continuous point is an `(x, y)` location in its declared space. A
center-size box is `(center_x, center_y, width, height)`. A half-open XYXY box
is `[x_min, x_max) x [y_min, y_max)` and requires strictly positive width and
height.

An integer extraction window is `(x, y, width, height)` where `(x, y)` is an
integer source-pixel index and the width and height are positive integer
extents. It must fit within the authoritative source dimensions. It describes
which pixels are extracted; it is not a continuous point or box.

Conversions do not clamp geometry. Finite coordinates outside the nominal
image interval remain representable so invalid scientific inputs are not
silently changed. Callers that require containment must validate it as a
separate policy.

## Transform Authority

Source-camera normalization requires authoritative positive source width and
height. ROI placement additionally requires:

- the exact crop manifest digest; and
- the exact crop policy digest.

Digest strings are opaque exact identities. Crimson compares the entire
persisted strings and does not reinterpret or shorten them. A missing digest,
an out-of-bounds extraction window, or mismatched source dimensions makes the
strict ROI placement invalid.

## Maintained Conversions

For a normalized source-camera point:

```text
source_x = normalized_x * source_width
source_y = normalized_y * source_height
```

For a normalized source-camera center-size box, both the center and size use
the same width/height denominators before deriving half-open XYXY edges.

For an ROI continuous-pixel point:

```text
source_point = roi_point + extraction_origin
```

For an ROI normalized point:

```text
source_point = roi_normalized * extraction_size + extraction_origin
```

The shared module also provides the exact inverse point and box transforms for
hit testing and editing round trips.

## Persisted Representation Mapping

The target Palette/Crimson interpretation is:

| Persisted field | Contract | Authority |
| --- | --- | --- |
| `bbox_norm_coords` | source-camera normalized center-size box | authoritative geometry |
| `bbox_img_xyxy` | source-camera continuous-pixel half-open XYXY box | derived projection |
| `centers_img_xy` | source-camera continuous-pixel point | derived projection |
| `roi_coordinates_full` | source-camera integer extraction origin | crop contract |
| `roi_sizes_full` | integer extraction extent | crop contract |
| `source_crop_xywh` | complete source-camera integer extraction window | crop contract |
| `bbox_roi_xyxy` | ROI continuous-pixel half-open XYXY box | derived/local geometry |

The canonical/refined detection overlay adapter now converts
`bbox_norm_coords` through the shared module and requires the selected run's
source dimensions to exactly match the video surface. The shared read-only
overlay scene declares `source_camera_continuous_pixels` and fails closed if a
different space reaches the camera renderer.

## Compatibility Boundary

Legacy names such as `keypoints_norm`, `keypoints_img`, `roi_pixels`, and
`full_frame_pixels` remain readable only through compatibility adapters. Their
names alone do not establish domain, denominator, geometry, or transform
provenance.

Those adapters may convert legacy values into the shared presentation space,
but new persisted contracts must declare exact coordinate semantics and
authority. This checkpoint does not change keypoint storage, movement-trail
policy, crop archives, or Palette writers.

## Portable Implementation

The backend-neutral implementation is in `src/coordinate_contract.h` and
`src/coordinate_contract.cpp`. `coordinate_contract_tests` covers vocabulary
validation, width/height normalization, half-open boxes, ROI placement,
inverse round trips, missing provenance, and out-of-bounds extraction windows.
