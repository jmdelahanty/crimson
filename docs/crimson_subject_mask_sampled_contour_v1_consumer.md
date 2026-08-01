# Subject-Mask Sampled-Contour V1 Consumer

Date: 2026-08-01

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

Telemetry explicitly reports:

- source offset reads and retained bytes;
- dense-mask payload reads;
- sampled-contour payload reads and logical bytes;
- `source_point_count` open attempts and payload reads;
- demand, prefetch, cache-hit, eviction, cancellation, and stale-result counts;
- file reads, transferred bytes, latency percentiles, deadline misses, and RSS.

## 2026-08-01 Mounted Checkpoint

One fresh-process Mac/VPN trial passed before the implementation checkpoint:

- first presentation readiness: 2175.95 ms;
- warm random-frame p95: 176.25 ms;
- forward/reverse 70-frame page p95: 108.17 / 56.47 ms;
- post-warmup deadline misses: 0;
- rapid-seek final readiness: 153.16 ms;
- stale visible frames: 0;
- peak RSS: 346,390,528 bytes;
- process file bytes: 80,269,151;
- source offset reads: 1;
- dense-mask reads: 0;
- `source_point_count` opens/reads: 0 / 0.

The Metal view was visually accepted against the matching 22-clip recording
index. This checkpoint establishes interoperability and consumer behavior; it
does not select the cache in production or replace dense-mask authority.
