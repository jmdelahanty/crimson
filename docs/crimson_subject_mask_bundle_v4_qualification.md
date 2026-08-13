# Subject-Mask Bundle V4 Qualification

Date: 2026-08-12

Status: exact consumer and performance gate implemented; production selection
remains unchanged.

## Scope

The bundle-v4 gate validates Palette's receipt-composed four-member subject-mask
publication without resolving `latest`, opening a compatibility adapter, or
changing archive state. It accepts an explicitly named selector-ineligible
bundle only when the bundle, raw, refined, quality, and sampled-contour
manifests agree exactly with their direct and consolidated Zarr declarations.

Supported modern manifests are deliberately narrow:

- subject-mask bundle manifest v4;
- raw and refined core manifests v5;
- quality manifest v3; and
- derived sampled-contour cache manifest v3.

The production-path import gate also accepts Palette's immediately preceding,
strictly versioned envelope as one indivisible combination:

- subject-mask bundle manifest v3;
- raw and refined core manifests v5;
- quality manifest v2; and
- derived sampled-contour cache manifest v2.

That branch validates the whole-array SHA-256 bindings carried consistently by
the quality, cache, and bundle manifests. It does not reinterpret them as v4
composable receipts, mix member versions, or weaken the v4 checks.

Legacy bundle-v2, core-v2, and cache-v1 support remains a separate accepted
contract. Modern validation does not add aliases or dtype probing to either
path.

## Correctness Boundary

`subject_mask_bundle_v4_probe` checks the exact bundle digest, all four member
bindings, schemas, shapes, dtypes, storage plans, codec chains, coordinate
catalogs, composable row-unit identities, complete receipts, and the absence of
ordinary selector references. An explicit nonexistent bundle or wrong digest
must fail; no fallback is permitted.

Its structured output records the observed bundle manifest schema version so
qualification evidence distinguishes an imported v3 envelope from v4.

`subject_mask_crop_join_probe` reads the compact refined identity and placement
columns and compares every `source_crop_row_ids` join against the crop run. The
dense `masks_roi` tensor remains the scientific/edit authority but is not read
by this join.

Ordinary presentation opens the sampled-contour cache and leaves all quality
payload arrays, dense masks, full ragged contours, and `source_point_count`
closed. A separate dense mode exercises the authority explicitly. Both modes
read and retain `frame_row_offsets` exactly once.

The mounted Sleepyfish recording contains empty and single-row frames but no
multi-row frames. Its first known empty frame and all 22 clip boundaries are in
the frozen workload. The portable benchmark self-test separately enforces the
`[2, 0, 1, 3]` frame-row pattern so complete multi-observation ranges remain a
consumer requirement.

## Reproducible Gate

Build the required targets:

```bash
cmake --build build/macos-arm64-release --target \
  subject_mask_bundle_v4_probe \
  subject_mask_crop_join_probe \
  subject_mask_v1_long_duration_benchmark
```

Run five alternating fresh processes for sampled contours and dense masks:

```bash
scripts/run_macos_subject_mask_bundle_v4_qualification.sh
```

For a digest-bound candidate imported into a production archive, use the thin
production wrapper instead. It verifies Palette's import/validation/lineage
receipts and supplies the production bundle/member digests to the same harness:

```bash
scripts/run_macos_subject_mask_bundle_v4_production_gate.sh
```

The frozen workload is
`tools/fixtures/subject_mask_bundle_v4_qualification_workload_v1.json`. It
includes 64 deterministic random frames, explicit clip-boundary and empty-frame
reads, forward and reverse 70-frame pages, and 32 rapid seeks with cancellation.

Every trial must satisfy:

- warm random-frame p95 at or below 150 ms;
- zero post-warmup page deadline misses;
- exactly one offset read;
- zero stale visible frames;
- zero derived/quality payload reads; and
- peak RSS at or below 2 GiB.

The aggregate also records readiness, page latency, current-frame queue delay,
file reads and bytes, cancellation, cache behavior, and RSS. Passing approves
the physical profile only for one digest-bound, selector-ineligible
production-path candidate. It never authorizes production activation.

The output directory contains the bundle and crop probes, both negative
fail-closed checks, environment identity, ten raw trial JSON files,
`aggregate.json`, and `aggregate.sha256`. macOS and mounted-network cache state
are recorded as uncontrolled rather than described as cold.
