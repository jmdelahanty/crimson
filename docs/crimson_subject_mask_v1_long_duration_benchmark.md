# Crimson Subject-Mask V1 Long-Duration Benchmark

Date: 2026-07-31

Status: deterministic harness and five-process integration calibration pass;
full-duration physical-profile evidence remains pending

## Purpose

`subject_mask_v1_long_duration_benchmark` exercises the same backend-neutral
strict repository, shared data scheduler, and presentation cache used by
Crimson playback. It opens only an explicitly named selector-ineligible run,
does not write to the archive, and does not alter production selection.

The versioned workload is
`tools/fixtures/subject_mask_v1_long_duration_workload_v1.json`. Each result is
bound to its canonical JSON SHA-256, the Crimson revision and dirty-worktree
state, the selected run, and the exact run-manifest payload digest.

## Workload

Each fresh process performs:

- strict manifest and direct/consolidated metadata validation;
- exact typed opens with no legacy fallback or dtype probing;
- exactly one retained read of `frame_row_offsets`;
- first-frame presentation;
- 64 deterministic random frames followed by the same warm pass;
- 700-frame forward and reverse traversals in 70-frame pages;
- 32 rapid discontinuous seeks with generation cancellation; and
- bounded presentation-cache close and scheduler shutdown.

The scheduler uses four workers, one reserved current-frame worker, and no more
than one speculative worker. The presentation buffer uses direction-aware
12-frame lookahead and a 24-frame cache. A one-frame reverse step retains useful
cached work and schedules lookahead toward earlier frames instead of resetting
the generation as a discontinuous seek.

This fixture's source rate is 60 FPS, so a 70-frame page has a 1,166.7 ms
deadline after one warmup page. The workload also gates first readiness, warm
random-frame p95, current-frame queue wait, rapid-seek readiness, stale visible
frames, close latency, read failures, peak RSS, the one-offset-read rule, zero
derived-metric payload reads, and zero `roi_images` open attempts.

## Metric Semantics

The result deliberately separates three byte measurements:

- `process_physical.file_bytes` is TensorStore's process-wide measured file
  transfer for this workload.
- `repository_metrics.logical_chunk_source_bytes` is the total uncompressed
  dense mask content represented by every decoded chunk request. Cache reuse
  and compression mean it is not network transfer.
- `repository_metrics.sparse_retained_bytes_produced` is the cumulative compact
  foreground-index payload produced after conversion. It is not peak memory;
  `peak_cached_sparse_bytes` and process RSS describe retained memory.

Queue wait and repository service time are reported separately by request
priority and source. Rapid-seek physical bytes are a conservative upper bound:
they include useful final-frame and bounded lookahead reads as well as any work
that completed after being superseded. The correctness requirement is zero
stale frames reaching presentation; internal discarded completions are expected
under cancellation.

## Commands

Build and run the portable self-test:

```bash
cmake --build build/macos-arm64-release \
  --target subject_mask_v1_long_duration_benchmark
ctest --test-dir build/macos-arm64-release \
  -R '^subject_mask_v1_long_duration_benchmark_self_test$' \
  --output-on-failure
```

Run five fresh mounted processes and produce `aggregate.json`:

```bash
scripts/run_macos_subject_mask_v1_long_duration.sh
```

The script defaults to Palette's immutable integration fixture and writes to
`/private/tmp/crimson-subject-mask-v1-long-duration`. Its store, run, manifest
digest, frame size, workload, binary, and output directory can be replaced with
the corresponding `CRIMSON_SUBJECT_MASK_V1_*` environment variables. Frozen
workload values must not be changed when comparing physical candidates.

## Integration Calibration

Palette's 23,287-frame, 22,926-row fixture passed five fresh mounted macOS
processes. The canonical workload digest was
`58d1e4d16d9c30787f2612010ed4c71aaf08f4fa5271ca853bed5f74019966e7`.
All trials read the offset vector exactly once, performed zero derived-metric
payload reads and zero `roi_images` opens, and published zero stale frames.

Median first-presentation readiness was 978.1 ms, warm random-frame p95 was
97.1 ms, and forward/reverse 70-frame page p95 was 841.6/860.7 ms. The maximum
page deadline-miss ratio was zero. Median measured file transfer was 4.79 MB,
median peak RSS was 222.8 MB, and median final readiness after rapid seeks was
150.1 ms. Current-frame queue wait was at most 106.4 ms across the five trials.

These measurements were collected from an intentionally dirty implementation
worktree and are calibration, not immutable release evidence. A clean-revision
rerun should be retained with hashes when a full-duration fixture is supplied.

The implementation-equivalent benchmark and reverse-lookahead repository test
also passed in the isolated `ws1` Linux/CUDA 12.4 worktree, and the complete
Linux `redgui` target linked successfully with explicit architectures `80;86`.
No shared Linux checkout or dataset was changed, and no cross-host performance
comparison is inferred from this portable build result.

## Remaining Gate

The integration fixture is enough to accept the harness, exact reader behavior,
direction-aware playback, cancellation, and telemetry. It is not evidence for
24-hour or multi-subject cache pressure, sustained transfer, storage-layout
selection, or profile promotion. A full-duration fixture must run the same
versioned workload in five fresh processes per physical candidate, with
balanced order and matching repetitions, before a physical profile is selected.
