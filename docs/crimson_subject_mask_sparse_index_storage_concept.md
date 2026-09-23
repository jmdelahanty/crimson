# Crimson Subject-Mask Sparse-Index Storage Concept

Status: deferred design concept, not a maintained storage contract.

This note records a possible persisted representation for binary subject masks
that matches Crimson's efficient in-memory playback representation. It does not
change the authoritative storage decision in
`docs/crimson_subject_mask_editable_dense_storage_decision_2026-07-08.md`.

## Motivation

An editable subject-mask row with four `512 x 512` components occupies 1 MiB
when represented as dense `uint8` planes. A Zarr row chunk containing 256 rows
and one component per physical chunk expands to 256 MiB across all four
components when decoded.

Crimson's established Linux/Windows display path avoids retaining that decoded
array. It scans the chunk once, records the exact flattened positions of every
nonzero pixel, and releases the dense TensorStore result. Sparse masks can be
much smaller when foreground occupancy is low. For example, 2,685 foreground
pixels require 10,740 bytes of `uint32` indices instead of a 1 MiB dense frame.

Persisting an equivalent derived cache could reduce cold-start conversion work
and improve read-heavy review over network filesystems.

## Candidate Representation

Use ordinary fixed-type Zarr arrays in a CSR-style layout rather than requiring
a variable-length array extension. Each semantic component would have a row
pointer and a flat foreground-index stream:

```text
mask_sparse/
  zarr.json
  components/
    subject_body/
      indptr       uint64 [n_rows + 1]
      indices      uint32 [n_foreground_pixels]
      present      bool   [n_rows]
    swim_bladder/
      ...
    eye_left/
      ...
    eye_right/
      ...
```

For mask row `r`, the exact foreground pixels are:

```text
indices[indptr[r]:indptr[r + 1]]
```

Each index is row-major and reconstructs an ROI-local pixel as:

```text
y = index / mask_width
x = index % mask_width
```

Candidate group attributes would need to declare at least:

- a versioned schema identifier;
- source refined-subject-mask run and revision identity;
- logical row, component, height, and width dimensions;
- component names and label-schema identity;
- row-major index order and index dtype;
- source dense-array identity or content digest;
- generation status and a stale flag.

The final schema must also define maximum index values, monotonic `indptr`
validation, duplicate-index policy, sorted-index policy, absent rows, and empty
but valid masks.

## Exactness

Sparse indices are lossless for Crimson's binary-mask contract. Every nonzero
dense pixel maps to one flattened index. Holes, disconnected regions, and
single-pixel structures remain representable.

This representation is not lossless for probability or confidence rasters
unless values are stored alongside indices. A future contract must therefore
either require binary input or define a separate sparse-value array.

## Authority And Editing

Persisted sparse indices would be a derived playback cache, not an edit target.
The authority rules remain:

```text
accepted edit
  -> update dense masks_roi
  -> mark sparse/bitpacked/RLE/contour products stale
  -> Palette validates and regenerates derived products
```

Sparse rows are variable length. Inserting or deleting foreground pixels can
shift later offsets in a compact flat stream, making them a poor live-edit
surface. Dense `masks_roi` remains simpler for training, validation, diffing,
and row/component writes.

## Comparison With Existing Compact Forms

Crimson already recognizes two lossless derived representations:

| Representation | Strength | Limitation |
| --- | --- | --- |
| `mask_bitpacked` | Fixed shape, predictable random access, 8x logical reduction | Requires bit unpacking and still stores every logical pixel |
| `mask_rle` | Compact for contiguous foreground regions and common interchange | Variable-length indirect reads can be latency-sensitive |
| proposed sparse indices | Direct match for Crimson's runtime foreground lists | Size depends on occupancy; variable-length edits are expensive |

With `uint32` indices, sparse storage is smaller than a one-bit bitmap while
foreground occupancy is below approximately 3.125 percent. Real occupancy must
be measured across complete production runs rather than inferred from a few
frames.

## Deferred Evaluation

Do not publish a maintained sparse-index schema until all of the following are
available:

1. Size and occupancy distributions for representative subject-body, swim
   bladder, and eye masks.
2. Cold and warm network-read benchmarks against dense, bitpacked, and RLE.
3. Sequential playback, seek, reverse-playback, and random-review results.
4. Palette provenance, regeneration, and stale-cache behavior.
5. Exact dense-to-sparse-to-dense round-trip tests.
6. Chunking guidance for `indptr` and `indices` that avoids excessive indirect
   reads on PRFS/SMB storage.
7. A clean separation between editable dense authority and derived playback
   products.

Until those gates pass, Crimson should restore the proven behavior of reading a
dense Zarr chunk once, converting it to sparse indices in memory, caching a
bounded number of converted chunks, and prefetching adjacent chunks.
