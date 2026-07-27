# Crimson Subject-Mask Editable Storage Decision

Decision timestamp: 2026-07-08

## Decision

Training Zarr datasets store subject-mask labels only as dense binary
`masks_roi` arrays for now.

Training datasets should not store compact subject-mask label encodings such as
`mask_bitpacked` or `mask_rle`. Those formats may exist in analysis or
publication archives as derived playback caches, but they are not training
surfaces and are not edit targets.

Editable analysis stores should also keep dense `masks_roi` present and treat it
as the authoritative mask-pixel surface. Crimson live edits should read and
write dense ROI-local component masks. Palette remains responsible for
validating saves, updating row revisions, and regenerating derived outputs.

## Storage Roles

| Store or surface | Role | Recommended mask storage |
| --- | --- | --- |
| Training Zarr | Model training source of truth | Dense `masks_roi` only |
| Editable analysis Zarr | Live Crimson review/edit authority | Dense `masks_roi` required |
| Publication/playback analysis Zarr | Read-heavy review/distribution | Dense optional; `mask_bitpacked` allowed as derived cache |
| `mask_rle` | Compact fallback/archive/interchange | Derived only, not an edit target |
| Component contours/metrics | Derived geometry and measurements | Recompute from dense masks after accepted edits |

## Dense Chunking Recommendation

For editable dense `masks_roi`, prefer component-separated row chunks:

```text
masks_roi chunks = [128, 1, 512, 512]
```

`[64, 1, 512, 512]` is also reasonable when lower write amplification is more
important than file count. Avoid `[512, 4, 512, 512]` for editable stores.

The component axis of `1` is the important part. A human edit usually changes
one ROI row and one semantic component, for example `eye_left` or
`subject_body`. Component-separated chunks avoid rewriting unrelated labels when
one component changes.

For read-only playback caches, the tested bitpacked shape remains a good
candidate:

```text
mask_bitpacked/masks_packed chunks = [512, 4, 512, 64]
```

That compact shape is appropriate because playback commonly loads all
components for a row window, then Crimson decides which components to draw.

## Strategy Rationale

This policy separates authority from acceleration.

Dense `masks_roi` is the authority because it is the simplest representation to
edit, validate, diff, review, and train from. Compact mask representations and
component contours are acceleration or convenience products derived from the
dense authority. They can be regenerated, invalidated, or omitted without
changing the label truth.

The intended invariant is:

```text
accepted user edit -> update dense masks_roi -> mark derived arrays stale
```

This avoids multiple competing truths inside one Zarr. If dense masks, RLE,
bitpacked masks, contours, and metrics disagree after an edit, consumers should
trust dense `masks_roi` and treat the other arrays as stale until Palette
regenerates them.

A possible CSR-style sparse-index playback cache is recorded as a deferred
concept in `docs/crimson_subject_mask_sparse_index_storage_concept.md`. It does
not change the dense-authority decision in this document.

## What Gets Rewritten On Edit?

Zarr arrays are chunked. A partial write affects only the chunks intersecting the
write region, not the whole array. However, when the write modifies only part of
a compressed chunk, the storage layer generally has to perform a read-modify-
write of the whole affected chunk. The rewritten unit is the compressed chunk
object or file.

The reason Crimson/Palette cannot usually rewrite just one ROI row inside a
larger chunk is that the row is not stored as an independent object. A chunk is
encoded as one compressed payload. To change bytes in the middle of that
payload, the writer typically has to:

1. read the existing chunk;
2. decompress/decode it;
3. replace the edited row/component plane in memory;
4. recompress/re-encode the chunk;
5. write the chunk object back.

The only way to make a single edited row become the chunk write unit is to use a
row chunk of `1`, for example:

```text
masks_roi chunks = [1, 1, 512, 512]
```

That minimizes per-edit write amplification, but it creates one chunk per
row/component. For a 120,000-row run with 4 components, that is about 480,000
mask chunks before considering other arrays and metadata. That file/object count
is usually worse for network filesystems and archive browsing than rewriting a
moderately larger chunk on save.

For a single dense subject-mask edit that writes one ROI row and one component:

```text
masks_roi[row, component, :, :]
```

the affected dense mask payload is one chunk when the chunk shape is component
separated. The logical uncompressed chunk sizes are:

| Dense chunk shape | Logical bytes per affected chunk | Editable consequence |
| --- | ---: | --- |
| `[1, 1, 512, 512]` | 256 KiB | Lowest edit write amplification, impractical chunk/file count |
| `[64, 1, 512, 512]` | 16 MiB | Lower write amplification, more chunk files |
| `[128, 1, 512, 512]` | 32 MiB | Recommended balance |
| `[512, 1, 512, 512]` | 128 MiB | Better read batching, heavier saves |
| `[512, 4, 512, 512]` | 512 MiB | Poor editable default; rewrites unrelated components |

Compression may make the physical file much smaller than these logical sizes,
but the CPU and storage work is still tied to the chunk as the update unit.

If a user edits multiple adjacent rows in the same chunk, the write cost can be
amortized. If they edit scattered rows, each touched chunk may need its own
rewrite.

## Dense Versus Compact Edit Targets

Dense `masks_roi` is the edit target because:

- it has fixed shape and simple row/component addressing;
- no-op detection is a bytewise comparison;
- training loaders can consume it directly;
- Palette can regenerate all derived caches from it.

`mask_bitpacked` is not a good primary edit target even though it is compact.
It still rewrites chunks, it adds bit packing/unpacking logic to the write path,
and it is harder to use as a direct training surface.

`mask_rle` is worse for live edits. A small pixel edit can change many run
lengths, and row payload sizes are variable. It is useful as a compact derived
format, but not as the authoritative editable representation.

## Contours And Derived In-Zarr Arrays

Component contours are derived from dense masks and should not be edited as
authoritative data. They are stored inside the same Zarr run, linked by
component and mask row, but they are still derived companion arrays rather than
the source label surface.

Historical and analysis-oriented contours use a variable-length layout:

```text
components/<component>/contours/ptr
components/<component>/contours/len
components/<component>/contours/points_xy
```

For default display reads, Crimson now prefers Palette's fixed-K derived cache:

```text
components/<component>/sampled_contours/points_xy
components/<component>/sampled_contours/valid
components/<component>/sampled_contours/source_point_count
```

It falls back to full ragged contours for historical runs and ignores both
stored contour representations when `contours_stale=true`. See
`docs/crimson_sampled_subject_mask_contour_reader_2026-07-10.md`.

Editing one mask row can change that row's contour length. If `points_xy` is a
compact flat stream and `ptr` stores cumulative offsets, then changing one row's
point count can shift offsets for later rows. That means an in-place contour
update is not necessarily row-local:

- `len[row]` changes for the edited row;
- `points_xy` changes for the edited row's contour points;
- `ptr` values after the edited row may need to change if the flat stream is
  compacted;
- the touched `ptr`, `len`, and `points_xy` chunks may all be rewritten.

For this reason, Palette should treat contours as derived caches:

1. accept and validate the dense mask edit;
2. update the dense mask chunk and row revision;
3. mark contour caches and other derived arrays stale;
4. regenerate full contour arrays during promotion, validation, or background
   maintenance.

For training Zarrs, the simplest policy is to make dense masks sufficient. If
training datasets later include contours, metrics, bitpacked masks, or RLE for
diagnostics or convenience, those outputs should be regenerated from dense masks
and validated as derived data.

## Crimson Writeback Implications

Crimson should:

- enable live subject-mask edits only when dense `masks_roi` is available;
- keep compact-only runs display-only;
- write one ROI row and one component per accepted edit request;
- refresh the touched row/component after a successful save;
- expect Palette to return row revision and stale/updated derived-cache status;
- not promote compact or contour-derived data into training datasets.

Palette should:

- own the save transaction and validation;
- reject stale row revisions or incompatible component labels;
- update dense `masks_roi` as the authority;
- mark contours, metrics, bitpacked masks, and RLE stale after accepted edits;
- regenerate stale derived arrays later during background maintenance,
  validation, or promotion;
- promote training datasets from reviewed dense masks only.

## Publication Archives

Read-only publication archives may include multiple mask representations when
the measured storage cost is small enough. Dense masks are useful because they
are simple, editable if the archive is copied back into a review workflow, and
directly compatible with training promotion. Bitpacked masks are useful because
they reduce chunk/file pressure for network playback. RLE remains useful as a
compact interchange/archive representation.

For now, do not force publication archives to be compact-only. Prefer keeping
dense `masks_roi` alongside compact caches unless a measured file-count,
transfer, or storage limit requires dropping dense masks from a specific
publication target.

## Open Questions

1. Should editable analysis stores use `[64, 1, 512, 512]` or
   `[128, 1, 512, 512]` by default?
2. For read-only publication archives, what measured storage, transfer, or
   file-count threshold should justify dropping dense `masks_roi` and publishing
   compact masks only?
