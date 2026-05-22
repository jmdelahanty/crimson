# Palette Clipped Detection Surface Recommendations

<!-- handoff-meta
status: draft
last_updated: 2026-05-22
-->

## Purpose

This note summarizes what Crimson learned while adding read support for Palette
clipped refined-detect collections. It is intended as input for Palette's next
iteration on finalized clipped collection surfaces.

The short version:

- Crimson can read the current clipped representation, but it has to use a
  custom path that differs from non-clipped datasets.
- A materialized collection-level detection surface would let Crimson read
  clipped and non-clipped runs through nearly the same sparse-array contract.
- Row chunks should be modest, aligned across arrays, and must never split the
  four bbox columns.
- Shards are useful for sealed read surfaces, but they should be avoided or kept
  small for surfaces that need small random updates because of read-modify-write
  penalties.

## Current Crimson Read Shape

Status note, 2026-05-22:

- Crimson's clipped detection aggregation is now factored into
  `src/zarr/clipped_detection_repository.*`, but it is still a custom clipped
  read path.
- Clipped playback now has Crimson-side frame-keyed buffer guards so the
  presented frame, bbox lookup, and clip resolver stay synchronized during
  playback.
- This does not replace the need for a Palette materialized parent-timeline
  detection surface. Startup still has to aggregate selected per-clip runs.

Non-clipped datasets:

- Crimson opens one top-level run or stage from `detect_runs/...` or
  `refined_detect_runs/...`.
- It reads a flat sparse detection group with `frame_indices`, boxes, scores,
  class IDs, and optional source labels.
- `frame_indices` are already in the video timeline Crimson is navigating.
- The older path generally prefers `bbox_norm_coords`, with `bbox_img_xyxy` as a
  fallback that can be normalized when image dimensions are known.

Clipped datasets:

- Crimson resolves a finalized collection, reads the parent/clip mapping, and
  loads many per-clip refined run groups.
- Per-clip `frame_indices` are clip-local, so Crimson maps every row back to the
  parent recording timeline.
- Crimson currently prefers `bbox_img_xyxy` when metadata says it is already in
  source-video pixel coordinates.
- It aggregates all selected clip runs, sorts by parent frame, then exposes the
  result through Crimson's existing in-memory detection structure.

This works, but it makes clipped collection loading more expensive and more
specialized than non-clipped loading.

## Recommended Unified Surface

Palette should keep the per-clip run outputs as provenance and editing sources,
but expose an additional materialized collection-level read surface for
consumers. The exact path name is Palette's choice, but the useful shape is:

```text
<finalized_collection_path>/materialized_instances/
  frame_indices
  frame_offsets
  frame_counts
  bbox_img_xyxy
  bbox_norm_coords
  confidence_scores
  class_ids
  source_kind_codes
  refined_row_ids
  source_detect_row_index
  clip_local_frame_indices
  clip_indices or clip_id_codes
```

Recommended attrs on the materialized group:

```text
surface_kind = "parent_timeline_sparse_detections"
surface_version = 1
frame_index_space = "parent_frame_index"
row_sort_order = ["frame_indices", "clip_indices", "refined_row_ids"]
bbox_img_xyxy_coordinate_space = "source_image_xyxy"
bbox_img_xyxy_format = "xyxy"
bbox_img_xyxy_reference_width = <source video width>
bbox_img_xyxy_reference_height = <source video height>
bbox_norm_coords_coordinate_space = "normalized"
bbox_norm_coords_format = "cxcywh"
bbox_norm_reference_width = <normalization reference width>
bbox_norm_reference_height = <normalization reference height>
```

Recommended row semantics:

- `frame_indices` should be the parent recording frame index, not clip-local
  frame index.
- Rows should be physically sorted by parent frame.
- `frame_offsets` should have shape `(num_parent_frames + 1,)`, where
  `frame_offsets[f]..frame_offsets[f + 1]` gives the row span for frame `f`.
- `frame_counts` should have shape `(num_parent_frames,)` and should match the
  offset deltas.
- `clip_local_frame_indices` and `clip_indices` preserve the ability to trace a
  materialized row back to the clip-local output.
- `refined_row_ids` remain local to their source refined run unless Palette also
  defines a collection-global row identity. If they are source-local, consumers
  need `clip_indices` plus selected run identity to form a stable compound key.

With this surface, Crimson can treat clipped collections like ordinary sparse
detections over the parent timeline. It would still use the finalized collection
manifest for media selection and provenance, but it would not need to aggregate
all per-clip detection rows during startup.

## Chunking Recommendation

Use the same row chunk boundaries across all row-aligned arrays:

```text
frame_indices              chunks = (R,)
bbox_img_xyxy              chunks = (R, 4)
bbox_norm_coords           chunks = (R, 4)
confidence_scores          chunks = (R,)
class_ids                  chunks = (R,)
source_kind_codes          chunks = (R,)
refined_row_ids            chunks = (R,)
source_detect_row_index    chunks = (R,)
clip_local_frame_indices   chunks = (R,)
clip_indices               chunks = (R,)
```

Do not allow bbox chunks like `(R, 2)`. They are valid Zarr, but they split the
four bbox columns across chunks and make simple row-block consumers much easier
to get wrong. Bbox arrays should always use a column chunk size of exactly `4`.

Start with:

```text
R = 2048 or 4096 rows for interactive review surfaces
R = 8192 rows for more sequential read-heavy surfaces
```

For the current sleepyfish clipped smoke, one detection row is roughly one video
frame, so:

- `R=2048` covers about 68 seconds at 30 FPS.
- `R=4096` covers about 136 seconds at 30 FPS.
- `R=8192` covers about 273 seconds at 30 FPS.

Those are much smaller and easier to cache than whole-clip chunks of about
54,000 rows. If future datasets have many detections per frame, the same row
chunk size covers fewer frames, which is usually acceptable for per-frame UI
access.

Recommended supporting chunks:

```text
frame_offsets chunks = (8192 or 16384 parent frames,)
frame_counts  chunks = (8192 or 16384 parent frames,)
```

If Palette writes both `bbox_img_xyxy` and `bbox_norm_coords`, both should share
the same row chunk size. Crimson can skip `bbox_norm_coords` when
`bbox_img_xyxy` metadata is authoritative, but validators and other consumers
benefit from aligned chunks.

## Type Recommendation

For read surfaces, consider `float32` for `bbox_img_xyxy` and
`bbox_norm_coords`. Source images in this workflow are thousands of pixels wide,
and `float32` has enough precision for subpixel boxes while cutting geometry
payload size in half relative to `float64`.

Keep compatibility in mind:

- `confidence_scores`: `float32`
- `class_ids`: `int32` unless a new contract explicitly allows narrower labels
- `source_kind_codes`: `int8`
- `frame_indices`: `int32` if parent frames fit, otherwise `int64`
- `frame_offsets`: `int64`

## Sharding Guidance

Shards are attractive because they reduce object/file count for many small row
chunks. The tradeoff is mutability.

Use shards for:

- finalized, immutable, materialized read surfaces
- compacted collection-level views regenerated by Palette
- archival outputs where updates create a new version or new group

Avoid large shards for:

- arrays that Crimson or Palette will update in place during review
- small random edits to a few frames or rows
- "latest" mutable groups where one changed row should not require rewriting a
  much larger storage object

The concern is RMW cost. Depending on the Zarr implementation, codec, and
storage backend, changing one inner chunk in a shard can require reading,
decompressing, modifying, recompressing, and rewriting the containing shard. On
NFS or object storage, that can make small edits much more expensive than
ordinary unsharded chunks.

Recommended policy:

- For mutable edit surfaces, prefer unsharded row chunks with modest `R`.
- For sealed materialized read surfaces, shard modest groups of chunks rather
  than an entire collection.
- If using `R=2048` or `R=4096`, consider shard groups of 16 to 64 row chunks
  after measuring object count and update behavior on the target storage.
- Treat the sharded materialized surface as disposable/read-only: edit the
  source or write a new version, then regenerate or compact the sharded view.

## Mutability Pattern

The safest long-term pattern is:

1. Per-clip or per-review run groups remain the authoritative editable outputs.
2. Palette finalization writes a collection-level materialized read surface.
3. Review edits write a new run, overlay, or collection version instead of
   mutating a large sharded materialized group in place.
4. Palette regenerates the materialized surface after accepted edits.

If small edits must be supported without full regeneration, consider a delta
overlay:

```text
<collection>/materialized_instances/        # sealed base, possibly sharded
<collection>/edit_overlays/<edit_id>/       # small unsharded additions/edits/deletes
```

The overlay can store added rows, replacement rows keyed by collection row ID or
compound source identity, and tombstones for deleted rows. Palette can compact
base plus overlay into a new materialized version when review is complete.

## Validator Checks Palette Should Add

For clipped refined-detect run groups and any materialized collection surface:

- `bbox_img_xyxy` and `bbox_norm_coords` have shape `(N, 4)`.
- bbox chunk shape is `(R, 4)`, never `(R, 1)` or `(R, 2)`.
- all row-aligned arrays have the same first-dimension chunk boundaries.
- `len(frame_indices) == N` and optional row arrays are either absent or length
  `N`.
- rows in the materialized surface are sorted by parent frame.
- `frame_offsets` is monotonic, has length `num_parent_frames + 1`, starts at
  zero, and ends at `N`.
- `sum(frame_counts) == N`.
- bbox coordinate attrs declare format, coordinate space, and reference
  dimensions.
- if `bbox_img_xyxy_coordinate_space = "source_image_xyxy"`, reference width and
  height match the source video dimensions used by Crimson.

## Crimson Follow-Up

Crimson should still read Zarr arrays through logical indexing and not rely on
raw chunk memory being row-contiguous. Palette's safer chunks reduce risk for all
consumers, but older archives can legally have split-column chunks.

Once Palette exposes a materialized parent-timeline detection surface, Crimson
can reduce the custom clipped detection path to:

- resolve finalized collection media and provenance
- open the materialized sparse detection group
- read row chunks lazily or eagerly using the same path as non-clipped datasets
- use `clip_indices` and `clip_local_frame_indices` only for provenance and
  media switching
