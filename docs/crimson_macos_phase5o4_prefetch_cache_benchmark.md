# Crimson macOS Phase 5O.4 Prefetch And Cache Benchmark

Date: 2026-07-24

Status: complete; next-stage candidate selected; production policy unchanged

## Purpose

The canonical-detection storage baseline established that Palette's
access-aware hybrid layout substantially reduces transfer and UI-shaped random
latency. It did not establish the smallest sufficient TensorStore cache or the
amount of directional read-ahead that preserves optional overlays at a
700-frame-per-second presentation rate.

This checkpoint measures that policy without changing production playback or
Palette's writer. It keeps the current shared data scheduler contract, issues
independent UI fields concurrently within one logical-source request, and
presents analysis only from a ready bounded cache. A missing overlay never
blocks the simulated video deadline.

## Outcome

All 80 fresh processes completed and every frozen prefetch gate passed. Within
the sampled cache limits, 16 MiB was the knee for all three read-ahead
horizons. During the first playback phase it reduced median hybrid-layout file
transfer from 3,826,500 bytes with no TensorStore cache to 76,530 bytes. The
32, 64, and 128 MiB limits transferred the same 76,530 bytes, so they provided
no storage-reuse benefit for this workload.

Zero, 0.5-second, and 1.0-second speculative horizons all produced zero
post-warmup deadline misses. The larger horizons made every submitted
speculative page useful, but did not reduce transfer or misses relative to the
one-page asynchronous demand lead. The next full-analysis fixture candidate is
therefore:

```text
TensorStore cache: 16 MiB
speculative read-ahead: 0 frames
demand lead: one 70-frame / 100-ms page
```

This is the smallest sampled nonzero cache, not proof that 16 MiB is the exact
minimum possible budget. It is also a candidate for the paired full-analysis
fixture, not a change to Crimson's current 64 MiB production archive cache.

## Mounted-Mac Results

The first-playback-phase medians for zero speculative read-ahead were:

| Layout | TensorStore cache | File bytes | Page-read p95 | Post-warmup misses |
| --- | ---: | ---: | ---: | ---: |
| hybrid | 0 MiB | 3,826,500 | 3.636 ms | 0% |
| hybrid | 16 MiB | 76,530 | 0.865 ms | 0% |
| hybrid | 32 MiB | 76,530 | 1.008 ms | 0% |
| hybrid | 64 MiB | 76,530 | 1.024 ms | 0% |
| hybrid | 128 MiB | 76,530 | 0.929 ms | 0% |
| regular anchor | 128 MiB | 603,167 | 1.427 ms | 0% |

The 16 MiB hybrid candidate used about 50 times less physical file transfer
than the zero-cache hybrid and 7.9 times less than the regular-layout anchor.
All conditions overlapped the three requested UI fields. Every direction also
evicted 18 pages while traversing 50 pages through the 32-page presentation
cache, proving that zero misses did not depend on retaining the full traversal.

Here, file bytes are ranges requested through TensorStore's file kvstore. They
show application/driver read amplification but are not packet-captured SMB wire
bytes. The macOS unified buffer cache or SMB client may satisfy a recorded file
read without another server transfer.

For the 16 MiB/no-speculation candidate, the cross-run random-seek cancellation
p95 was 155.0 ms, post-cancel transfer p95 was 78,204 bytes, stale publications
were zero, and peak-RSS p95 was 46,196,326 bytes. Across the complete hybrid
matrix, the worst reduced values were 225.3 ms cancellation, 79,828 bytes after
cancellation, zero stale publications, and 46,196,326 bytes peak-RSS p95. All
remain inside their frozen limits.

Offset initialization remains separate and unresolved. The five first
processes, with OS/filesystem cache uncontrolled, had a 135.2 ms median and
143.3 ms p95. The four process-first hybrid reads had a 132.5 ms median. The
26.1 ms median across all 75 hybrid processes mixes in repeated access after
the same mounted files warmed and must not be presented as fresh-start
latency. Playback and seeking nevertheless retained the offsets and performed
exactly one offset read per adapter lifetime in all 80 processes.

## Frozen Matrix

The main matrix contains 75 fresh processes:

```text
5 TensorStore cache limits x 3 read-ahead horizons x 5 repetitions
```

The cache limits are 0, 16, 32, 64, and 128 MiB. The read-ahead horizons are
0, 350, and 700 frames, corresponding to 0, 0.5, and 1.0 seconds at 700 FPS.
Each repetition also contains a fresh regular-layout, 128 MiB, zero-read-ahead
anchor. The total is therefore 80 processes. Process order is rotated and
reversed across repetitions. Forward runs first in even repetitions and reverse
runs first in odd repetitions so neither direction always inherits the other
direction's decoded TensorStore cache.

The current production `ArchiveContext` cache limit is recorded explicitly as
64 MiB. The experiment does not silently substitute TensorStore's zero-byte
default or infer total memory from the configured cache limit.

## Playback Model

Presentation advances in fixed 70-frame/100-ms pages over a 3,500-frame
interval in both forward and reverse directions. The first page is prepared
before the clock starts, matching Crimson's session-readiness boundary.
Fifty pages exceed the fixed 32-page presentation-cache cap, so the run also
exercises and reports ordinary LRU eviction.

At each presentation deadline:

1. the renderer-facing path performs a cache-only lookup;
2. a missing analysis page is counted and playback continues;
3. the next 70-frame page is submitted as current demand; and
4. any remaining pages inside the configured horizon are submitted as
   speculative work.

Zero read-ahead still includes the one-page demand lead needed for an
asynchronous producer. The 350- and 700-frame settings describe the complete
future horizon from the current page, not an additional hidden horizon.

`bbox_norm_coords`, `scores`, and `class_ids` are read concurrently with three
TensorStore operations inside a single scheduled job. This preserves scheduler
source isolation: one logical detection stream cannot occupy multiple shared
scheduler workers, while independent storage fields may still overlap.

The application presentation cache is fixed at 4 MiB and 32 pages. It retains
the exact decoded vectors and uses the shared byte-budgeted LRU policy. This
cache is separate from TensorStore's decoded-chunk and shard-index cache.

## Seek And Cancellation Model

The eight deterministic Palette random frames drive a cancellation phase. For
each destination, the benchmark:

1. submits current demand plus the configured speculative horizon;
2. waits until that generation begins work;
3. advances the source generation;
4. waits for the cancelled source work to settle; and
5. records cancellation latency and file bytes transferred after cancellation.

The worker checks the cancellation token and generation before and after the
TensorStore reads. A stale result is never inserted into the presentation
cache. TensorStore file reads already in progress are not assumed to be
physically cancellable; their remaining transfer is measured as waste.

## Separate Offset Subtest

Every fresh process reads the complete selected-run `frame_row_offsets` array
once, validates it, retains the 9.5 MiB vector, and closes the TensorStore
handle. Playback and seeking resolve row ranges only from that retained vector.

Offset time, file operations, transfer, retained bytes, and RSS after offset
initialization are reported separately. Prefetch cannot explain or repair the
previously observed one-time offset latency variance, so offset performance is
not mixed into the prefetch cache-knee reducer.

## Frozen Gates

These numeric limits were fixed before the 80-process matrix was interpreted:

| Evidence | Limit |
| --- | ---: |
| post-warmup overlay deadline miss rate | at most 1% |
| random-seek cancellation cross-run p95 | at most 250 ms |
| post-cancel transfer cross-run p95 | at most 1 MiB per seek |
| stale cache publications | exactly 0 |
| total benchmark peak RSS | at most 768 MiB |
| observed concurrent UI-field reads | at least 2 |
| offset reads per adapter lifetime | exactly 1 |

The miss-rate gate is applied after a fixed 700-frame/one-second warmup. Peak
RSS is the process high-water mark from `getrusage`, so it includes retained
offsets, TensorStore cache entries, presentation pages, in-flight decoded
arrays, scheduler state, and benchmark/runtime overhead.

A cache-knee candidate is the smallest cache that passes the miss-rate gate and
whose median physical file bytes are within 10% of the minimum passing
condition for the same read-ahead horizon. This comparison uses only the first
playback phase from each fresh process. Forward-first and reverse-first order is
balanced across repetitions, so second-phase TensorStore or filesystem warmth
cannot choose the cache size. The reducer does not simply choose the largest or
fastest cache.

## Promotion Boundary

Passing this matrix selects only a cache/read-ahead candidate for the next
consumer test. It does not promote Palette's storage profile or change Crimson
production policy. Promotion remains blocked until paired noncanonical
full-analysis fixtures establish end-to-end archive initialization and actual
application playback behavior without a material regression.

Production `detect_yolo` output remains unchanged during this checkpoint.

## Implementation And Artifacts

The benchmark implementation is in:

- `tools/canonical_detection_storage_benchmark.cpp` (`prefetch` mode);
- `tools/run_canonical_detection_prefetch_benchmark.py`; and
- `tools/plot_canonical_detection_prefetch_benchmark.py`.

The complete resumable process outputs are written outside the repository to:

```text
/tmp/crimson-canonical-detection-prefetch-macos-vpn-20260724-v3
```

The earlier `v1` and `v2` directories are superseded partial runs. Review found
that `v1` always executed forward before reverse. `v2` corrected direction
order but did not yet record scheduler queue delay or cross the presentation
cache's item cap. Both were stopped before reduction and are not selection
evidence.

Reduced checked-in evidence is under
`docs/diagnostics/canonical_detection_prefetch_macos_20260724/`:

- `environment.json`: Crimson/TensorStore revisions, Mac and mount identity,
  configured cache matrix, and frozen gates;
- `execution_plan.json`: the balanced 80-process order;
- `prefetch.csv`: both playback directions, physical/cache metrics, queue
  delay, bounded-cache eviction, and useful-prefetch evidence;
- `offset_initialization.csv`: process order and the separate one-time offset
  measurements;
- `seek_cancellation.csv`: cancellation, post-cancel transfer, stale-result,
  and RSS evidence;
- `summary.json`: five-process reductions, cache-knee selection, offset context,
  and every frozen gate; and
- `cache-prefetch-profile.png` / `.svg`: cache, transfer, cancellation, and
  process-memory plots.

The next checkpoint is the paired noncanonical full-analysis comparison frozen
in `docs/crimson_macos_phase5o4_full_analysis_fixture_contract.md`. Its first
stage holds Crimson's production archive cache at 64 MiB while comparing the
regular and hybrid layouts. Only after the hybrid passes does its second stage
compare 16 MiB with 64 MiB. The explicit canonical-detection application
adapter and fail-closed `--detection-run` override are now implemented. Palette
may build the paired fixtures, but neither Palette's layout nor Crimson's
production cache/read-ahead policy changes before the full-archive runner and
reducer pass.

## Verification

- the complete 80-process mounted-volume runner reduced successfully;
- all 120 aggregate gate records passed and all 80 adapters retained exactly
  one offset read;
- the benchmark target rebuilt with Apple Clang;
- `clang-format 22.1.8 --dry-run --Werror` passed for the C++ benchmark;
- both Python tools compiled and the no-anchor reduced-smoke path passed;
- all 58 macOS headless CTest targets passed; and
- the PNG/SVG profile was generated and visually inspected.
