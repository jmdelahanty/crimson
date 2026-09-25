# Subject-Mask Sampled-Contour Canary

Date: 2026-08-03

Status: consumer integration pass; five revision-bound mounted processes plus
human Metal visual acceptance. This is not a storage-profile promotion or a
production-selector decision.

## Provenance

- Crimson cache-consumer commit:
  `c819854abf5b0f0c602b9510e360e2f57f0a9d96`
- Crimson concurrent-read commit:
  `71ae4f86dd3e2c6b344a8eab0f30d48e65fd56a9`
- Crimson demand-preemption/final evidence commit:
  `f0d8bb23c8e5dad8eb6c6e626d8241f9bb485652`
- Crimson worktree during every recorded trial: clean
- Palette implementation commit: `90829491`
- Palette evidence/native-thread commit: `483367c2`
- Cache manifest payload digest:
  `c04a3f9283da0bd9bd16497b7ecc9ca9871c816ad517b1e5d40b092bde8c6861`
- Source manifest payload digest:
  `9efe2d3e5865d495e40779ecb1be3fcc55953b02bd82ca20125f953c0c2aa78c`
- Serialized baseline aggregate SHA-256:
  `8b99bdbf7fa92aacd3563da8960cb2cec076bb40309587f6d7a8c6c8b6600325`
- Final aggregate SHA-256:
  `1f943c287be74d9a460c4f8152daf8450f7b0fcc88fc4c330b0f11ac6c486229`
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

Each of five fresh processes used the frozen subject-mask workload: 64 random
frames in two passes, 700-frame forward and reverse traversals in 70-frame
pages, and 32 rapid seeks. All five optimized trials passed every frozen gate.

- Median first presentation readiness: 2931.79 ms
- Median warm random-frame p95: 185.02 ms
- Median forward 70-frame page p95: 154.98 ms
- Median reverse 70-frame page p95: 214.77 ms
- Maximum current-frame queue wait across trials: 98.86 ms
- Current-frame service average range: 125.88-148.30 ms
- Post-warmup deadline miss ratio: 0
- Median rapid-seek final readiness: 271.93 ms
- Stale visible frames: 0
- Median peak RSS: 338,034,688 bytes
- Median process file bytes: 86,240,221
- Source offset reads: 1
- Dense-mask payload reads: 0
- `source_point_count` open attempts/payload reads: 0 / 0
- Chunk load failures: 0

The clean serialized baseline at `c819854` failed the warm-random and queue
gates. Its medians were 5485.33 ms first readiness, 522.94 ms warm random p95,
435.71/448.51 ms forward/reverse page p95, and 523.04 ms rapid-seek readiness.
Median physical transfer was 86,534,627 bytes. Issuing each component's exact
TensorStore reads concurrently and allowing current-frame demand to preempt
lower-priority same-source work reduced latency without materially changing
transfer.

The complete optimized result is in `aggregate.json`; the comparison input is
in `serialized_baseline_aggregate.json`. TensorStore file metrics are
process-level physical I/O counters; repository contour byte metrics are
decoded logical payload bytes and are labeled separately.

## Visual Gate

Crimson opened the matching 22-clip recording through
`recording_clip_index.json`, presented frame 1000 through Metal, and displayed
the sampled body, eye, and swim-bladder contours in source-camera placement.
The user visually accepted the result.

## Remaining Scope

No additional consumer performance run is necessary for this interoperability
checkpoint. Production selection remains blocked on Palette's broader
subject-mask lifecycle/promotion decision; dense `masks_roi` remains the
editing and scientific authority regardless of future cache selection.
