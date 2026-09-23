# Crimson Keypoint V2 Long-Duration Benchmark

Date: 2026-07-30

Status: deterministic harness and fresh-process reducer implemented; mounted
integration and full-duration refined-keypoint fixtures pass

## Purpose

`keypoint_v2_long_duration_benchmark` exercises the same backend-neutral
repository, shared scheduler, and presentation cache used by Crimson playback.
It does not select a production run and does not write to an archive. Its input
stores and run-manifest digests are always explicit.

The frozen workload is
`tools/fixtures/keypoint_v2_long_duration_workload_v1.json`. Every result binds
itself to the canonical JSON SHA-256 of that file, the Crimson revision, input
manifest digest, mode, and repetition.

## Workload

Each fresh process performs:

- exact consolidated/direct metadata validation and exact typed opens;
- one retained read of each required frame-offset index;
- first-frame presentation;
- 128 deterministic random frames followed by the same warm pass;
- 3,500-frame forward and reverse traversals in 70-frame pages;
- 32 rapid discontinuous seeks with stale-publication validation; and
- bounded close and scheduler shutdown.

The scheduler uses four workers, at most one speculative worker, and one
reserved current-frame worker. The presentation buffer uses 12-frame
directional lookahead and a 24-frame cache. Forward playback prefetches later
frames; reverse playback prefetches earlier frames. A normal one-frame reverse
step is not treated as a seek or generation reset.

The workload models 700 FPS: each 70-frame page has a 100 ms deadline after
one warmup page. It also gates warm random-frame p95, current-frame queue wait,
first-presentation readiness, rapid-seek final readiness, close latency, stale
publication, read failures, quality-array laziness, and peak RSS.

## Metrics

The JSON result separates:

- archive-context construction from repository open;
- metadata, exact-handle, and identity-validation time;
- current-frame queue wait from repository service time;
- first, random, forward, reverse, seek, and process-wide file reads and bytes;
- TensorStore cache hits, misses, and evictions;
- retained offset bytes and exact offset-read counts;
- presentation cache, cancellation, discard, and failure counters; and
- process peak RSS.

File metrics are TensorStore process counters for the mounted `file` kvstore.
Rapid-seek bytes are deliberately labeled an upper bound: they include the
final useful request and its bounded lookahead, not only superseded work.

`ArchiveContext` currently owns a 64 MiB TensorStore cache pool. If raw,
quality, refined, and body-frame products are separate Zarr archives, each
unique archive has a separate configured pool. The result reports both every
archive pool and their aggregate configured limit. This limit is not resident
allocation; RSS and physical cache counters show actual process behavior.

## Commands

Build and run the portable contract self-test:

```bash
cmake --build build/macos-arm64-release \
  --target keypoint_v2_long_duration_benchmark
ctest --test-dir build/macos-arm64-release \
  -R '^keypoint_v2_long_duration_benchmark_self_test$' \
  --output-on-failure
```

Run one fresh raw trial:

```bash
build/macos-arm64-release/keypoint_v2_long_duration_benchmark \
  --mode raw \
  --raw-store RAW.zarr --raw-run RAW_RUN --raw-digest RAW_SHA256 \
  --quality-store QUALITY.zarr --quality-run QUALITY_RUN \
  --quality-digest QUALITY_SHA256 \
  --body-store BODY.zarr --body-run BODY_RUN --body-digest BODY_SHA256 \
  --workload tools/fixtures/keypoint_v2_long_duration_workload_v1.json \
  --repetition 0 --output raw-repetition-0.json
```

For refined mode, add the three `--refined-*` arguments and use the body-frame
snapshot derived from that refined run. Run repetitions zero through four in
separate fresh processes. `--deep-validate-identity` is an optional audit; the
production-shaped benchmark leaves full identity scans disabled because every
presented page is still identity-validated.

## Fresh-Process Experiment

`tools/run_keypoint_v2_long_duration_benchmark.py` owns process isolation,
candidate order, result validation, aggregation, and plotting. Its versioned
experiment manifest declares exact artifact paths, run names, manifest and
handoff digests, the workload, repetition count, expected Crimson revision,
and any physical-candidate comparisons. Candidate order rotates cyclically so
each candidate occupies every ordinal position equally when the repetition
count is a multiple of the candidate count.

Run the mounted integration exercise with:

```bash
python3 tools/run_keypoint_v2_long_duration_benchmark.py \
  --experiment \
    tools/fixtures/keypoint_v2_integration_experiment_macos_v1.json \
  --output-dir /tmp/crimson-keypoint-v2-integration
```

Each trial is a new subprocess. The runner rejects incompatible result schemas,
failed absolute gates, dirty benchmark builds, unexpected commits, artifact or
workload digest mismatches, and missing structured output. `--resume` accepts
only an existing result that passes the same validation; it does not trust a
trial merely because its JSON file exists.

The reducer writes `aggregate.json` and `summary.svg`. For an explicit
baseline/contender comparison, it pairs matching repetition numbers and uses
the median of per-repetition contender/baseline ratios. Logical inequality,
incomplete trials, or a protected-metric regression fails the selection gate.
A valid contender replaces the baseline only when at least one declared
primary metric improves by the frozen material threshold; otherwise the
baseline is retained. Raw and refined semantic fixtures are not compared as
physical candidates because their decoded values intentionally differ.

The Python self-test covers cyclic ordering, paired reduction, logical
fail-closed behavior, and SVG generation:

```bash
python3 tools/run_keypoint_v2_long_duration_benchmark.py --self-test
```

## Integration Checkpoint

The 23,287-frame Palette raw and refined fixtures passed five fresh processes
each on the mounted macOS path. Exact opens used no dtype or metadata fallback,
offset indexes were read once, ordinary quality payload reads remained zero,
warm random p95 medians stayed below 0.19 ms, forward and reverse 70-frame page
p95 medians stayed below 4.6 ms, deadline misses and stale visible frames were
zero, and current-frame queue maximum medians stayed below 0.12 ms.

The structured aggregate, per-process results, logs, plot, hashes, and
interpretation are retained in
`docs/diagnostics/keypoint_v2_fresh_process_experiment_2026-07-30/`.

Those small archives fit entirely in the configured TensorStore caches after
the first random pass, so their later traversal transferred no additional file
bytes. This accepts harness behavior and interoperability, not long-duration
layout, cache-pressure, or transfer performance.

The benchmark target, Python runner self-test, and reverse-prefetch regression
also build and pass `3/3` in an isolated `ws1` Linux/CUDA 12.4 worktree at the
recorded evidence commit, configured for architectures `80;86`. This
establishes portable compilation and contract behavior only; it is not a
cross-host performance comparison.

## Full-Duration Gate

When Palette supplies the immutable full-length raw and refined artifacts:

1. Record each exact path, run, manifest digest, Palette commit, and handoff
   digest.
2. Run five fresh processes per physical candidate with balanced candidate
   order and matching repetition numbers.
3. Keep Mac mounted-path and Linux on-site results separate; do not compare
   them as platform performance evidence.
4. Compare logical digests, retained offsets, physical bytes, queue/service
   timing, deadlines, cancellation, cache behavior, and RSS.
5. Run a source-matched GUI smoke only after the headless gate passes.

One passing process is not a storage-profile promotion verdict. The frozen
fresh-process experiment and its aggregate are the promotion evidence unit.

### Sleepyfish V8 Result

Palette's full-duration selector-ineligible v8 package passed five refined
keypoint fresh processes on the mounted macOS path. The package has 1,188,000
camera frames and 1,169,010 keypoint rows. Median readiness was 594 ms, warm
random-frame p95 was 1.90 ms, forward/reverse 70-frame page p95 was 5.06/4.47
ms, process transfer was about 608 MiB, and peak RSS was about 254 MiB. All
deadline, stale-publication, retained-offset, read-failure, and quality-laziness
gates passed.

The structured result and cache-condition limitations are recorded in
`docs/diagnostics/keypoint_v2_full_duration_experiment_2026-07-30/`.
Because only one physical keypoint profile was supplied, this is a consumer
acceptance result rather than a physical-profile selection verdict.
