# Subject-Mask Sampled-Contour Canary

Date: 2026-08-01

Status: consumer integration pass; one revision-bound mounted process plus
human Metal visual acceptance. This is not a storage-profile promotion or a
production-selector decision.

## Provenance

- Crimson implementation commit:
  `c819854abf5b0f0c602b9510e360e2f57f0a9d96`
- Crimson worktree during the recorded trial: clean
- Palette implementation commit: `90829491`
- Palette evidence/native-thread commit: `483367c2`
- Cache manifest payload digest:
  `c04a3f9283da0bd9bd16497b7ecc9ca9871c816ad517b1e5d40b092bde8c6861`
- Source manifest payload digest:
  `9efe2d3e5865d495e40779ecb1be3fcc55953b02bd82ca20125f953c0c2aa78c`
- Recorded trial SHA-256:
  `c473c1fe27bfbf53e490cad8a5cf20a9baa18714c7394427ec3cdb8908b36190`
- Aggregate SHA-256:
  `283708b555fa8e2c3ae38bd30b044dbcc41f1ef5ef2659bb3ee4aac3ab0254e2`
- Metal acceptance screenshot SHA-256:
  `48975dfd0de0047071ed362948a5e93d17eb4c2e85c82de2bc760bfe8f5a6423`

## Fixture

Source archive:

```text
/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/subject_mask_storage/full_duration/sleepyfish_cam2010095_20260731_73f7bb5e/analysis.zarr
```

Source run:

```text
refined_subject_masks_runs/refined_subject_masks_sleepyfish_subject_mask_full_duration_20260731_73f7bb5e
```

Cache archive:

```text
/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/subject_mask_storage/sampled_contours/sleepyfish_cam2010095_sampled_contours_20260801_90829491/cache.zarr
```

Cache run:

```text
subject_mask_cache_runs/subject_mask_sampled_contours_sleepyfish_20260801_90829491
```

The source contains 1,169,010 rows over 1,188,000 camera frames. The cache has
12 exact arrays and four fixed sampled-contour components.

## Contract Gate

The mounted contract gate accepted the untouched source/cache pair and rejected
all recomputed adversarial candidates:

- selector-eligible cache;
- wrong dense source authority;
- wrong dense source run;
- reordered components;
- changed fixed sample count;
- changed `source_point_count` dtype; and
- missing full-dense-equivalence receipt.

Direct/consolidated metadata equivalence, exact physical declarations, source
binding, receipt binding, and full logical inventory validation passed.

## Workload Result

The recorded fresh process used the frozen subject-mask workload: 64 random
frames in two passes, 700-frame forward and reverse traversals in 70-frame
pages, and 32 rapid seeks.

- First presentation readiness: 2200.37 ms
- Warm random-frame p95: 217.60 ms
- Forward 70-frame page p95: 105.03 ms
- Reverse 70-frame page p95: 50.73 ms
- Post-warmup deadline miss ratio: 0
- Rapid-seek final readiness: 163.28 ms
- Stale visible frames: 0
- Peak RSS: 347,045,888 bytes
- Process file bytes: 80,737,125
- Source offset reads: 1
- Dense-mask payload reads: 0
- Sampled-contour payload reads: 3264
- `source_point_count` open attempts/payload reads: 0 / 0
- Chunk load failures: 0

The complete structured result is in `aggregate.json`. TensorStore file metrics
are process-level physical I/O counters; repository contour byte metrics are
decoded logical payload bytes and are labeled separately.

## Visual Gate

Crimson opened the matching 22-clip recording through
`recording_clip_index.json`, presented frame 1000 through Metal, and displayed
the sampled body, eye, and swim-bladder contours in source-camera placement.
The user visually accepted the result.

## Remaining Scope

The five-repetition default harness remains available for a later formal
distribution. No additional run is necessary for this interoperability
checkpoint. Production selection remains blocked on Palette's broader
subject-mask lifecycle/promotion decision; dense `masks_roi` remains the
editing and scientific authority regardless of future cache selection.
