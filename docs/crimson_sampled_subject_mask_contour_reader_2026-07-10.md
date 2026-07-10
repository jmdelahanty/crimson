# Crimson Sampled Subject-Mask Contour Reader

**Date:** 2026-07-10

## Decision

Crimson prefers Palette's fixed-size sampled contour cache for subject-mask
display overlays:

```text
refined_subject_masks_runs/<run>/components/<component>/sampled_contours/
  points_xy          float32 [N,K,2]
  valid              bool    [N]
  source_point_count int32   [N]
```

The required schema is `sampled_component_contours_v1`. Coordinates are ROI
pixels in `(x, y)` order. Palette's current display sizes are body `K=128`,
each eye `K=64`, and swim bladder `K=32`.

Dense `masks_roi` remains the editable pixel authority. Sampled contours are a
derived display cache and are never a write target.

## Reader Policy

For each component, Crimson now:

1. tries `sampled_contours/points_xy` and `sampled_contours/valid`;
2. validates row count, rank, point width, coordinate metadata, and component
   identity;
3. falls back to legacy `contours/{ptr,len,points_xy}` if the sampled arrays
   are absent or structurally unreadable;
4. prefers sampled contours when both representations exist;
5. disables both stored representations when the refined run has
   `contours_stale=true`.

Missing or incomplete optional attributes produce a tolerant-read warning, as
with the existing ragged reader. Structurally incompatible arrays are not used.

Optional contour and eye-geometry discovery remains deferred from application
startup. Enabling the optional overlay opens array metadata only. The sampled
`valid` and `points_xy` payloads are read together for the requested mask row
window and copied into the existing mask-chunk contour cache. Crimson therefore
does not scan all validity rows or load all sampled points when the overlay is
enabled.

The overlay debug panel reports available, sampled, and ragged component
counts. `CRIMSON_SUBJECT_MASK_CHUNK_PERF=1` also reports sampled-versus-ragged
components alongside contour rows, points, read time, and copy time.

## Diagnostic Probe

`subject_mask_contour_probe` loads a named refined subject-mask run, requests
the deferred optional overlays, checks the selected representation, and can
materialize one frame through the same chunk-cache path used by the GUI.

Example:

```bash
release/subject_mask_contour_probe recording_analysis.zarr \
  --run refined_subject_masks_candidate \
  --storage dense \
  --expect-representation sampled \
  --expect-components 4 \
  --frame 0 \
  --require-row-contours
```

## Validation

The implementation was validated against the 120221-row GoodCopBadCop
recording shape.

- The published full-ragged run loaded all four components through fallback.
- A local overlay fixture exposed the real run's dense masks and metadata but
  contained fixed-K sampled contours and no required ragged surface. All four
  components loaded as sampled and frame 0 produced all four contours.
- Adding ragged directories to that fixture did not change selection: sampled
  remained preferred.
- The sampled fixture's frame-0 cache load copied 9216 contour points from the
  256-row window; the published ragged run copied 90351. Observed contour read
  times were 3.55 ms and 55.71 ms respectively, but this is not a controlled
  storage benchmark because the sampled arrays were local while the ragged
  arrays were on PRFS.
- `subject_mask_contour_probe`, `palette_clipped_loader_probe`, and `redgui`
  built successfully.
- CTest passed `tensorstore_zarr3_check` and `frame_slot_tests`.
- The authenticated GUI playback smoke loaded `sampled=4, ragged=0` and passed
  frames 0 through 120.

## Production Rollout Gate

Before Palette disables full ragged contours by default:

1. publish one full sampled-only refined-run canary to PRFS;
2. run `subject_mask_contour_probe` against that exact published run;
3. record cold and warm sampled contour reads on representative row windows;
4. visually inspect representative body, eye, and swim-bladder overlays;
5. retain ragged fallback in Crimson for historical runs;
6. flip Palette's production wrapper to sampled on, full ragged off.

No historical run needs rechunking or migration for this rollout.
