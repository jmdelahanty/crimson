# Subject-Mask Sampled-Contour V1 Consumer

Date: 2026-08-03

Status: selector-ineligible full-duration integration accepted; production
selection and storage-profile promotion remain unchanged.

## Authority Boundary

`refined_subject_masks_runs/.../masks_roi` remains the editable and scientific
pixel authority. The sampled-contour cache is a derived, immutable display
surface. Crimson validates its exact source binding before opening any contour
payload and never treats it as an editing source.

The strict presentation path is:

1. Open and validate the explicit refined subject-mask v1 source manifest.
2. Read and retain the source `frame_row_offsets` exactly once.
3. Validate the explicit sampled-contour manifest against that source manifest,
   including row identity, component registry, dense-mask digest, receipts, and
   full-dense-equivalence evidence.
4. Validate direct and consolidated Zarr v3 metadata for all 12 cache arrays.
5. Open only each component's `valid` and `points_xy` arrays.
6. Resolve complete frame row ranges through the retained source offsets and
   preserve every observation.

`source_point_count` remains closed and unread during ordinary presentation.
Dense `masks_roi` is also unopened by the contour-only payload path. A required
contour read failure rejects the frame rather than silently displaying an empty
overlay.

## Physical Contract

The reader accepts exactly four components in persisted order:

- `subject_body`: 128 points
- `eye_left`: 64 points
- `eye_right`: 64 points
- `swim_bladder`: 32 points

The arrays use exact `float32`, `bool`, and `int32` declarations with Zarr v3
indexed sharding, 128 KiB inner chunks, at most 8 MiB outer shards, Zstd level
0 payload compression, and a CRC32C shard index stored at the end. Legacy mask
layouts continue through the existing compatibility adapter; the strict cache
reader does not add aliases or dtype probes.

## App Integration

The macOS asynchronous analysis loader accepts an external cache archive. The
benchmark-only CLI requires both the strict source and cache identities:

```bash
--benchmark-subject-mask-v1 SOURCE_RUN SOURCE_MANIFEST_DIGEST
--benchmark-subject-mask-presentation-cache-v1 \
  CACHE.zarr CACHE_RUN CACHE_MANIFEST_DIGEST
```

Normal archive discovery is unchanged. Use
`scripts/launch_macos_subject_mask_sampled_contour_demo.sh` for the frozen
Sleepyfish clip-index visual gate.

## Reproducible Gates

`subject_mask_sampled_contour_v1_contract_gate` validates the mounted source
and cache, then proves that recomputed semantic mutations fail closed. Covered
mutations include selector eligibility, source authority/run binding, component
ordering, fixed sample counts, `source_point_count` dtype, and the full dense
equivalence receipt.

`scripts/run_macos_subject_mask_sampled_contour_gate.sh` reuses Crimson's
existing subject-mask workload for random frames, 70-frame forward and reverse
pages, rapid seeks, cancellation, stale-publication prevention, physical file
telemetry, TensorStore cache telemetry, and peak RSS. Set
`CRIMSON_SUBJECT_MASK_CONTOUR_REPETITIONS=1` for a quick mounted checkpoint;
the default follows the frozen five-repetition workload.

The chunk loader issues every component's exact `points_xy` and `valid` read
as a TensorStore future, forces all reads before waiting, and then decodes the
completed arrays into the same immutable presentation chunk. Current-frame
demand also cancels and overtakes lower-priority same-source scheduler work;
current/current source isolation remains intact.

Telemetry explicitly reports:

- source offset reads and retained bytes;
- dense-mask payload reads;
- sampled-contour payload reads and logical bytes;
- `source_point_count` open attempts and payload reads;
- demand, prefetch, cache-hit, eviction, cancellation, and stale-result counts;
- file reads, transferred bytes, latency percentiles, deadline misses, and RSS.

## 2026-08-03 Full-Duration Mounted Checkpoint

Five fresh-process Mac/VPN trials passed from clean commit
`f0d8bb23c8e5dad8eb6c6e626d8241f9bb485652`:

- median first presentation readiness: 2931.79 ms;
- median warm random-frame p95: 185.02 ms;
- median forward/reverse 70-frame page p95: 154.98 / 214.77 ms;
- maximum current-frame queue wait: 98.86 ms;
- post-warmup deadline misses: 0;
- median rapid-seek final readiness: 271.93 ms;
- stale visible frames: 0;
- median peak RSS: 338,034,688 bytes;
- median process file bytes: 86,240,221;
- source offset reads: 1;
- dense-mask reads: 0;
- `source_point_count` opens/reads: 0 / 0.

The final aggregate SHA-256 is
`1f943c287be74d9a460c4f8152daf8450f7b0fcc88fc4c330b0f11ac6c486229`.
All five trials passed every frozen gate.

The Metal view was visually accepted against the matching 22-clip recording
index. This checkpoint establishes interoperability and consumer behavior; it
does not select the cache in production or replace dense-mask authority.
