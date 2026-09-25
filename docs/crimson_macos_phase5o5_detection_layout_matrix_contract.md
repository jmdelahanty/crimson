# Crimson Phase 5O.5 Detection-Layout Matrix Contract

Date: 2026-07-26

Contract version: 1

Status: deferred by the Phase 5O.5 residency-strategy gate; Palette publication
is paused

## Decision

Palette must not build the five-candidate matrix yet. The superseding strategy
gate in `docs/crimson_macos_phase5o5_detection_residency_gate.md` first compares
bounded paging with first-page-then-background residency using the existing
full-duration regular and hybrid fixtures. This contract remains the exact
fallback experiment if residency fails. If residency passes, Crimson replaces
it with a reduced three-candidate matrix contract before Palette publishes any
new stores.

This matrix is detection-isolated. It follows the full-application Stage 1
comparison, which showed that nondetection repositories and scheduler queueing
can dominate first-overlay latency and whole-process memory. Those are real
Crimson concerns, but they must not be attributed to a canonical-detection
physical layout when every nondetection byte is unchanged.

The canonical workload is:

```text
docs/reference/crimson_canonical_detection_layout_matrix_workload_v1.json
```

Its SHA-256 is:

```text
75d958f7ef4a7162b9b945a210c25de26fb7c3020ed7e9d9551753875bcb6d57
```

The file is authoritative for the exact selections, process order, traversal
regions, page shape, cache policy, and phase order. The repository commit that
first contains the artifact must be recorded in Palette's matrix manifest.
The current pre-commit workspace revision `34ff3c38229450b4e6bbe9b16fed99e9ca966197`
must not be cited as containing this new file.

## Corrections To The Proposal

Palette must apply these consumer-side corrections before publication:

1. Crimson's current UI overlay reads `bbox_norm_coords`, `scores`, and
   `class_ids`. It does not read `bbox_img_xyxy` in the timed UI path.
   `bbox_img_xyxy` remains part of the all-column contract workload.
2. Palette still publishes 25 stores, but Crimson runs two fresh processes per
   store, for 50 processes total. Running UI and all-column reads sequentially
   in one process would allow the second workload to inherit the first
   workload's decoded TensorStore cache.
3. The 150 ms storage-latency gate applies to the UI workload. The all-column
   inspection/export workload is non-realtime and receives a 300 ms p95 gate.
4. Native TensorStore file counters do not currently identify individual
   shard-index range reads. Crimson will report directly observable file-read,
   batch, byte, and cache counters. Optional range tracing may add shard-index
   evidence, but missing native shard-index attribution must be reported as
   unavailable rather than inferred.

## Fixed Logical Dataset

Every candidate has:

- 1,188,000 camera frames;
- 1,187,087 detection instances;
- canonical detection schema `palette.stage.canonical_detection:1`;
- the same exact decoded values and normalized logical attributes; and
- one eager `int64 frame_row_offsets` array with 1,188,001 entries.

The eight windowed columns are:

| Path | Exact dtype and shape |
| --- | --- |
| `instances/frame_indices` | `int32 (N,)` |
| `instances/source_acquisition_frame_index` | `int64 (N,)` |
| `instances/instance_key` | `uint64 (N,)` |
| `instances/bbox_norm_coords` | `float32 (N,4)` |
| `instances/bbox_img_xyxy` | `float32 (N,4)` |
| `instances/centers_img_xy` | `float32 (N,2)` |
| `instances/scores` | `float32 (N,)` |
| `instances/class_ids` | `int32 (N,)` |

The eager index is:

| Path | Exact dtype and shape |
| --- | --- |
| `instances/frame_row_offsets` | `int64 (F+1,)` |

`frame_row_offsets` starts at zero, is monotone, ends at `N`, and selects rows
whose `frame_indices` equal the requested frame. It is read, validated, and
retained exactly once per process. The handle is then closed, and no later
seek, random read, or traversal may read it again.

## Physical Candidates

Palette publishes five independently materialized paths in each repetition:

| Candidate ID | Windowed inner chunks | Outer storage |
| --- | --- | --- |
| `hybrid_windowed_128k` | approximately 128 KiB by dtype and trailing shape | approximately 8 MiB indexed shards |
| `hybrid_windowed_64k` | approximately 64 KiB by dtype and trailing shape | approximately 8 MiB indexed shards |
| `aligned_rows_4096` | 4,096 complete rows for all eight columns | approximately 8 MiB indexed shards |
| `aligned_rows_8192` | 8,192 complete rows for all eight columns | approximately 8 MiB indexed shards |
| `regular_unsharded_1m` | approximately 1 MiB by dtype and trailing shape | genuinely unsharded |

For the first four candidates, `frame_row_offsets` remains separately eager:
approximately 1 MiB inner chunks, approximately 8 MiB outer shards, and the
existing Zarr v3 indexed-sharding codec/checksum chain. Row-aligned chunks
retain complete trailing dimensions. The regular control must not inherit an
outer shard merely because a generic profile name contains a shard budget.

Each repetition is a physically distinct publication. Published hardlinks,
reflinks, or shared mutable payload objects are forbidden. Palette may reuse
source-side computation while building, but each immutable destination must
stand alone.

## Equality Semantics

Cross-candidate byte-for-byte root metadata equality is neither possible nor
required because chunk, shard, codec, benchmark identity, and provenance
declarations differ. Required equality is:

- every decoded array is exactly equal;
- normalized logical schemas and logical attributes are exactly equal;
- direct array metadata exactly agrees with inline consolidated metadata
  inside each store; and
- cross-candidate differences are restricted to physical layout, benchmark
  identity, and provenance.

Palette fully validates offsets against all `frame_indices`, not only sampled
frames. Palette records complete canonical-detection hashes and the normalized
logical manifest in `matrix_manifest.json`.

Crimson validates that manifest before timing. Each timed process reads inline
consolidated metadata, validates all nine exact declarations, opens only the
exact handles needed by its process mode, and performs no dtype or legacy-path
fallback probes. A separate direct-metadata preflight must not be inserted
inside a timed payload phase.

## Process Design

Palette creates five candidates for each of five matched repetitions: 25
stores. Crimson launches two fresh processes per store:

- `ui_runtime` opens offsets plus `bbox_norm_coords`, `scores`, and
  `class_ids`; and
- `contract_all` opens offsets plus all eight windowed columns.

There are therefore 50 process runs. The two modes never share a TensorStore
context or Crimson decoded cache. Each uses the production 64 MiB TensorStore
cache. There is no speculative time-based read-ahead; UI traversal retains one
70-frame asynchronous demand-lead page.

The workload JSON defines a cyclic Latin-square candidate order. Every
candidate occupies each candidate ordinal once. Process-mode order alternates
by repetition. This balances obvious order effects but cannot evict the macOS
unified buffer cache, SMB client cache, or server cache. Results are described
as `fresh process, OS/filesystem/server cache uncontrolled`, never as cold.

The five traversal regions are disjoint across repetitions. The primary
direction alternates forward and reverse. The opposite direction immediately
repeats the same region and is a cache-reuse diagnostic; it cannot replace the
primary direction in the physical-layout reducer.

## UI Runtime Workload

The `ui_runtime` process performs these phases in order:

1. Initialize the process, file kvstore, repository, and metrics.
2. Read consolidated metadata and open the three exact UI fields.
3. Read, validate, retain, and close `frame_row_offsets` exactly once.
4. Prepare the 70-frame page beginning at frame zero before the playback
   clock.
5. Traverse the repetition's untouched 3,500-frame region in the primary
   direction using 70-frame pages and a 100 ms page deadline.
6. Traverse the same region in the opposite direction as a warm-cache
   diagnostic.
7. Execute the eight-frame rapid-seek cancellation sequence.
8. Execute the remaining 120 deterministic random frames.
9. Cancel outstanding work, settle the scheduler, and shut down.

For every logical UI request, the three field reads are issued concurrently
inside one logical detection job. Crimson joins the fields, verifies their row
counts, validates decoded values, and publishes only the active generation.
Observed peak concurrent field reads must be at least two.

The primary traversal prepares its first page before the clock. At each later
page boundary, presentation is cache-only and the next page has one 100 ms
demand-lead interval. The post-warmup deadline-miss rate excludes the first 700
frames, or ten pages. The denominator is the remaining 40 pages.

## All-Column Contract Workload

The `contract_all` process performs these phases in order:

1. Initialize the process, file kvstore, repository, and metrics.
2. Read consolidated metadata and open all eight exact windowed fields.
3. Read, validate, retain, and close `frame_row_offsets` exactly once.
4. Read all eight fields for 128 deterministic random frames.
5. Read all eight fields for 64 deterministic 32-row ranges.
6. Settle and shut down.

The eight fields are submitted concurrently per logical frame or range. They
are joined in the declared column order for shape validation and a deterministic
decoded-value digest. This workload protects inspection, export, refinement,
and training consumers. It is not subject to the 700 FPS playback deadline.

The random-row-range phase follows the random-frame phase in the same
`contract_all` process, so it is explicitly a later application-order
measurement. Only its own phase delta is reported; it is not described as
process-first or cold.

## Measurement Boundaries

Every process snapshots wall time, peak RSS, TensorStore counters, repository
counters, and scheduler counters at each phase boundary. At minimum it reports:

- process and repository initialization time;
- consolidated-schema validation and exact-handle-open time;
- one-time offset open, read, validation, retained bytes, and physical I/O;
- first-page queue wait, repository read/decode service, publication, and
  request-to-publication time;
- per-operation queue, storage/read, decode, and publication times;
- UI random-frame and all-column random-frame latencies;
- all-column 32-row-range latencies;
- traversal page service, deadlines, misses, physical bytes, and throughput;
- seek cancellation latency, stale publications, and bytes after
  cancellation;
- cache hits, misses, evictions, file reads, batch reads, and file bytes; and
- current and peak decoded/presentation bytes plus process peak RSS.

Offset initialization has its own metric snapshot. Offset bytes, reads, and
latency are excluded from first-page, random, range, traversal, and
cancellation deltas. Offsets remain included in elapsed time to an initialized
adapter and in total process RSS.

TensorStore `file_bytes` measures file-kvstore byte ranges requested through
the driver. It is not SMB wire traffic. macOS or SMB may satisfy a recorded
file read from RAM without another server transfer. The reducer must not label
these counters as network packets or server bytes.

Current native counters provide file reads, batch reads, bytes, cache hits,
cache misses, and cache evictions. If exact file ranges and shard-index reuse
are captured with `fs_usage` or an instrumented kvstore, they are retained as
additional evidence. Otherwise the result contains an explicit
`not_observable` field. It must not infer a shard-index cache hit solely from a
wall-time improvement.

Scheduler queue time is reported separately from repository storage/read and
decode time. Scheduler starvation can fail the Crimson runtime verdict, but it
does not make a Palette physical candidate incorrect or slow at storage
service.

## Percentiles And Pairing

Within a process, p50 and p95 use nearest rank: sort `n` observations and use
the one-based rank `ceil(p*n)`. Samples from different processes are never
pooled.

For each candidate and process mode, Crimson retains all five per-process
values and reports their median and nearest-rank cross-repetition p95. With
five repetitions, that p95 is the maximum. It is an intentionally conservative
noise indicator, not a population-confidence claim.

Candidate-versus-baseline comparisons are paired by repetition and process
mode. For positive lower-is-better quantities, each pair produces
`candidate / hybrid_windowed_128k`. Reports include every raw pair, the median
paired ratio, direction agreement, and the maximum paired ratio. No unpaired
ratio of aggregate medians is used for selection.

Offset initialization, UI work, and contract work remain separate. Offset
bytes and latency never enter page-read metrics. The primary traversal means
the first direction in the repetition's `direction_order`; the opposite
direction is diagnostic.

Near-zero values use these rules:

- if both paired values are zero, the ratio is `1.0`;
- if the baseline is zero and the candidate is positive, the ratio is not
  finite and the absolute-value rule is used;
- if median baseline cancellation waste is below 64 KiB, waste cannot qualify
  as a 20% improvement and its protected regression limit is a 64 KiB median
  absolute increase; and
- if baseline deadline misses are below 0.1%, the protected regression limit
  is an absolute 0.1 percentage-point increase, while the 1% absolute gate
  still applies.

Five pairs do not support a strong distributional inference. The contract
therefore uses paired direction consistency rather than a p-value or an
unstated confidence interval.

## Absolute Gates

Every selected candidate must pass all applicable absolute gates:

| Evidence | Frozen limit |
| --- | ---: |
| exact decoded correctness | pass |
| exact schema, codec, CRC, and consolidated/direct agreement | pass |
| stale publications | 0 |
| offset reads per process | exactly 1 |
| observed concurrent UI field reads | at least 2 |
| UI random-frame repository service p95 | at most 150 ms |
| primary UI page repository service p95 | at most 150 ms |
| all-column random-frame repository service p95 | at most 300 ms |
| all-column 32-row-range repository service p95 | at most 300 ms |
| post-warmup UI deadline misses | at most 1% |
| seek cancellation p95 | at most 250 ms |
| post-cancel file bytes | at most 1 MiB per superseded seek |
| detection-benchmark process peak RSS | at most 768 MiB |
| scheduler, repository, and decode failures | 0 |

The 150 ms page-service threshold and 100 ms playback deadline measure
different things. A paused first page or isolated random page may take up to
150 ms of repository service. During playback, the one-page demand lead gives
the producer 100 ms before presentation; the decisive realtime gate is at most
1% post-warmup deadline misses. Passing the 150 ms service cap does not excuse
a deadline miss-rate failure.

## Protected Metrics

The following lower-is-better values are protected against regression from
`hybrid_windowed_128k`:

- UI random-frame repository-service p95;
- primary UI traversal page-service p95, file reads, and file bytes;
- deadline-miss rate;
- cancellation p95 and post-cancel bytes;
- detection-benchmark peak RSS;
- all-column random-frame p95, file reads, and file bytes; and
- all-column row-range p95, file reads, and file bytes.

A protected metric passes when its median paired ratio is at most `1.10` and
no single paired ratio exceeds `1.25`. The near-zero absolute rules above
replace ratios for deadline misses and cancellation waste when applicable.
Every absolute gate must also pass.

## Replacement And Tie Rules

A challenger can replace the 128 KiB hybrid only when:

1. all correctness and absolute gates pass;
2. all protected metrics pass;
3. at least one declared primary metric has a median paired ratio at most
   `0.80`; and
4. that primary metric is no worse than the baseline in at least four of five
   paired repetitions.

Primary metrics are UI random-frame p95, primary UI traversal page-service
p95, primary UI traversal file bytes, all-column random-frame p95, and
all-column random-frame file bytes. Near-zero cancellation waste and deadline
misses cannot supply the required 20% improvement.

Selection is Pareto-based; there is no composite score. A challenger dominates
the baseline only under the rules above. If multiple challengers dominate the
baseline but none dominates the others, Crimson reports the tradeoff and
retains `hybrid_windowed_128k` rather than choosing by an invented weight. If
no challenger demonstrates a material improvement, retain the baseline.

The regular candidate is a genuine control and may be reported as Pareto
eligible, but it receives no exception from object-count, publication, or
consumer-read evidence merely because it is simpler.

## Required Output

Crimson's result directory contains:

- one immutable JSON result per process;
- the expanded 50-process execution plan;
- environment, mount, VPN, cache, Crimson, TensorStore, Palette, and fixture
  identity;
- workload JSON path and SHA-256;
- per-phase CSV tables;
- paired raw values and reduced summaries;
- latency, byte/read, cache, cancellation, deadline, and RSS plots;
- a correctness verdict;
- a detection physical-profile verdict; and
- separate Crimson scheduler/cache observations.

Palette's matrix manifest records the workload artifact, its SHA-256, the
Crimson commit that contains it, all candidate paths, physical declarations,
and logical equality evidence. Neither project changes production writers,
profiles, registries, selectors, source archives, training artifacts, or
earlier fixtures during this experiment.

Passing this matrix selects a proposed physical profile. It does not itself
modify Palette production output. Promotion remains a separately reviewed,
versioned storage-policy change.
