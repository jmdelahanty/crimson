# Zarr v3 tabular sharding performance

Date: 2026-07-14  
Host: `delahantyj-ws1.hhmi.org`  
Destination filesystem: NFS mounted below `/groups/johnson/johnsonlab/jeremy`  
Zarr Python: 3.1.3; NumPy: 2.2.6; Zarr async concurrency: 10

## Executive conclusion

For large, immutable tabular analysis surfaces, use:

- 1,024 rows per inner chunk;
- 131,072 rows per outer shard as the general default;
- 32,768 rows per outer shard when parallel shard ownership or lower
  maintenance-write amplification matters more than minimum object count.

Do not collapse a million-row surface into only one or two outer shards. Those
extremes did not materially improve full publication and made small rewrites
substantially slower.

This recommendation assumes the editing architecture is an immutable base plus
sparse delta runs. Live editors should write small, separately owned deltas.
Compaction should merge the base and a frozen delta generation into a new
immutable run, validate it, then atomically promote the new run pointer.

The subsequent Crimson/TensorStore qualification described below passed for
ordinary chunks, 32,768-row shards, and 131,072-row shards. It strengthens the
case for 131,072 rows as the immutable tabular default: the sharded fixtures
loaded 11-15 times faster, returned byte-identical refined boxes/keypoints, and
had indistinguishable steady-state playback timing. No production default was
changed as part of this work.

## Why larger outer shards published so quickly

Zarr v3 indexed sharding separates two granularities:

- the **inner chunk** is the unit of compression and logical reads;
- the **outer shard** is the filesystem object containing many indexed inner
  chunks.

The 131,072-row layout did not turn the data into one monolithic compressed
block. It retained 1,024-row independently compressed inner chunks but packed
128 of them into each outer shard. Full sequential publication therefore did
roughly the same encoding work while avoiding thousands of NFS file creates,
opens, closes, directory updates, and metadata operations. On this workload,
filesystem object overhead dominated the ordinary-chunk layout.

The large-shard advantage is not unlimited. Updating an inner chunk inside an
existing shard can require shard-level read-modify-write behavior. One- and
two-shard layouts exposed that amplification without improving publication
enough to justify it.

## Does reading pay for the whole shard?

Normally, no. The indexed shard stores the byte location and length of each
independently encoded inner chunk. A compliant reader can load the shard index
and range-read only the requested 1,024-row inner chunk. A random row read does
not inherently fetch or decompress all 131,072 rows.

There is not literally zero overhead:

- the shard index must be read or found in cache;
- the backend performs a range read or file seek;
- remote/object backends may add a request for the index and another for data;
- cache state and backend range-read quality affect latency.

The current benchmarks validated sample reads after each write, so those reads
were warm and were not a dedicated cold-cache random-read qualification. Before
making the format default, run fresh-process random and sequential read tests
through both Zarr Python and Crimson's TensorStore reader. The expected result
is no outer-shard-sized read amplification, but that should be measured.

## Benchmark 1: finalized refined keypoints

This was real refined production data, not synthetic data:

```text
refined_keypoints_runs/
  refined_keypoints_sleepyfish_kp_allclips_20260708_01/
    keypoints_img
```

Source recording:

```text
/groups/johnson/johnsonlab/jeremy/recordings/
  sleepyfish_2026_05_05_17_45_30_cam2010095/zarr/
  sleepyfish_2026_05_05_17_45_30_cam2010095_analysis.zarr
```

Array shape was `(1,169,010, 5, 2)`, dtype `float64`, with 93.5 MB raw and
approximately 37.5 MB stored. Results below are means of two passes run in
opposite layout order.

| Layout | Data objects | Files | Full write (s) | One-row update (s) | 1,024-row update (s) |
| --- | ---: | ---: | ---: | ---: | ---: |
| Ordinary chunks | 1,142 | 1,143 | 3.738 | 0.016 | 0.011 |
| 2,048 rows/shard | 571 | 572 | 2.211 | 0.018 | 0.016 |
| 8,192 rows/shard | 143 | 144 | 0.679 | 0.018 | 0.019 |
| 32,768 rows/shard | 36 | 37 | 0.495 | 0.023 | 0.028 |
| 131,072 rows/shard | 9 | 10 | 0.611 | 0.047 | 0.060 |
| 584,704 rows/shard | 2 | 3 | 1.819 | 0.346 | 0.338 |
| 1,169,408 rows/shard | 1 | 2 | 4.975 | 1.896 | 1.874 |

Interpretation:

- 32,768 rows was fastest to publish.
- 131,072 rows reduced the data to nine shard objects while remaining
  sub-second for an in-memory full write.
- One or two giant shards were slower to publish and imposed severe small-write
  amplification.

Source preload took 1.08-2.90 seconds and is not included in the full-write
column.

## Benchmark 2: representative full refined-detection instances

The detection benchmark combined the actual `instances` arrays from all 22
finalized per-clip refined-detection runs for the same recording. The combined
surface contained 1,169,010 detection rows and 1,188,000 frame rows.

Twelve arrays were published together:

```text
bbox_img_xyxy
bbox_norm_coords
class_ids
confidence_scores
frame_counts
frame_indices
frame_offsets
manual_edit_flags
reason_bytes
refined_row_ids
source_detect_row_index
source_kind_codes
```

The raw bundle was 194.3 MB and stored in approximately 20.2 MB. The redundant
variable-length `reason` array was omitted because `reason_bytes` contains the
same labels in the fixed-width cross-platform representation. QA arrays outside
the `instances` group were also outside this benchmark.

Results below are means of two passes run in opposite layout order.

| Layout | Logical data objects | Observed files | Full bundle write (s) | One bbox-row update (s) | Six-array edit (s) |
| --- | ---: | ---: | ---: | ---: | ---: |
| Ordinary chunks | 13,742 | 11,466 | 70.109 | 0.018 | 0.114 |
| 8,192 rows/shard | 1,722 | 1,450 | 10.847 | 0.021 | 0.114 |
| 32,768 rows/shard | 434 | 376 | 4.638 | 0.020 | 0.117 |
| 131,072 rows/shard | 110 | 106 | 3.687 | 0.025 | 0.161 |
| 584,704 rows/shard | 26 | 36 | 3.622 | 0.081 | 0.271 |
| 1,169,408 rows/shard | 14 | 26 | 4.043 | 0.130 | 0.440 |

The logical object count is an upper bound from array lengths and shard shape.
Observed files include array/group metadata, while Zarr may omit chunks or
shards that contain only fill values.

The six-array edit updated both box forms, confidence, manual-edit flag,
fixed-width reason, and source-kind code. A class edit would additionally touch
`class_ids`. Counts, offsets, and lineage arrays do not need to change for a
pure box-coordinate edit, but insertion/deletion or topology-changing edits can
require those structures to be rebuilt during compaction.

Source preload took 3.63-4.20 seconds and is not included in the full-write
column.

## Crimson/TensorStore full-bundle qualification

Three minimal but Crimson-loadable analysis archives were built from the same
real 1,169,010-row refined data. Each archive contained:

- the representative twelve-array refined-detection bundle;
- a matching base detection run;
- 45 numeric or fixed-width arrays from the finalized refined-keypoint run;
- the crop row/frame metadata needed for overlay placement and direct edits.

All layouts retained 1,024-row inner chunks. Only the outer layout differed.

| Layout | Files | Stored size (MB) | Load pass 1 (s) | Load pass 2 (s) | Mean load (s) |
| --- | ---: | ---: | ---: | ---: | ---: |
| Ordinary chunks | 57,301 | 275.7 | 21.476 | 23.546 | 22.511 |
| 32,768 rows/shard | 1,905 | 276.6 | 2.140 | 1.830 | 1.985 |
| 131,072 rows/shard | 542 | 276.6 | 1.593 | 1.501 | 1.547 |

Each measurement used a new process. Pass 1 used ordinary, 32K, 131K order;
pass 2 reversed that order. The OS and NFS caches were not forcibly dropped,
so these should be called fresh-process forward/reverse passes rather than
guaranteed cold-cache measurements.

Crimson selected `RefinedRoot` plus the expected finalized refined-keypoint run
in all six loads. Sequential and seeded-random queries returned identical FNV-1a
checksums in every layout. After eager loading, 20,000 frame lookups measured:

| Layout | Sequential lookup p99 (microseconds) | Random lookup p99 (microseconds) |
| --- | ---: | ---: |
| Ordinary chunks | 0.24-0.55 | 1.23-1.24 |
| 32,768 rows/shard | 0.24-0.31 | 1.27-1.28 |
| 131,072 rows/shard | 0.24-0.26 | 1.20-1.21 |

This confirms the important runtime property: Crimson eagerly materializes the
refined tabular arrays, then frame display and seeks index in-memory vectors.
Outer sharding changes archive-open behavior, not per-frame overlay lookup.

### Process memory

Observed post-load RSS ranges were:

| Layout | Post-load RSS (MB) | Peak RSS (MB) |
| --- | ---: | ---: |
| Ordinary chunks | 607-619 | 635-637 |
| 32,768 rows/shard | 628-641 | 660-663 |
| 131,072 rows/shard | 647-661 | 671-679 |

Sharding therefore cost approximately 20-54 MB of additional resident memory
in these runs. That is modest relative to the roughly 610-660 MB full loader
footprint, but it is a real tradeoff and should remain in release regression
tests.

### Native GUI playback

The existing GPU/OpenGL GUI playback smoke ran frames 0-600 of the affiliated
4512x4512, 30-fps HEVC video for all three archives. Every run reached frame 600
in approximately 19.97 seconds. After excluding the first 30 playback frames,
the 250-ms performance logger produced 79 samples per layout:

| Layout | Frame loop p50 / p95 / p99 (ms) | Overlay UI p99 (ms) | Refined detection lookup p99 (ms) |
| --- | ---: | ---: | ---: |
| Ordinary chunks | 8.401 / 8.406 / 8.420 | 0.057 | 0.002 |
| 32,768 rows/shard | 8.404 / 8.423 / 8.431 | 0.059 | 0.002 |
| 131,072 rows/shard | 8.404 / 8.412 / 8.430 | 0.058 | 0.001 |

The approximately 8.4-ms frame loop is the requested 120-Hz GUI cap. There is
no practically meaningful layout-dependent playback difference.

### Direct refined-keypoint editing

The qualification probe used Crimson's `RefinedKeypointRepository`, changed one
of five keypoints on ROI row 0, and then wrote the original coordinates back.
This is the full coordinated edit path, not a single-array microbenchmark: it
updates coordinate spaces, confidence, heading, geometry, QA flags, fixed-width
reason text, edit provenance, and summary metadata.

| Layout | Manual edit (s) | Coordinate-restoring edit (s) |
| --- | ---: | ---: |
| Ordinary chunks | 8.517 | 7.631 |
| 32,768 rows/shard | 1.529 | 1.186 |
| 131,072 rows/shard | 1.331 | 1.218 |

All calls succeeded. A fresh process subsequently loaded each modified fixture,
and all three produced identical sequential and random checksums. The second
write restores coordinates, not historical provenance: `edit_applied`, manual
reasoning, confidence, and derived manual values intentionally remain, as they
would after an actual user correction.

This full edit result differs from the earlier one-array rewrite benchmark.
On this NFS filesystem, avoiding thousands of ordinary objects outweighed the
indexed-shard read-modify-write cost even for the current multi-array edit.
This does not remove the concurrency rule: two writers must not mutate inner
chunks that share an outer shard without serialization or explicit shard
ownership.

## Expected compaction time

For data already merged in memory, the 131,072-row layout wrote:

- the keypoint coordinate array in about 0.61 seconds;
- the twelve-array detection bundle in about 3.69 seconds.

A real compaction also reads the base and frozen deltas, applies edits,
validates cross-array invariants, writes metadata, and promotes the new run. On
this host, read plus write alone was approximately:

- 1.7-3.5 seconds for the keypoint coordinate array;
- 7.3-7.9 seconds for the representative detection bundle.

Therefore, "a few seconds" is reasonable for a single compact coordinate
surface already in memory. A complete detection-run compaction should initially
be budgeted at roughly 8-12 seconds under similar filesystem conditions, then
measured in the production compactor. Storage congestion, cold cache, additional
QA arrays, validation, and durable-sync policy can increase that time.

## Timing and durability caveats

All timings are client-observed completion of synchronous Zarr API calls. The
benchmark did not issue a global filesystem `fsync` and is not a power-loss
durability measurement. It was run twice with reversed layout order to reduce
simple cache/order bias, but it is still one host and one NFS environment.

Publication should use a new run path and pointer promotion. It should never
overwrite the currently authoritative immutable base in place.

## Reproduction assets

Repository scripts:

```text
tools/benchmark_keypoint_sharding.py
tools/benchmark_detection_run_sharding.py
tools/create_refined_read_qualification_fixtures.py
tools/refined_tabular_read_qualification.cpp
```

Retained result files:

```text
/groups/johnson/johnsonlab/jeremy/crimson-keypoint-shard-benchmarks/
  aggregate-20260714-192942.md
  aggregate-20260714-192942.csv
  aggregate-20260714-192942.json
  keypoint-shard-benchmark-20260714-192656/results.json
  keypoint-shard-benchmark-20260714-192811/results.json
  detection-run-shard-benchmark-20260714-193838/results.json
  detection-run-shard-benchmark-20260714-194200/results.json
```

Generated benchmark array payloads were removed after each run; only results
were retained. The later full-bundle qualification fixtures and their results
were retained separately below
`/groups/johnson/johnsonlab/jeremy/crimson-refined-read-qualification-20260714`.

## Remaining gates before changing a production default

The Crimson reader, random seek, memory, GUI playback, and direct-edit gates are
now complete. Remaining work is:

1. Test concurrent writers with disjoint outer-shard ownership and prove that
   no two workers can read-modify-write the same shard.
2. Prototype the immutable-base plus sparse-delta compactor and measure complete
   read, merge, validate, publish, and pointer-promotion latency.
3. Repeat under representative cluster load before changing the canonical
   storage default.
