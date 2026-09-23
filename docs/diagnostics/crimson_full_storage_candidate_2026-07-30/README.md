# Crimson Full Storage Candidate Gate

Date: 2026-07-30

Status: ACCEPTED; canonical v3 companion and source-matched GUI smoke passed

## Fixture

Crimson validated Palette's immutable, selector-ineligible Sleepyfish v8
handoff at:

`/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/crimson_storage_candidates/sleepyfish_cam2010095_full_v8_20260730/`

The handoff file SHA-256 is
`86e638ed977f71a69bdd4fd334d7778583226901e7acfc22f790607e8b6fc374`;
its independently recomputed canonical payload digest is
`1089386cce3e94ab368203c3e9e70ebb0fee956860405a37f33a7d35dd4c5ec0`.
Palette produced the package from commit
`8fd810fd8f5ba5c7bc9dcc000ee9b4b90b4af342`.

## Accepted Surfaces

The refined-detection gate accepted the complete 1,188,000-frame,
1,169,010-row snapshot:

- all 38 exact clipped-snapshot declarations validated;
- only 11 presentation handles opened;
- all source-audit payload handles remained lazy;
- `frame_row_offsets` was read once and retained in 9,504,008 bytes;
- all rows, instance keys, refined row IDs, source rows, and overlay boxes were
  preserved during complete traversal;
- 18,990 empty frames were routed correctly;
- rapid seeks produced zero stale publications;
- the 59,619,510-byte resident UI snapshot published exactly once; and
- a resident probe performed no additional TensorStore UI-field read.

The refined subgate took 127.7 ms to open, 352.5 ms for the seek burst,
3.75 seconds for complete traversal, and 532.8 ms to build resident UI columns
in the clean-provenance process. This fixture has no manual or multi-instance
frames; the deterministic `[2,0,1,3]` contract tests remain the evidence for
those cases.

Five fresh crop-geometry processes also passed. Median process-to-first-frame
readiness was 570 ms, warm random-frame p95 was 0.108 ms, 70-frame sequential
page p95 was 0.085 ms, cancellation p95 was 0.136 ms, process file transfer was
73.04 MiB, and peak RSS was 225.56 MiB. Every process used the exact 13-array
schema, retained offsets after one read, opened no pixel arrays, published no
stale result, and validated all 1,169,010 rows.

The raw keypoint, lazy quality, refined keypoint, and refined-derived body-frame
stores passed their separate five-process full-duration gate. See
`../keypoint_v2_full_duration_experiment_2026-07-30/`.

## Canonical V3 Closure

The original package-level result was not a complete pass because
`canonical_detection.zarr` publishes
`palette.canonical_detection.run_manifest` version 2. Crimson's strict shared
coordinate-aware canonical adapter requires version 3. The adapter therefore
failed closed with:

`Canonical detection run_manifest envelope is invalid`

Crimson did not reinterpret v2 as v3 or weaken the coordinate contract. Palette
closed that blocker with the logically identical selector-ineligible v3
companion at:

`/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/crimson_storage_candidates/sleepyfish_cam2010095_full_v8_canonical_v3_20260730_v1/`

The companion handoff SHA-256 is
`5913c8437522a2cf28ea7a2356e1760b7be9b489ee4ad1b4abc2d326a0718c38`.
The headless gate accepted all 1,186,376 rows, exact schema opens, one retained
offset read, coordinate-catalog validation, rapid-seek cancellation, and the
unchanged refined companion. Its structured result is `canonical_v3_result.json`.

The first source-matched GUI run exposed and then closed a Crimson consumer
bug: canonical manifest validation incorrectly advertised the complete refined
identity-column set. The canonical repository deliberately opens only the
three frozen raw UI columns, so background residency rejected the otherwise
valid chunk. Commit `8ef7e86` separates manifest/coordinate validation from
the refined-only identity presentation flag.

The clean revision-bound rerun at Crimson commit
`49f7907d1389dcf5669048ad4c44093254c44a71` passed against the matching
4512x4512, 1,188,000-frame video. Canonical residency loaded all 55 chunks,
atomically published 28,473,024 bytes once in 616.4 ms, and reported no failed
or stale chunks. The smoke advanced from frame 0 through 300 with 601 detection
presentations, no failed pages, no skipped source frames, no late
presentations, and zero maximum lag. Structured GUI evidence is in
`canonical_v3_gui_smoke.json`.

## Evidence Limits

Fresh process means a new Crimson process; macOS, SMB, VPN, and server caches
were not evicted or controlled. The first two crop repetitions paid markedly
higher metadata latency than later repetitions. The measurements are an honest
process-first distribution, not a controlled cold-cache claim.

Only one physical layout was supplied for crop and keypoints. These results
accept consumer behavior but do not compare or promote storage profiles. No
selector, registry, source archive, or production state was changed.

## Cross-Platform Check

The exact pushed revision `e6eecb054e8fc10468b887bd3728e5ef2fbf234d`
was transferred as an immutable Git bundle to an isolated ws1 clone. Linux
successfully built `refined_detection_shadow_gate`,
`crop_geometry_v2_read_benchmark`, `keypoint_v2_contract_tests`, and
`canonical_detection_repository_tests` against CUDA 12.4 and the existing
TensorStore build. The keypoint, canonical-detection, and crop self-tests all
passed. The shared dirty Linux checkout was not changed.

After the canonical-v3 residency classification fix, the isolated checkout was
advanced through an immutable Git bundle to
`49f7907d1389dcf5669048ad4c44093254c44a71`. Linux rebuilt and passed
`canonical_detection_repository_tests` again. The shared ws1 checkout remained
untouched.

Primary evidence SHA-256 values:

- `crop_geometry_aggregate.json`:
  `e263b378fbcf29789db7d08ef6d73c12f27f472754fd62fff09ec971b17cc133`;
- `crop_geometry_fresh_process_summary.png`:
  `a284cd0a529c22c79e6cfe37423b57102bd0a7f5033fbd2192954bb17954eaf8`;
- `refined_detection_result.json`:
  `efe4800b2f315e899cecd6e7c9fb24c804cefb859a6f587c4e3e1d7699a42668`;
- `canonical_v3_result.json`:
  `d6cf8e9c793d06c15de94cf6156572d57a61991a4b13530e6c8073ea0811c2de`;
- `canonical_v3_gui_smoke.json`:
  `1718d534993c40eb92440fccb28485fe32400db225f373ef8bfdea7620c2361f`.
