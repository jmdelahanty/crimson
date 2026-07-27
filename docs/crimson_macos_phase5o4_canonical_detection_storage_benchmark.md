# Crimson Phase 5O.4 Canonical Detection Storage Benchmark

Date anchored: 2026-07-24.

Status: the access-aware hybrid is compatible with Crimson and remains the
consumer-test finalist. It is not promoted. Exact schema, codec, consolidated
metadata, value-equivalence, retained-offset, random-UI, and traversal
throughput checks passed. The one-time offset threshold and first-pass
eight-column random-frame tail latency did not pass in every condition.

## Purpose

Palette compared a regular Zarr v3 layout with an access-aware sharded layout
for canonical detections. This checkpoint exercises those exact stores through
Crimson's bundled TensorStore build over the mounted Johnson Lab SMB volume.
It answers four consumer questions:

1. Can Crimson open the declared schema and hybrid codec chain without dtype
   probing or compatibility fallback?
2. Can Crimson consume inline consolidated metadata without opening every
   array's metadata object?
3. Does retaining `frame_row_offsets` once support later reads without index
   rereads?
4. How do layout and a bounded TensorStore cache affect actual UI-shaped and
   contract-shaped reads over the Mac/VPN path?

The benchmark is a headless storage-adapter measurement. Its frame throughput
does not include video decode, scheduling delay, UI construction, GPU upload,
or presentation deadlines.

## Compared Stores

The five matched fixture repetitions are under:

```text
/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/
canonical_detection_storage/workflows/
sleepyfish_det_storage_access_aware_full_20260724_03/candidates/
sleepyfish_det_storage_access_aware_full_20260724_03/frames_full
```

The two layouts are:

| label | windowed inner chunks | eager offset inner chunks | outer shards |
| --- | ---: | ---: | ---: |
| regular control | 1 MiB | 1 MiB | none |
| access-aware hybrid | 128 KiB | 1 MiB | 8 MiB |

The hybrid uses indexed Zarr v3 sharding. Inner payloads use little-endian
bytes followed by Zstandard level 0. The shard index uses little-endian bytes
plus CRC32C and is stored at the end of the shard.

Crimson opened these exact arrays:

| path | declared type |
| --- | --- |
| `instances/frame_indices` | `int32 (N,)` |
| `instances/source_acquisition_frame_index` | `int64 (N,)` |
| `instances/instance_key` | `uint64 (N,)` |
| `instances/bbox_norm_coords` | `float32 (N,4)` |
| `instances/bbox_img_xyxy` | `float32 (N,4)` |
| `instances/centers_img_xy` | `float32 (N,2)` |
| `instances/scores` | `float32 (N,)` |
| `instances/class_ids` | `int32 (N,)` |
| `instances/frame_row_offsets` | `int64 (F+1,)` |

There were no fallback dtype opens.

## Consumer Environment

- Crimson source revision: `34ff3c38229450b4e6bbe9b16fed99e9ca966197`
- bundled TensorStore revision:
  `cc9fb6641b2b661d3569e1b16004af1c024251ad`
- macOS: `26.3.1` build `25D2128`
- architecture: Apple Silicon `arm64`
- mount: `smbfs` at `/Volumes/johnsonlab`
- user-reported access path: home Wi-Fi and VPN

The zero-byte and 134,217,728-byte TensorStore-cache processes were separate.
The latter is a benchmark candidate, not a production-default change. The
macOS/SMB and server page caches were not flushed or controlled, so no sample
is described as physically cold.

## Validation Before Timing

One regular and one hybrid repetition were validated separately before the
matrix:

- all nine direct array metadata objects matched their inline consolidated
  descriptions;
- the direct and consolidated `instances` group envelopes matched after
  normalizing an optional empty nested consolidated-metadata object;
- exact dtype and rank opens succeeded for both layouts;
- offsets started at zero, were monotone, contained `F+1` entries, and ended at
  `N`;
- sampled offset slices contained only rows with the requested frame index;
- regular and hybrid offset and sampled-value fingerprints matched; and
- the hybrid codec and CRC chain decoded through Crimson's TensorStore build.

The complete matrix also compared deterministic value fingerprints across
every layout, cache, repetition, workload, and repeated pass. All matched.

## Matrix Design

The final matrix contained 20 new processes:

```text
5 matched repetitions x 2 layouts x 2 TensorStore cache limits
```

Repetition order alternated cache-first and layout-first conditions. Each
process:

1. read root inline consolidated metadata once;
2. opened nine exact typed handles with `assume_metadata`;
3. read and retained complete `frame_row_offsets` once;
4. closed the TensorStore offset handle;
5. ran deterministic UI random frames, contract random frames, contract row
   ranges, UI forward traversal, contract forward traversal, and UI reverse
   traversal; and
6. repeated every workload immediately in the same shared process cache.

UI-shaped reads use the fields needed by the current canonical detection
overlay: normalized boxes, scores, and class IDs. Contract-shaped reads use all
eight instance columns. Reads for different columns are currently issued
serially. TensorStore may batch physical work within an individual array read.

`frame_row_offsets` was never reread during random access or traversal. It
retained 9,504,008 bytes for 1,188,001 offsets in every process.

## Initialization Result

Values are five-run medians. Parentheses show the observed minimum and maximum.

| cache | layout | consolidated root | nine exact opens | full offsets read |
| ---: | --- | ---: | ---: | ---: |
| 0 | regular | 47.7 ms (24.6-63.4) | 1.14 ms (0.45-1.59) | 114.8 ms (70.9-149.5) |
| 0 | hybrid | 60.8 ms (60.7-77.1) | 1.08 ms (1.00-3.06) | 130.7 ms (114.2-133.8) |
| 128 MiB | regular | 62.4 ms (59.5-67.6) | 1.00 ms (0.48-1.57) | 153.8 ms (143.7-267.9) |
| 128 MiB | hybrid | 25.3 ms (15.0-63.9) | 0.64 ms (0.52-1.72) | 83.1 ms (71.6-168.2) |

Inline consolidated metadata removes the earlier per-array metadata-open
bottleneck: the nine typed opens required no array-metadata file reads in the
timed path and completed around one millisecond. The full offset read did not
consistently meet the provisional 100 ms median gate. Only the cached hybrid
median passed it.

Cache size should not materially accelerate a one-shot offset read by itself.
The variation between cache conditions is therefore evidence of uncontrolled
lower-layer and process-order latency, not proof that 128 MiB is required for
the offset array. The hybrid remained within the separate no-more-than-25-ms
absolute-regression gate for both cache conditions.

## Random UI Result

The following values are five-run medians for each run's 128-frame p95. File
bytes cover the complete first workload pass.

| cache | layout | first-pass frame p95 | first-pass wall | file bytes | immediate-repeat p95 |
| ---: | --- | ---: | ---: | ---: | ---: |
| 0 | regular | 68.5 ms | 2,470 ms | 57.8 MiB | 2.22 ms |
| 0 | hybrid | 31.0 ms | 2,020 ms | 7.7 MiB | 1.48 ms |
| 128 MiB | regular | 48.9 ms | 1,020 ms | 4.6 MiB | 0.11 ms |
| 128 MiB | hybrid | 0.47 ms | 27 ms | 3.3 MiB | 0.03 ms |

All UI-shaped cross-repetition p95 gates remained below 150 ms, including the
observed run-to-run range. At zero TensorStore cache, the hybrid transferred
about 7.5 times fewer file bytes than the regular control. With the cache
enabled, repeated reads caused no additional file transfer.

## Random Contract Result

The eight-column contract read is deliberately broader than the current UI.
Its first-pass five-run median p95 values were:

| cache | regular | hybrid |
| ---: | ---: | ---: |
| 0 | 262.2 ms | 89.4 ms |
| 128 MiB | 156.4 ms | 1.06 ms |

Those medians favor the hybrid, but the cross-repetition p95 values were
308.3/267.0 ms at zero cache and 283.9/223.9 ms at 128 MiB for
regular/hybrid. Therefore every first-pass eight-column group failed the
provisional 150 ms tail gate. The immediate-repeat hybrid groups passed; the
zero-cache regular repeat was 151.4 ms at the cross-repetition p95.

This is the main remaining storage-path tail risk. The current benchmark issues
the eight column reads serially. A future adapter should not require every
canonical field before drawing the three-field detection overlay, and should
test concurrent independent-field reads before attributing the complete tail
to physical layout.

## Traversal Result

Traversal used complete duration in 700-frame windows. FPS is storage-adapter
throughput only.

| workload/cache | regular median FPS | hybrid median FPS | regular file bytes | hybrid file bytes |
| --- | ---: | ---: | ---: | ---: |
| UI forward, 0 | 375,635 | 1,277,502 | 747.8 MiB | 104.2 MiB |
| UI forward, 128 MiB | 22,402,497 | 18,394,529 | 0 | 0.8 MiB |
| contract forward, 0 | 114,737 | 470,231 | 4,632.4 MiB | 622.6 MiB |
| contract forward, 128 MiB | 9,798,743 | 7,016,186 | 0 | 2.3 MiB |

Every traversal group exceeded the provisional 1,400 FPS lower bound by a wide
margin, including the five-run lower-tail reducer. At zero cache, indexed
sharding generated more file-range operations but reduced transferred bytes by
about 7.2-7.4 times. This is consistent with reading independently addressable
inner chunks rather than complete 8 MiB shards.

The cached traversal numbers include reuse from earlier ordered workloads in
the same process. They establish bounded-cache reuse, not independent cold
traversal latency.

## What Is And Is Not Measured

TensorStore counters captured file reads, batched reads, file bytes, cache hits,
cache misses, and evictions for each phase. The benchmark did not capture each
file offset/length tuple, distinguish shard-index cache entries from decoded
payload entries, or prove SMB wire bytes with packet tracing. Exact physical
range evidence remains missing if promotion requires it; `fs_usage` or a
request-logging file/HTTP adapter should be added rather than inferred from
wall time.

The zero-byte TensorStore cache does not disable macOS, SMB client, server page,
or storage-controller caches. That explains why a second zero-cache pass can be
faster while still recording TensorStore misses and repeated file bytes.

## Prefetch Follow-Up

The controlled mounted-Mac prefetch/cache sweep is complete and documented in
`docs/crimson_macos_phase5o4_prefetch_cache_benchmark.md`. It used the physical-
layout/cache result here as its baseline.

The target adapter can prefetch without rereading the frame index:

1. map a future frame interval to one exact row interval through the retained
   offsets vector;
2. align the row interval to the physical inner chunks for only the enabled UI
   fields;
3. submit 0.5-second or 1.0-second directional work at speculative priority;
4. let current-frame demand promote or reuse equivalent in-flight work;
5. publish presentation only from ready application/TensorStore cache entries;
6. cancel queued work by generation after a seek or direction change; and
7. retain already completed immutable chunks under the byte-bounded LRU until
   ordinary eviction.

Prefetch cannot predict an arbitrary scrub destination. A scrub should demand
only the fields needed for the destination frame, then prefetch neighboring
chunks after the new generation settles. Independent columns should be tested
concurrently so a slow optional field does not serialize the current overlay.

The completed benchmark swept:

- TensorStore cache limits of 0, 16, 32, 64, and 128 MiB;
- read-ahead of 0, 0.5, and 1.0 seconds;
- forward, reverse, and deterministic seek workloads;
- deadline misses after warmup, stale cancellations, useful-prefetch ratio,
  queue delay, bytes, cache hits, evictions, and peak resident bytes; and
- cache-only presentation behavior under simultaneous video playback.

All 80 fresh processes passed the frozen deadline, cancellation, stale-result,
transfer-waste, concurrency, offset-read, and memory gates. A 16 MiB
TensorStore cache was the smallest sampled cache within 10% of minimum transfer
for every read-ahead horizon. Zero speculative read-ahead, beyond the one-page
asynchronous demand lead, had no deadline misses and transferred the same bytes
as the longer horizons. This selects 16 MiB/no speculation for a paired full-
analysis fixture; it does not yet change the 64 MiB production cache.

## Decision

1. Keep the access-aware hybrid as Palette's Crimson finalist.
2. Do not promote it from this result because the full-offset median gate and
   first-pass eight-column random tail gate did not pass consistently.
3. Require inline consolidated metadata and exact declared dtypes in the future
   canonical adapter; do not reintroduce per-array fallback probing.
4. Read and retain selected-run `frame_row_offsets` once. Playback, traversal,
   and scrubbing must not reread it.
5. Preserve a nonzero bounded TensorStore cache. Do not change the production
   budget from two endpoint measurements alone.
6. Carry the measured 16 MiB/no-speculation candidate into paired full-analysis
   fixtures before changing Palette's chunk policy or Crimson production.

## Artifacts

The checked-in reduced evidence is under
`docs/diagnostics/canonical_detection_storage_macos_20260724/`:

- `environment.json`: consumer revisions, OS, mount, cache limits, and fixture;
- `execution_plan.json`: balanced 20-process order;
- `initialization.csv`: per-process metadata, exact-open, and offset evidence;
- `workloads.csv`: per-workload/pass timing, bytes, cache, and digest evidence;
- `summary.json`: five-run reducers and every gate result; and
- `cache-profile.png` / `cache-profile.svg`: cache/read profile plots.

The follow-up evidence is under
`docs/diagnostics/canonical_detection_prefetch_macos_20260724/` and includes the
balanced 80-process plan, prefetch/offset/seek CSVs, reduced summary, and cache-
prefetch plots.

The complete resumable child results remain at:

```text
/tmp/crimson-canonical-detection-macos-vpn-20260724-v2
```

The implementation is in:

- `tools/canonical_detection_storage_benchmark.cpp`;
- `tools/run_canonical_detection_storage_benchmark.py`; and
- `tools/plot_canonical_detection_storage_benchmark.py`.
