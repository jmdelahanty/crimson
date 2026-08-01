# Subject-Mask Full-Duration Canary

Date: 2026-08-01

Status: interoperability and read-behavior gate passed; production selection
and physical-profile promotion were not evaluated.

## Fixture

- Archive:
  `/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/subject_mask_storage/full_duration/sleepyfish_cam2010095_20260731_73f7bb5e/analysis.zarr`
- Recording root:
  `/Volumes/johnsonlab/jeremy/recordings/sleepyfish_2026_05_05_17_45_30_cam2010095`
- Refined-mask run:
  `refined_subject_masks_sleepyfish_subject_mask_full_duration_20260731_73f7bb5e`
- Crop run: `crop_sleepyfish_cam2010095_full_v8_20260730`
- Bundle run:
  `subject_mask_bundle_sleepyfish_subject_mask_full_duration_20260731_73f7bb5e`
- Bundle manifest payload digest:
  `cfcfd2297985bd097546d8642f02cea5d5556df0487dad0ad1d9a9bafea3ed0b`
- Refined-mask manifest payload digest:
  `9efe2d3e5865d495e40779ecb1be3fcc55953b02bd82ca20125f953c0c2aa78c`
- Palette worker/publisher/evidence commits: `73f7bb5e`, `58a010aa`,
  `979ed273`
- Crimson base commit embedded in benchmark results:
  `ad06b3b0872f2ce5255fc53a5659c79fa0a6a762`
- Crimson worktree: dirty while this consumer implementation was under review.

The fixture contains 1,188,000 camera frames, 1,169,010 observations, and four
refined mask components at 512 by 512 pixels. It is selector-ineligible.

## Bundle Contract

`subject_mask_bundle_v2_probe` validates the bundle without opening member
payload arrays. The mounted fixture passed these checks:

- exact bundle-v2 envelope and expected payload digest;
- direct/consolidated metadata equivalence;
- exact raw, refined, and quality member references and manifest digests;
- 39 exact array declarations and dtypes;
- independent raw and refined component registries;
- raw/refined identity hashes and refined/quality source bindings;
- selector-ineligible, activation-deferred publication state; and
- zero quality payload opens.

Supplying an incorrect expected bundle digest failed closed before member
validation with `Subject-mask bundle manifest digest differs`.

The maintained core-v2 member metadata scope is now accepted explicitly:

`exact_run_group_and_array_declarations_redacting_manifest_lifecycle_and_transport_publication_attrs`

The older supported scope remains accepted. No other scope is accepted.

## Crop Join

`subject_mask_crop_join_probe` opened only the compact placement/identity
columns and validated every refined-mask row against the explicit crop-v2 run.

| Check | Result |
| --- | ---: |
| Source crop row IDs read | 1,169,010 |
| Instance keys compared | 1,169,010 |
| Placements compared | 1,169,010 |
| Out-of-range row IDs | 0 |
| Instance-key mismatches | 0 |
| Placement mismatches | 0 |
| Elapsed time | 2,172.3 ms |

This proves the authoritative `source_crop_row_ids` join and exact
`source_crop_xywh` placement agreement. The probe never opens `masks_roi`.

## Five-Process Read Gate

The existing deterministic workload ran in five fresh processes. The workload
file SHA-256 is
`febf85d99f303ef8c116353a89eaca8d074fdee6571fd5f2c56a613a7596df0d`;
its canonical workload-document digest recorded by the harness is
`58d1e4d16d9c30787f2612010ed4c71aaf08f4fa5271ca853bed5f74019966e7`.

| Metric | Five-process result |
| --- | ---: |
| First-presentation readiness, median | 1,219.5 ms |
| Warm random-frame p95, median | 77.9 ms |
| Forward 70-frame page p95, median | 401.6 ms |
| Reverse 70-frame page p95, median | 413.2 ms |
| Post-warmup deadline-miss ratio, maximum | 0% |
| Rapid-seek final readiness, median | 220.9 ms |
| Stale visible frames | 0 |
| TensorStore file bytes, median | 21,198,711 bytes |
| Peak RSS, median | 388,251,648 bytes |
| Frame-offset reads per process | exactly 1 |
| Quality/derived payload reads | 0 |
| ROI-image open attempts | 0 |

The repository's `logical_chunk_source_bytes` counter was about 4.27 GB per
trial. This is cumulative decoded dense chunk input processed while converting
requested masks into sparse presentation data; it is not physical network
transfer. TensorStore's file metrics reported about 21.2 MB per process for the
deterministic workload.

The aggregate evidence is stored beside this document as `aggregate.json`.
Its SHA-256 is
`f1137d87065d26d7c78781af81773563fd5ad90b1e6b3698a4fcfe615f3c00d4`.

## GUI Smoke

A Metal/AVFoundation smoke used clip 0 from `recording_clip_index.json`, parent
frames 1000 through 1100, and the explicit selector-ineligible refined-mask
run. It passed with:

- presented frame 1100;
- 106 decoded video frames and no skipped or late presentations;
- zero presentation lag;
- 112 resolved subject-mask requests with no missing or failed results;
- zero stale visible overlays;
- maximum mask service time 64.3 ms; and
- peak process memory about 473.7 MiB.

The recording index itself contains 22 readable, contiguous 54,000-frame clips
covering parent frames `[0, 1188000)`. Crimson's maintained clipped-collection
player is driven by mappings stored in an analysis archive; it does not yet
adapt this external `recording_clip_index.json` directly. Therefore this gate
proves full-axis mask access and one source-matched visual segment, but not
automatic GUI playback across all 22 clip boundaries. That remaining work is a
media-provider integration task, not a subject-mask storage failure.

## Verdict

The full-duration refined-mask bundle is accepted for Crimson read-only
interoperability. Exact schema handling, component binding, retained offsets,
crop placement, paging, cancellation, stale-publication prevention, and lazy
quality behavior passed. No production selector, authority, fixture, or
Palette state was changed.

The result does not promote a physical storage profile and does not make the
selector-ineligible bundle authoritative.
