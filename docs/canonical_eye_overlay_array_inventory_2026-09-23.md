# Canonical eye-overlay array inventory

Scope: the new canonical eye-overlay reader, not the complete Crimson session.
Video, detections, dense masks, contours, keypoints, shapes and timeline plots
have separate readers and are not counted below. No loading behavior was changed
while preparing this inventory.

## Counts and when reads occur

- **21 distinct per-observation payload arrays** supply a mapped frame.
- **28 logical TensorStore reads** resolve an uncached, nonempty valid frame:
  `roi_angles` is read five times, `roi_vectors` twice and `roi_qa` three times,
  each for a selected channel. Other payload arrays are read once.
- **7 additional arrays** are read at repository open: six channel-index tables
  and the frame-row-offset index. Thus there are **28 distinct arrays total**,
  coincidentally the same number as the per-frame logical read-call count.
- Frame cache hits return the assembled result without those payload reads.
  Missing frames use the retained offsets and return without payload reads.
  Multiple observations in one frame are read as row ranges, not 28 calls per
  individual observation. Failures may stop the read sequence early.
- Turning off the master eye-overlay demand stops new per-frame eye requests.
  Repository metadata/index opening still occurs asynchronously at session open.
  Turning off individual labels/arcs/cones currently affects drawing, **not**
  the payload field selection; both eyes are still loaded.
- Logical reads, distinct arrays, decoded chunks, filesystem reads and NFS
  network operations are different counts. These numbers are not NFS requests.

Prefixes below are exact selected run roots, never independently chosen latest
upstream runs:

| Alias | Root |
| --- | --- |
| E | `analysis/eye_angle_runs/<selected-eye-run>` |
| S | `analysis/subject_shape_runs/<eye-bound-shape-run>` |
| M | `refined_subject_masks_runs/<bound-mask-run>` |

## All 21 per-frame arrays

| # | Array | Use |
| --- | --- | --- |
| 1 | `E/support/frame_indices` | Verify eye row matches the requested camera frame. |
| 2 | `E/support/source_acquisition_frame_index` | Verify explicit eye acquisition-frame identity. |
| 3 | `S/source_acquisition_frame_index` | Verify shape acquisition-frame identity. |
| 4 | `M/source_acquisition_frame_index` | Verify mask/crop acquisition-frame identity. |
| 5 | `E/support/instance_key` | Stable observation identity for the eye result. |
| 6 | `S/instance_key` | Match the ellipse observation to the eye observation. |
| 7 | `M/instance_key` | Match crop placement to the same observation. |
| 8 | `S/source_crop_row_ids` | Verify shape crop lineage. |
| 9 | `M/source_crop_row_ids` | Verify matching mask crop lineage. |
| 10 | `M/source_crop_xywh` | ROI placement and dimensions; convert camera ellipse centers to ROI-local coordinates and preserve scene scale. |
| 11 | `S/components/eye_left/ellipse_params` | Left ellipse center, major/minor lengths and major-axis angle. |
| 12 | `S/components/eye_right/ellipse_params` | Right ellipse center, major/minor lengths and major-axis angle. |
| 13 | `S/components/eye_left/ellipse_success` | Reject failed left ellipse fits. |
| 14 | `S/components/eye_right/ellipse_success` | Reject failed right ellipse fits. |
| 15 | `E/support/body_frame/valid` | Published body-frame validity. |
| 16 | `E/support/body_frame/origin_xy` | Retained body-frame support and finite-value validation; not directly used to position the current eye scene. |
| 17 | `E/support/body_frame/forward_axis_xy` | Body-relative arc reference and body-frame validation. |
| 18 | `E/support/body_frame/left_axis_xy` | Signed-arc orientation and body-frame validation. |
| 19 | `E/roi_angles` | Five named scalar channels, listed below. |
| 20 | `E/roi_vectors` | Two named 2-D gaze-vector channels. |
| 21 | `E/roi_qa` | Three named validity channels. |

Only these named channels are requested; the eye reader does not request all
141 angle columns. The storage backend can still decode the containing chunks.

| Array | Channels | Uses |
| --- | --- | --- |
| `E/roi_angles` | `left_eye_angle_deg`, `right_eye_angle_deg`, `vergence_eye_angle_deg` | Numeric labels and inspector values. |
| `E/roi_angles` | `left_gaze_signed_deg`, `right_gaze_signed_deg` | Signed angle arcs and inspector values; legacy scene label fallback. |
| `E/roi_vectors` | `left_gaze_xy`, `right_gaze_xy` | Measured gaze rays and cone direction; inspector values. |
| `E/roi_qa` | `valid_frame`, `valid_left`, `valid_right` | Suppress invalid measurements. |

## All 7 open-time arrays

| # | Array | Use |
| --- | --- | --- |
| 22 | `M/frame_row_offsets` | Locate each camera frame's observation rows. Entire index is retained; sample recording costs 23,500,840 bytes (22.4 MiB). |
| 23 | `E/angle_channel_index/name` | Resolve angle channels by name. |
| 24 | `E/angle_channel_index/roi_available` | Check per-observation angle availability. |
| 25 | `E/vector_channel_index/name` | Resolve vector channels by name. |
| 26 | `E/vector_channel_index/roi_available` | Check per-observation vector availability. |
| 27 | `E/qa_channel_index/name` | Resolve QA channels by name. |
| 28 | `E/qa_channel_index/roi_available` | Check per-observation QA availability. |

Group and array metadata (`zarr.json`), publication/lineage records and coordinate
descriptors are also read/validated; they are not extra payload arrays. This
inventory does not imply that open-time metadata I/O is negligible.

## What does a user actually need?

The visible feature set is much smaller than the array count: two ellipses, two
gaze vectors, a few angles, their validity, and a trustworthy mapping to the
video. Nine payload arrays implement row/frame/crop identity checks; twelve
supply geometry, measurements, validity or crop placement. Cones, overlap
polygons and label positions are constructed in the shared scene code; no
additional cone or overlap arrays are loaded.

Potential read-plan reductions, **not implemented by this inventory**:

1. **Identity/mapping reuse:** share already validated pages across bound eye,
   shape, mask and keypoint readers. Preserve exact source/run/frame/key checks
   rather than deleting them or relying solely on row ordinals.
2. **Feature-specific fields:** an axes-only view does not need angle scalars or
   measured gaze vectors; a view without labels does not need its three label
   scalars unless the inspector requests them. A view without arcs need not
   request the two signed-angle scalars solely for arcs. Preserve existing QA
   and fallback semantics explicitly when introducing these narrower plans.
3. **Body-frame support:** `origin_xy` is not directly consumed by eye drawing,
   but currently contributes to the finite-value validity gate. Treat removing
   its read as a deliberate, tested contract change, not an automatic cleanup.
4. **Batch selected channels:** issue selected angle/vector/QA reads together
   and overlap independent array I/O with bounded concurrency. Do not fetch all
   141 angle channels just to reduce API call count.
5. **Trace-only usage:** the existing timeline reader supplies frame-level
   traces separately. A user who only wants eye-angle traces does not need this
   per-observation eye-overlay payload at all.

These are ways to reduce repeated I/O without removing source data or weakening
scientific identity. Per-array latency/chunk metrics should guide prioritization;
the initial 4.9-second distant-read outlier remains unattributed.

Implementation references:
`src/zarr/tensorstore_bound_eye_geometry_overlay_repository.cpp` (reads and
validation), `src/read_only_overlay_scene.cpp:appendEyeGeometry` (actual drawing
dependencies), and `src/gui/eye_geometry_overlay_inspect_adapter.cpp` (inspector).
