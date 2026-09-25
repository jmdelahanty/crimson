# Crimson Phase 5D Production Subject Masks

Date: 2026-07-13

Phase 5D connects the native macOS camera view to Crimson's production refined
subject-mask metadata. Binary ROI masks are resolved for an exact presented
camera frame, placed in full-image coordinates through their source crop rows,
and rendered as `R8Unorm` Metal textures. Stored component contours use the
same placement and are rendered as vectors.

This is a read-only parity slice. It does not port subject-mask picking,
painting, brush or polygon drafts, component visibility controls, ellipse-axis
overlays, writeback, or review-state mutation. Those remain later Phase 5
workflows and continue to belong to the maintained NVIDIA application.

## Source Contract

`SubjectMaskOverlayRepository` is the backend-neutral contract. It carries no
TensorStore, ImGui, Metal, OpenGL, CUDA, decoder, or Python dependency. One
resolution contains every verified detection for one camera frame and, for
each detection, its crop-row identity, full-frame ROI placement, semantic
component labels, immutable binary mask planes, and optional contours.

The TensorStore implementation selects `refined_subject_masks_runs` through
the maintained latest-run pointers. Storage precedence is:

1. dense `masks_roi`;
2. `mask_bitpacked/masks_packed` with the supported bitpacked schema; and
3. component `mask_rle` with the supported RLE schema when
   `mask_rle_stale` is not set.

Dense masks are the production realtime path when available. Bitpacked masks
are expanded least-significant bit first along width. RLE counts use COCO
Fortran order and are converted into row-major binary planes. All three paths
publish the same repository payload, with stored nonzero values normalized to
255 for Metal's normalized `R8` sampling.

`available_channels` is honored when it matches the component list. Missing or
incompatible availability metadata falls back to all stored channels, matching
the maintained tolerant read behavior. The selected run's `mask_labels` remain
the storage-channel authority; scene ordering does not rewrite channel order.

## Row Identity and Coordinates

`source_crop_row_ids` is the primary mask-to-crop join. Every referenced crop
row is range checked, and its camera frame and detection index must match the
mask row. A malformed explicit join fails repository construction instead of
drawing a mask on the wrong animal. Legacy same-position rows are accepted only
when the mask and crop arrays are aligned and pass the same frame check.

`crop_runs/<run>/roi_coordinates_full` supplies the full-frame top-left
placement. `roi_size` supplies height and width, with the stored mask dimensions
as a compatibility fallback. Mask texture coordinates remain ROI-local;
contour points are converted from ROI-local `(x, y)` into full-image pixels by
adding the crop offset.

Sampled contours are preferred from
`components/<label>/sampled_contours/{valid,points_xy}`. The validity array may
be stored as Zarr `bool` or `uint8`. If sampled contours are unavailable, the
loader falls back to `components/<label>/contours/{ptr,len,points_xy}`. A run
with `contours_stale=true` exposes no stored contours. TensorStore slice reads
use each returned array's offset origin and byte strides; they never assume a
sliced result begins at logical index zero or is tightly packed.

## Async Playback Policy

The camera render thread never performs a Zarr mask read. A bounded
`SubjectMaskOverlayBuffer` owns one worker, a 12-frame read-ahead queue, and a
24-frame immutable result cache. The GUI requests the current camera frame,
uses an exact cached result if ready, and otherwise presents video without a
mask for that frame. It never substitutes the most recently available mask.
Obsolete sequential requests outside the current read-ahead window are pruned,
so a slow read cannot leave an unbounded trail of frames that playback has
already passed.

Seeks, backward moves, and large forward jumps increment a generation, clear
queued/cache state, and discard results from the old generation. Normal
sequential requests retain read-ahead. Metrics report requests, cache hits,
mapped/missing/failed frames, discarded stale results, bounded queue/cache
peaks, and worst repository resolve time.

This policy protects playback from transient PRFS, VPN, and Wi-Fi latency. It
does not claim that every overlay frame is continuous when storage cannot keep
up. Missing mask metadata is an optional-overlay state; a mapped mask that
cannot be read, or mapped results that never reach Metal in a smoke range, is a
validation failure.

Normal playback never waits for a mask. Automated video smokes first hold the
logical clock at the requested start frame until that exact mask result is
available and the first drawable is submitted, then start the playback clock.
This makes the smoke assertion deterministic without introducing an
interactive render-thread read. `--no-subject-masks` is retained as a
diagnostic switch for playback isolation.

## Scene and Metal Path

The scene adapter accepts only a mapped resolution whose camera frame exactly
matches both the presented surface and the scene identity. Stale, future, and
cross-frame masks are withheld. Semantic component draw rank is fixed as:

```text
subject_body -> swim_bladder -> eye_left -> eye_right -> unknown components
```

This rank affects drawing only. The shared camera layer contract remains:

```text
... -> keypoint heading -> movement label -> subject masks -> subject shape ->
tail kinematics -> subject-mask picking -> keypoints
```

The macOS shell builds one combined read-only scene per presented camera frame.
The selected crop box, headings, subject-mask rasters and contours, and
keypoints are submitted through one Metal overlay encode. The renderer walks
the shared layer order explicitly, drawing mask rasters before mask contours
inside the subject-mask layer and keypoint markers afterward.

Each binary mask is uploaded as an `R8Unorm` texture and colorized in the
fragment shader. Texture keys use the immutable source group/run, crop row,
component, and dimensions. A bounded 64-texture cache prevents unbounded GPU
resource growth or cross-session texture reuse. Source-ROI intersection
generates both the visible display quad and its cropped texture coordinates, so
zoom and pan do not stretch or shift mask pixels. The camera display rectangle
is enforced with a Metal scissor.

## Deterministic Coverage

`subject_mask_overlay_repository_tests` creates real Zarr v3 fixtures through
TensorStore. It covers:

- dense, bitpacked, and RLE storage with exact expected binary planes;
- deliberately scrambled mask rows joined through `source_crop_row_ids`;
- multiple detections in one camera frame;
- sampled contour placement and canonical component ordering;
- exact, missing, out-of-range, and stale-frame behavior;
- bounded async read-ahead, exact cache publication, and discontinuity discard;
  and
- scene raster/contour generation without a GUI.

`apple_read_only_overlay_metal_tests` adds pixel-level coverage for an `R8`
binary fill, contour, full view, zoomed texture-coordinate crop, display
scissor, and stale-frame withholding. Existing box, heading, skeleton, and
keypoint pixel assertions continue to run in the same target.

## Production Fixture

The Phase 5D production gate uses the June 14 GoodCopBadCop analysis archive.
On 2026-07-13 its `latest`, `latest_complete`, and review-status pointers all
selected:

```text
refined_subject_masks_smart_finalizer_goodcopbadcop_prejuly_masks_wave01_20260712_v001
```

The run declares geometry-only crop `crop_2026-06-17_19-37-49`, 120,221 mask
rows, four 512-by-512 dense component planes, and sampled contour counts of 128
for body, 32 for swim bladder, and 64 for each eye. The stored channel order is
body, left eye, right eye, swim bladder; the scene applies the semantic draw
rank above without changing the storage mapping.

## Validation

The Apple Silicon release build completed and the full macOS preset passed 23
of 23 tests. `subject_mask_overlay_repository_tests` passed its TensorStore
Zarr v3 dense, bitpacked, RLE, row-join, contour, scene, and bounded-worker
fixtures. The offscreen Metal test passed binary-mask fill, contour, zoomed UV,
scissor, and stale-frame pixel checks alongside the existing vector overlay
checks.

The production probe resolved camera frame 1024 as one detection with all four
components present, 2,468 foreground pixels, and 288 contour points. The final
combined acquisition-crop smoke passed frames 1000 through 1300 with three
exact mask presentations, 12 presented components, 14 resolved mask frames,
zero failed reads, and a worst mask resolve of 2,000.4 ms over the mounted
network volume. Sparse presentations are expected under the exact-only
asynchronous policy; video is never held for late masks after smoke startup.

The same cumulative source configured and built in an isolated NVIDIA tree on
`ws1` with effective CUDA architectures 80 and 86. The maintained application,
CUDA/NVDEC path, TensorStore probe/tests, and portable overlay targets built;
the Linux preset passed 12 of 12 tests. Its production probe selected the same
run and resolved the same frame contents in 143.3 ms. The authenticated GUI
smoke passed frames 1000 through 1300 with the optional refined mask overlay,
and the ordinary frames 0 through 300 regression smoke also passed. The
4512-by-4512 HEVC NVDEC regression decoded all 160 requested frames at about
166.6 decode frames per second and 141.7 end-to-end frames per second on the
RTX A6000.

## Adjacent Acquisition-Crop Fix

Phase 5D validation reproduced the acquisition-exact jitter recorded in Phase
5C even with subject masks disabled. The dedicated crop decoder was already on
its own worker, but its six-frame queue was smaller than observed main-camera
catch-up jumps of up to 14 frames. Exact crop requests therefore flushed into
11 seeks, produced only 10 paired updates, and deferred 112 presentations.

The macOS acquisition crop session now uses a bounded 32-frame queue. For the
256-by-256 NV12 sidecar this is only a few MiB and 320 ms at 100 fps. The same
mask-disabled production smoke produced 112 paired updates, nine one-frame
deferrals, and one initial seek; the user-visible playback was materially
smoother. The final combined Phase 5D run produced 121 paired updates, nine
one-frame deferrals, one initial seek, zero camera skew, and a five-frame
maximum main-video lag. Exact camera/crop identity was unchanged.
