# Crimson Phase 5O.0 Data-Access Characterization

Date anchored: 2026-07-22.

Status: in progress. The pre-migration Mac subject-mask instrumentation and
mounted-PRFS production measurements are complete. Phase 5O.1, the first
subject-mask lazy-mapping slice, and post-migration PRFS probes are complete;
the first responsive whole-application archive-open measurement and the
headless archive-session cache baseline are also recorded. A first post-cache
mounted-PRFS subject-mask probe and the zero-cache versus 64 MiB sharded-index
controls are complete. Linux, local-storage, application-level traversal,
random-seek, and long-run measurements remain open.

## Scope

This is the first implementation slice of
`docs/crimson_macos_phase5o_bounded_data_access.md`. It characterizes the slow
subject-mask startup observed on the 1.18-million-frame Sleepyfish recordings
without changing archive schemas, coordinate semantics, selection behavior, or
mask presentation.

The portable subject-mask repository metrics now distinguish:

- total repository open, catalog, mapping read, storage open, contour open, and
  metadata index time;
- individual subject/crop mapping-column times;
- decoded metadata bytes and repository-retained metadata bytes;
- physical mask/contour bytes read for completed chunks;
- bytes produced and retained by the sparse chunk cache;
- bytes retained and released by the materialized frame-presentation cache;
- cumulative and maximum mask read, sparse conversion, contour load, and total
  chunk latency; and
- existing demand/prefetch/cache-hit/eviction/failure counters.

The Mac shell emits structured `[AppleDataAccess]` records for subject-mask open
begin, open completion/failure, mapping breakdown, first-ready publication, and
shutdown summary. `subject_mask_overlay_repository_probe` emits the same core
metrics without launching a GUI.

Byte values are decoded or retained in-process payload accounting. They are not
compressed network-transfer byte counts and do not include every allocator or
container bookkeeping byte. Process RSS remains the authoritative whole-process
measurement.

## Environment

The first production measurements were taken on the Apple Silicon development
Mac while connected from home over Wi-Fi and VPN to the mounted Johnson Lab
PRFS volume. The values characterize this access path and date; they must not be
treated as universal local-disk or server-local performance.

## Sleepyfish Cam2010094

Archive:

```text
/Volumes/johnsonlab/jeremy/recordings/sleepyfish_2026_05_05_17_45_30_cam2010094/zarr/sleepyfish_2026_05_05_17_45_30_cam2010094_analysis.zarr
```

Selected subject-mask run:

```text
refined_subject_masks_sleepyfish_cam2010094_full_encoded_20260715_v002_sleepyfish_cam2010094
```

The detailed warm-filesystem probe reported:

| stage | measurement |
| --- | ---: |
| repository open | 18,558 ms |
| catalog | 106 ms |
| all mapping reads | 17,938 ms |
| storage open | 100 ms |
| contour open | 321 ms |
| metadata index construction | 73 ms |
| decoded metadata | 94,081,924 bytes |
| retained repository metadata | 122,306,500 bytes |

The earlier cold probe took 25,404 ms to open the same repository, including
24,567 ms in mapping reads. Both trials identify mapping reads as the dominant
startup stage.

The warm-filesystem per-column breakdown was:

| column | time | physical chunk rows |
| --- | ---: | ---: |
| subject `frame_indices` | 5,541 ms | 1,024 |
| subject `detection_indices` | 5,693 ms | 1,024 |
| subject `source_crop_row_ids` | 5,378 ms | 1,024 |
| crop `frame_indices` | 389 ms | 16,384 |
| crop `roi_coordinates_full` | 494 ms | 16,384 |
| crop `detection_indices` | 400 ms | 16,384 |

All six arrays have 1,176,024 rows. Each subject column therefore requires about
1,149 physical chunks, while each crop column requires about 72. The three
subject columns decode to only 28,224,576 bytes, while the faster crop columns
decode to 65,857,344 bytes. This is evidence that mounted-filesystem per-chunk
latency and layout dominate simple byte throughput for these arrays.

The first dense mask chunk at frame zero reported:

| stage | measurement |
| --- | ---: |
| total resolve | 478 ms |
| mask/contour source payload read | 134,513,152 bytes |
| TensorStore mask read | 64 ms |
| dense-to-sparse conversion | 46 ms |
| contour load | 367 ms |
| retained sparse chunk payload | 3,238,992 bytes |
| warm same-frame resolve | 0.031 ms |

The 128-row dense mask chunk is released after conversion. Its retained sparse
form is about 2.4 percent of the decoded source payload in this sample. The
first-frame cold cost is dominated by contour loading, not mask conversion.

The post-migration frame-1024 probe on the remounted PRFS volume reported:

| stage | measurement |
| --- | ---: |
| repository open | 3,413 ms |
| catalog | 456 ms |
| eager mapping reads | 0 ms |
| storage open | 454 ms |
| contour open | 1,362 ms |
| first-demand frame-count index | 290 ms |
| first-demand mapping pages | 549,024 bytes in 2 reads |
| total first resolve | 3,120 ms |
| TensorStore mask read | 216 ms |
| dense-to-sparse conversion | 54 ms |
| contour load | 1,620 ms |
| retained sparse chunk payload | 3,238,992 bytes |
| warm same-frame resolve | 0.256 ms |

Repository open fell from 18,558 ms to 3,413 ms, an approximately 82 percent
reduction for these two VPN/PRFS observations. The cost did not disappear: the
compact 1,188,000-frame count index and exact mapping pages moved to first
demand, and contour access remains the largest first-resolution stage.

## GoodCopBadCop

Archive:

```text
/Volumes/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop/zarr/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop_analysis.zarr
```

Selected subject-mask run:

```text
refined_subject_masks_smart_finalizer_goodcopbadcop_prejuly_masks_wave01_20260712_v001
```

The 120,221-row repository opened in 4,750 ms, including 4,166 ms of mapping
reads. It decoded 9,617,684 metadata bytes and retained 12,502,988 repository
metadata bytes.

Its first 256-row dense chunk read 269,026,304 mask/contour payload bytes and
retained 5,568,592 bytes. The 628 ms cold resolution split into 126 ms of
TensorStore mask read, 89 ms of dense-to-sparse conversion, and 414 ms of
contour loading. The warm same-frame resolution took 0.038 ms.

The post-migration frame-1024 probe reported a 2,166 ms repository open with no
eager mapping reads, down from 4,750 ms. First demand spent 204 ms building the
140,035-frame count index, read 56,160 bytes in two mapping pages, and resolved
in 2,039 ms. That resolution included 217 ms of mask read, 89 ms of sparse
conversion, and 961 ms of contour loading; its warm repeat took 0.098 ms. The
slower first resolve than the earlier observation reinforces that mounted-PRFS
latency varies and that contour scheduling remains open work.

## Whole-Application Responsive Open

On 2026-07-23, a `0:30` Sleepyfish Cam2010094 video smoke exercised the native
Mac background repository loader over the mounted PRFS volume. The main thread
continued polling events and presenting the phase-oriented loading view for the
entire operation. The repository bundle became ready after 154,867 ms.

| product | open time | result |
| --- | ---: | --- |
| chaser polar | 107 ms | available but descriptor unavailable |
| stimulus | 19 ms | unavailable |
| keypoints | 53,017 ms | ready |
| subject masks | 1,720 ms | ready, lazy mapping |
| subject shape | 59,453 ms | ready |
| eye geometry | 31,940 ms | ready |
| motion | 2,952 ms | ready, 44,538,128 bytes preloaded |
| swim bouts | 4,660 ms | unavailable |
| eye-angle timeline | 666 ms | ready, paged in this run |
| remaining unavailable products | 166 ms | unavailable |

The application then completed the playback smoke exactly through frame 30.
Video startup was 1,156 ms, the smoke reported no late presentations or
catch-up seeks, and process RSS at completion was 1,261.5 MiB.

The phase display made the dominant costs directly observable: keypoints,
subject shape, and eye geometry account for about 144 seconds of the 155-second
repository-open time. All three still eagerly read million-row placement or
lineage arrays. Keypoints additionally materialize the complete keypoint tensor
and construct every presentation row. Subject shape and eye geometry page their
large geometry payloads later, but still front-load multiple complete lineage
tables. These are now the highest-priority Phase 5O.2 lazy-index migrations.

This run preceded the refinement that limits eye-angle resident preload to the
default representation's channels. The repository test covers the refined
resident/default and paged/alternate-representation paths; a second production
trial remains to be recorded after the overlay startup migrations so that a
multi-minute eager-overlay open is not repeated merely to measure the small
trace.

## Sharded Index Cache Control

On 2026-07-24, the headless
`tensorstore_array_access_benchmark` compared a zero-byte TensorStore cache with
the application policy's 67,108,864-byte cache over the mounted Sleepyfish
Cam2010095 archive. It used the selected refined-keypoint `frame_counts` array
as a physical-layout proxy for a future `frame_row_offsets` index:

```text
refined_keypoints_runs/refined_keypoints_sleepyfish_kp_allclips_sharded_20260715_01/frame_counts
```

The array has 1,188,000 `int32` values, 16,384-element inner chunks, and
131,072-element outer shards. Each workload received a new archive context so
the forward, random, and eager reads did not share a TensorStore cache. The
same fixed random sequence was used in both modes. Two complete comparisons
were run in opposite order after a three-request validation trial.

| workload | TensorStore cache | file range reads | compressed file bytes | elapsed observations |
| --- | ---: | ---: | ---: | ---: |
| 700 adjacent two-value reads | 0 bytes | 1,400 | 107,800 | 66.0 ms, 69.9 ms |
| 700 adjacent two-value reads | 64 MiB | 2 | 154 | 24.4 ms, 43.4 ms |
| 100 deterministic random two-value reads | 0 bytes | 200 | 22,844 | 7.5 ms, 9.5 ms |
| 100 deterministic random two-value reads | 64 MiB | 70 | 6,479 | 4.4 ms, 4.2 ms |
| one complete-array read | 0 bytes | 11 | 7,428 | 1.0 ms, 1.2 ms |
| one complete-array read | 64 MiB | 11 | 7,428 | 1.0 ms, 0.9 ms |

The validation trial also showed the physical range behavior directly. The
first two-value read caused two file operations and transferred 154 compressed
bytes, consistent with an indexed-shard lookup plus one independently
addressed inner chunk. Two more reads in that inner chunk caused no additional
file operations with the cache enabled. TensorStore did not download the
complete outer shard for that small range. For the complete-array request it
coalesced the work to 11 file operations rather than issuing one operation per
inner chunk.

The request and byte counts establish the useful result: the bounded driver
cache prevents pathological re-reading for fine-grained forward access,
deduplicates repeated random working-set access, and does not increase physical
I/O for a one-shot eager read. Wall times are supporting observations only.
The zero-cache mode disables TensorStore cache reuse but cannot disable the
macOS/SMB filesystem cache, and this highly compressible count array transfers
only about 7.4 KiB when read completely.

This is a storage-request benchmark, not a trace of the current application
asking for `frame_row_offsets`; that array is not yet published. It does not
measure scheduling, GPU publication, multiple simultaneous products, cache
pressure, or end-to-end frame readiness. It supports keeping a nonzero bounded
TensorStore cache and confirms that small range requests address inner chunks,
while leaving the final cache budget and producer chunk profile open to the
broader workload measurements.

## Conclusions

1. The reported long Sleepyfish startup is real and is primarily synchronous
   full-column mapping I/O, not dense mask conversion.
2. Chunk count and remote filesystem latency matter more than decoded bytes for
   the current mapping columns.
3. Building and retaining the full `rows_by_frame` model consumes a material
   amount of RAM even before presentation-frame masks are cached.
4. Whole-chunk dense-to-sparse conversion remains effective and should be kept;
   the temporary dense allocation is not retained.
5. Contour loading is the largest measured part of the first cold chunk and
   needs its own scheduling/cache policy.
6. App-side lazy paging is required for existing archives. A producer-side
   larger-chunk or sharded tabular layout can improve future archives, but cannot
   replace compatibility with current 1,024-row layouts.

## Implemented Follow-Up

Subject-mask runs that publish `frame_counts` no longer read the six complete
subject/crop mapping columns during repository open. The adapter now:

- opens the row arrays as TensorStore handles;
- builds a compact count/prefix index on first demand;
- reads and validates only physical mapping pages intersecting requested rows;
- retains those pages under explicit byte budgets; and
- preserves a capped compatibility index for detected unordered rows.

Both characterized production runs publish `frame_counts`, select the new lazy
path, and have now been reprobed over the remounted PRFS volume. The results
show a substantial repository-open reduction while moving compact index and
mapping-page work to first demand. They do not yet establish local-storage,
long-traversal, or whole-application open-to-first-presentation behavior.

The shared native `ArchiveContext` now configures a 64 MiB TensorStore cache
pool and all maintained TensorStore array openers use an immutable-read policy
that revalidates metadata and data at array open rather than on every access.
A local headless Zarr v3 indexed-sharding fixture confirms that an identical
second array read causes no additional file-kvstore request or transferred
bytes. This proves the policy and driver reuse path, but it is not a PRFS
latency measurement.

A subsequent mounted-PRFS probe of Sleepyfish Cam2010095 at frame 1024 confirmed
the 67,108,864-byte pool in the production adapter. Repository open took
2,054 ms. First resolution took 12,362 ms, including 10,836 ms to read and build
the one-time 1,188,000-frame count index, 202 ms for the dense mask read, 95 ms
for dense-to-sparse conversion, and 755 ms for contours. Two mapping pages read
549,024 source bytes and then produced two cache hits. The exact warm repeat
took 0.310 ms. That warm result includes Crimson's application-level mask and
mapping caches, so it must not be presented as an isolated TensorStore-cache
speedup. The local metric test is the exact proof of driver request suppression;
the production probe is compatibility and end-to-end latency evidence.

The zero-cache versus 64 MiB control now covers isolated sharded-index forward,
random, and eager access plus transferred-byte deltas. Application-level
multi-product traversal and random seek, cache eviction under pressure, and
long-run resident-memory measurements remain required before the 64 MiB value
is accepted as final.

The motion/tail TensorStore adapter now persists its 16,384-row frame-index
blocks across adjacent windows under a 2 MiB budget instead of reconstructing a
temporary cache for every `resolveWindow` call. The Mac summary reports index
reads, hits, evictions, source bytes, current/peak bytes, and maximum block-read
latency for the next production traversal measurement.

## Verification

The macOS arm64 Release targets `Crimson`,
`subject_mask_overlay_repository_tests`, and
`subject_mask_overlay_repository_probe` build successfully. The new
`tensorstore_array_access_benchmark` also builds and completes against the
mounted production archive. The focused
repository test passes, including deterministic lazy-index, malformed-count,
mapping-page, byte-accounting, sparse conversion, eviction, and frame-cache
release assertions. The complete macOS suite passes 58/58 tests, including the
asynchronous repository-loader lifecycle and resident-versus-paged motion,
tail, and eye-angle repository paths.

## Remaining Phase 5O.0 Work

- collect the equivalent maintained Linux timings against the same archives;
- compare mounted PRFS with a local copy using identical runs and frames;
- repeat Mac shell open-to-first-ready and process-RSS measurements after
  keypoint, subject-shape, and eye-geometry lazy-index migration;
- add equivalent byte/timing telemetry to the generic analysis timeline path;
- measure forward/reverse traversal, random seek, cancellation, and long-run
  peak cache bytes; and
- use those results to refine the initial four-worker/one-speculative baseline
  and add decoded-byte-weighted in-flight budgets.

The next measurement boundaries are a post-overlay-migration Mac shell trial,
Mac shell traversal/RSS, a local copy, and the equivalent maintained Linux run.
The next implementation boundaries are lazy keypoint, subject-shape, and eye-
geometry lineage indexes, contour paging, decoded-byte-weighted speculative
admission, and migration of the remaining native buffers. None depends on the
pending movement coordinate contract.
