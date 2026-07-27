# Crimson Phase 5O Shared Bounded Data Access and Scheduling

Date anchored: 2026-07-22.

Status: implementation in progress. Phase 5O.1's portable request, result,
queue-policy, and byte-budget primitives are complete. Phase 5O.2 now includes
subject-mask lazy mapping and a responsive native Mac archive-open lifecycle.
Phase 5O.3 routes subject masks, motion, and tail through one application-owned
scheduler. Phase 5O.4 has begun with budgeted small-series preload for motion,
eye angles, and tail, plus a bounded archive-session TensorStore cache shared by
the maintained native repositories. Remaining repository migrations,
byte-weighted in-flight admission, and mounted-storage cache measurements are
not complete.

## Purpose

Phase 5O turns the useful parts of the current Linux and native Mac data paths
into one cross-platform read architecture. The target combines:

- the native Mac path's bounded TensorStore range reads and immutable published
  windows;
- the maintained Linux mask path's demand-first asynchronous prefetch,
  cache-only playback reads, and willingness to omit a late overlay rather than
  stall the video presenter; and
- storage-specific cached representations, including converting a temporary
  dense subject-mask chunk to sparse exact pixels and releasing the dense
  allocation.

This is not a Mac-only performance layer. The range contract, scheduler,
priority rules, cache accounting, and availability states must be portable and
usable by Linux, Windows, and macOS. Render and decode backends remain platform
specific consumers.

## Decision

The future default for large analysis products is bounded, asynchronous,
demand-prioritized paging. Eagerly materializing an entire selected series
remains an allowed optimization only when its estimated decoded size fits an
explicit budget.

Row count alone is not a sufficient policy. One million scalar rows may be
reasonable, while one million dense mask rows are not. The selection decision
must consider decoded bytes, number of requested fields, number of source
variants, expected reuse, and the active workflow.

The design must not introduce a second application model for Apple platforms.
Storage adapters may differ, but all builds should converge on the same portable
request, result, cancellation, priority, and cache-budget contracts.

## Why Full-Series Loading Is Not the General Contract

The maintained movement path discovers track kinematics at archive open, starts
a deferred background load on user request, and then keeps the selected arrays
and a `frame -> row` hash map in memory. After the load, a movement trail is a
fast in-memory walk over at most 600 prior points. That behavior is responsive,
but it is not future-chunk prefetch during playback.

A representative one-million-row movement series can use tens of MiB for its
column vectors and tens of MiB more for a node-based frame lookup map. Depending
on the selected fields, allocator, and hash-table load factor, one series can
approach 100 MiB. Loading several tracks, filtered/raw variants, eye products,
and other analyses with the same policy can turn a convenient local optimization
into process-wide GiB growth.

Full materialization remains appropriate for small event tables, compact
metadata, and analysis products whose measured decoded footprint is below the
configured budget. It must not be an implicit consequence of opening a Zarr
archive or displaying one frame.

## Current Baselines to Preserve

### Maintained Linux masks

The maintained mask path already has the essential playback policy:

- physical Zarr chunks are loaded and converted to the cached display form;
- dense and compact mask caches are bounded;
- a bounded worker queue suppresses duplicate in-flight reads;
- the current chunk and adjacent chunks are prefetched;
- playback requests a broad forward frame window and performs cache-only mask
  lookup; and
- paused inspection may block to settle the exact requested mask.

The important contract is not the current hard-coded 1,024-frame lookahead. It
is that current-frame demand outranks speculative work and a cold optional
overlay cannot block video presentation.

### Maintained Linux movement series

Movement data is cheap to discover, deferred until requested, loaded in a
background stage, and then fully resident. This remains a valid small-series or
explicit-preload policy. It is not the default for every million-row analysis
product.

The maintained Linux timeline path is not one worker per displayed stream.
Eye-angle and tail-kinematics products are normally loaded into resident vectors
during archive loading. Movement uses one staged `std::async` load at a time and
then becomes resident. Its apparent playback consistency therefore comes from
paying an earlier load and RAM cost, not from concurrent per-timeline paging.

### Native Mac timelines

The native motion, tail, and eye timeline path publishes immutable 4,096-frame
windows asynchronously, advances pages in 2,048-frame steps, limits display
output to 1,200 points per trace, and retains three completed pages. When a
selected small series fits the session preload budget, those windows resolve
from resident native-precision arrays instead of issuing later storage reads.
Otherwise only intersecting TensorStore slices are read. Motion and tail use
the shared scheduler and persistent frame-index blocks; eye and swim-bout
retain their earlier feature-owned workers.

This is the preferred range-oriented foundation, but the current implementation
has limitations Phase 5O must address:

- cache capacity is expressed as page count rather than decoded bytes;
- eye, swim-bout, subject-shape, and other unmigrated buffers still own
  independent workers without shared back-pressure;
- a page is requested only when the UI reaches its key; scheduling is not yet a
  general direction-aware policy; and
- RLE/ragged mask metadata and several other repositories still have eager
  metadata paths to migrate.

The movement-trail presentation remains separately blocked on an accepted
coordinate/provenance contract. Phase 5O must not reinterpret millimeters as
camera pixels to make that overlay appear.

## Portable Request Contract

The exact type names remain an implementation detail, but the shared boundary
should express this information:

```cpp
struct DataRangeRequest {
  SourceIdentity source;
  FrameRange frames;
  FieldSelection fields;
  RequestPriority priority;
  AccessPattern access_pattern;
  uint64_t generation;
};
```

Required request semantics:

- exact source/run identity and requested fields;
- camera-frame range, not an unqualified storage-row range;
- current-frame, inspection, history, visible-window, and speculative priority;
- forward, reverse, paused, random-seek, and review-navigation access patterns;
- a generation that makes pre-seek and pre-reload results stale; and
- explicit cancellation and completion states.

Published results must remain immutable and distinguish unavailable, pending,
missing, valid-empty, ready, stale/discarded, and failed states. Consumers must
never observe a partially filled page.

## Shared Scheduler

One application-owned scheduler should coordinate analysis reads across visible
repositories. It should provide:

- a bounded global queue and a configurable cap on concurrent storage work;
- demand-first priority for the exact presented frame;
- retained history for trails and short reverse navigation;
- modest direction-aware read-ahead during sequential playback;
- duplicate suppression for equivalent source/range/field requests;
- generation cancellation after seeks, source changes, and archive reloads;
- fairness that prevents a large background timeline read from starving the
  current mask or inspection frame; and
- no renderer, decoder, ImGui, Metal, OpenGL, or CUDA dependency.

The first Mac integration uses a 64-request, four-worker pool for subject masks,
motion, and tail. It admits at most one speculative task and at most one active
task per source. Three demand-capable slots therefore remain available even
when lookahead is running. This preserves concurrent visible service for the
three migrated sources while bounding top-level read pressure.

That count is an initial configuration for production measurement, not the
final storage policy. More workers can reduce latency on independent reads, but
can also increase contention over VPN or Wi-Fi. A future in-flight decoded-byte
budget should allow multiple small timeline prefetches while limiting large
dense-mask chunk decodes.

## Cache Policy

Application caches should have explicit CPU and GPU byte budgets. Item-count
limits may remain as secondary safety bounds, but are not sufficient by
themselves.

The native TensorStore archive context now also owns one 64 MiB cache pool for
the lifetime of an open archive. Every maintained TensorStore repository opens
its arrays through one shared read-only spec with metadata and data revalidation
scoped to array open. Published runs are immutable during a Crimson session, so
this permits TensorStore to reuse decoded chunks and Zarr v3 shard indexes
without issuing a filesystem read merely to revalidate an unchanged chunk on
each access. Closing or replacing the archive releases the context and its
cache. The 64 MiB value is an initial measured-policy candidate, not a final
universal storage constant.

This driver cache is separate from Crimson's application caches. TensorStore
retains bounded decoded storage chunks and shard indexes; Crimson retains
consumer-ready pages, sparse mask pixels, frame indexes, and GPU resources.
Both levels remain bounded because they solve different reuse problems.

The cache should retain the representation that best matches the consumer:

- subject masks: sparse exact pixels, row spans/RLE, contours, or uploaded GPU
  textures; temporary dense TensorStore chunks are released after conversion;
- scalar timelines: selected source columns for the active page plus the
  decimated immutable display window;
- movement trails: current samples plus the requested historical duration, with
  optional directional headroom;
- frame indices: a bounded persistent block index, or a persisted archive index
  when the storage contract provides one; and
- small event and metadata tables: complete materialization when their measured
  footprint is within budget.

Eviction should be weighted LRU or an equivalently explicit policy that accounts
for bytes, request priority, and active working-set membership. Hidden windows
and disabled overlays must not continue expanding their caches.

## Archive Opening and Indexing

Selecting an archive should make the video and shell responsive after reading a
minimal catalog: run names, schema versions, shapes, provenance, coordinate
declarations, and TensorStore handles. Million-row mapping and placement arrays
must be opened or indexed lazily on background work when their feature is
requested.

Until storage contracts publish an efficient persisted frame-to-row index,
adapters may maintain a bounded persistent block index. Repeating a binary
search that rereads the same remote frame-index blocks for each page is not the
target behavior.

Initialization and first-demand work must expose separate telemetry so a slow
archive catalog, row index, first physical chunk, conversion, and GPU upload are
not reported as one unexplained application hang.

## Rollout

### Phase 5O.0: Characterize and instrument

- record Linux and Mac archive-open, first-demand, warm-page, seek, and playback
  timings on the same local and PRFS recordings;
- report bytes read, decoded bytes retained, queue delay, resolve time, cache
  hits, evictions, cancellations, and peak resident cache bytes; and
- separate repository discovery, row-index construction, data read, conversion,
  and GPU publication in logs.

Status: in progress. Mac subject-mask instrumentation and the first Sleepyfish
and GoodCopBadCop pre/post-migration mounted-PRFS measurements are recorded in
`docs/crimson_macos_phase5o0_data_access_characterization.md`. Maintained Linux,
local-storage, timeline, traversal, and long-run memory baselines remain open.

### Phase 5O.1: Define the portable boundary

- add backend-neutral request, result, priority, generation, and byte-budget
  contracts;
- add deterministic scheduler and cache-policy tests without TensorStore or a
  graphics backend; and
- retain compatibility adapters for unmigrated `ZarrDetectionLoader` paths.

Status: complete. The implementation and its current integration boundary are
documented in
`docs/crimson_macos_phase5o1_portable_data_access_contract.md`. The tested
`DataAccessQueue` is the scheduling policy core; `DataAccessScheduler` supplies
the first application-owned worker layer described in Phase 5O.3.

### Phase 5O.2: Make initialization lazy

- move large subject-mask and analysis row-index construction off the UI thread;
- publish progress and explicit unavailable/failed states; and
- keep the GUI event/render loop responsive while repositories initialize.

Status: in progress. Subject-mask runs with a valid `frame_counts` inventory now
open mapping arrays as TensorStore handles, build a compact count/prefix index
on first demand, and read exact physical mapping pages through 8 MiB subject and
16 MiB crop byte-budget caches. Runs without the inventory retain the eager
compatibility path, and detected unordered rows have a capped 64 MiB exact-index
fallback. RLE/ragged pointer metadata and other repositories remain to migrate.
The generic motion/tail TensorStore repository also keeps 16,384-row frame-
index blocks across windows under a 2 MiB/16-block budget; source variants with
the same mapping share that cache.

The native Mac application now treats archive readiness and analysis-product
readiness as separate milestones. A coordinator validates the archive, then
submits independent product jobs to the shared bounded scheduler. Archive
readiness is an internal transaction milestone; it does not release the
interactive workspace. The event/render loop keeps the loading modal responsive
and adopts each completed product between frames, but analysis presentation and
playback controls remain gated until every scheduled product reaches a terminal
state. Motion, eye-angle, and tail initialization share one serialized
trace-preload job so their combined 128 MiB budget cannot be oversubscribed.
Smoke and reference modes use the same readiness boundary before assertions.
Decoder opens and short main-thread adoption work remain outside the scheduler,
and active TensorStore calls remain cooperatively rather than forcibly
cancellable.

The first whole-application Sleepyfish trial remained responsive but took
154.9 seconds to publish the former whole bundle. Keypoints consumed 53.0
seconds, subject shape 59.5 seconds, and eye geometry 31.9 seconds because those
adapters materialized complete million-row placement or lineage products.
Maintained keypoint runs with `frame_counts` and `source_crop_row_ids` now retain
a compact prefix index plus TensorStore handles and read exact keypoint/crop
rows on demand. Legacy keypoint runs keep the eager compatibility adapter.
Subject-shape and eye-geometry lazy-index migration remain priorities; a
responsive shell is not treated as acceptance of their open latency.

A mounted Sleepyfish run during the earlier archive-first checkpoint confirmed
that the loader is concurrent (`11` accepted jobs, four peak-active workers)
and reduced the former serialized wall time to about 111.6 seconds. The current
strict readiness policy changes when the workspace is released, not how those
repository jobs overlap. The remaining latency was inside repository adapters
rather than the GUI thread: keypoints took 80.8 seconds, subject masks 80.9
seconds, eye geometry 84.7 seconds, and subject shape 99.3 seconds while those
operations overlapped. The keypoint
`frame_counts` array is only 1,188,000 `int32` values in ten outer shards; its
latency and the subject-mask 49.4-second contour-open phase demonstrate that
per-array metadata round trips over the mounted VPN path dominate logical byte
size. A consolidated run manifest, persisted frame-row offsets, and deferred
TensorStore opens are therefore the next initialization optimizations.

Normal archive sessions keep the playback clock paused while the responsive
analysis-loading modal reports product initialization. The modal closes when
every scheduled product reaches a terminal state, but playback remains paused
until the user presses Play. The modal cannot be dismissed into the background.
Optional products may finish unavailable without blocking readiness; a failed
required product blocks the session and reports the failure. Presentation-demand
reads remain gated until initialization finishes, so first-frame mask and
overlay fetches do not contend with repository opening.

The recording-open lifecycle around this work is now portable. A shared
controller owns the active session transaction, per-product timing, terminal
failure/cancellation transitions, and generation-checked settlement of child
loader progress. Both the macOS and NVIDIA shells call that controller while
retaining platform ownership of archive access, decoders, repository results,
worker execution, and graphics resources. This completes a bounded workflow
extraction; it does not complete the remaining lazy repository migrations in
Phase 5O.2.

### Phase 5O.3: Introduce shared scheduling and budgets

- route native timeline and mask range work through the shared scheduler;
- add demand priority, direction-aware read-ahead, generation cancellation, and
  byte-accounted eviction; and
- prevent hidden or superseded work from competing with current presentation.

Status: in progress. The portable scheduler worker layer is implemented with
demand-first ordering, generation cancellation, source isolation, bounded
speculation, shutdown draining, and aggregate metrics. Native archive
initialization, keypoints, subject masks, and the generic motion/tail timeline
buffer use it, and the Mac application injects one shared four-worker instance.
Deterministic tests verify that three visible sources run while one speculative
source is active, one source cannot occupy multiple workers, product
completions publish independently, and keypoint seeks discard stale
generations. Remaining buffers, direction-aware timeline prefetch, queue-delay
telemetry, and decoded-byte-weighted in-flight admission remain open.

### Phase 5O.4: Converge maintained adapters

- adapt maintained Linux/Windows mask and movement readers to the portable
  scheduling boundary without a big-bang loader rewrite;
- retain explicit full-series preload when it fits the selected budget; and
- compare portable TensorStore and compatibility adapters on identical fixtures.

Status: in progress. Motion, eye-angle, and tail TensorStore repositories now
share a 128 MiB per-session preload budget during native Mac archive opening.
The motion and tail repositories estimate and retain every field of the
selected default source, including position, speed, heading, values, masks, and
sample times. The eye-angle repository retains only the channels needed by its
default representation plus optional frame times; alternate representations
remain paged. Values remain in their native stored `float` or `double`
representation until a display window is published. If the estimate does not
fit the remaining budget, the repository keeps its bounded paged path;
non-default source variants are not duplicated in RAM. Metrics report candidate
bytes, retained bytes, preload time, and resident versus paged window resolves.
All maintained native TensorStore array adapters now also share the archive's
64 MiB cache pool and open-scoped immutable-read policy. A headless sharded
Zarr v3 test verifies that the first read reaches the file kvstore while an
identical second read adds neither a file request nor transferred bytes.
`[AppleTensorStore]` reports the configured pool and revalidation policy at
archive adoption. Headless tests cover both preload decisions, the asynchronous
archive-loader lifecycle, and repeated driver-cache reuse. A first mounted-PRFS
Cam2010095 frame probe confirms production compatibility and the configured
pool. A headless mounted-PRFS comparison over the sharded 1,188,000-element
keypoint `frame_counts` array now provides the zero-cache control: 700 adjacent
two-value reads required 1,400 file operations and 107,800 compressed bytes
without the TensorStore cache, versus two operations and 154 bytes with 64 MiB.
For 100 fixed random reads the counts were 200 operations/22,844 bytes versus
70 operations/6,479 bytes. A complete eager read was unchanged at 11
operations/7,428 bytes. The cache therefore suppresses repeated inner-chunk
and shard-index reads without penalizing one-shot eager I/O. This isolated
storage benchmark uses `frame_counts` as a physical-layout proxy for future
persisted offsets; application-level traversal, random seek, cache pressure,
and Linux/Windows compatibility adapters remain open.

The canonical-detection adapter benchmark now supplies the previously missing
application-level random and full-traversal storage evidence on the Mac mount.
Five matched repetitions compared Palette's regular 1 MiB control and
128 KiB-windowed/1 MiB-eager/8 MiB-sharded hybrid at zero and 128 MiB
TensorStore cache limits. Exact schema/codec/consolidated-metadata validation,
decoded-value equivalence, one retained offset read, UI random latency, and
traversal throughput passed. The hybrid cut zero-cache UI and traversal file
bytes by roughly 7.2-7.5 times. Promotion remains blocked by one-time offset
latency variance and first-pass eight-column random-read outliers. The full
result and prefetch/cache-sweep follow-up are documented in
`docs/crimson_macos_phase5o4_canonical_detection_storage_benchmark.md`.

The follow-up 80-process mounted-Mac cache/read-ahead matrix is also complete.
It exercised 3,500-frame forward and reverse traversal, 50 pages through a
32-page presentation cache, three concurrent UI fields, and deterministic seek
cancellation. Every frozen gate passed. A 16 MiB TensorStore cache cut median
first-phase hybrid transfer from 3,826,500 to 76,530 bytes; 32, 64, and 128 MiB
provided no further transfer reduction. Zero speculative read-ahead had no
post-warmup deadline misses, so 16 MiB with only a one-page asynchronous demand
lead is the paired full-analysis fixture candidate. The current 64 MiB
production setting remains unchanged. Process-first offset reads still had a
135.2 ms median and 143.3 ms p95, so initialization remains a separate open
problem. Detailed results are in
`docs/crimson_macos_phase5o4_prefetch_cache_benchmark.md`.

The paired full-analysis experiment is frozen in
`docs/crimson_macos_phase5o4_full_analysis_fixture_contract.md`. Stage 1 keeps
the production 64 MiB archive cache fixed while regular and hybrid fixtures
exercise the same explicit detection run, nondetection products, recording,
startup, seek, traversal, cancellation, and shutdown sequence. Stage 2 compares
16 MiB with 64 MiB only if the hybrid passes. The normal macOS application now
has the canonical-detection repository, bounded 70-frame paging, shared scene
adapter, and fail-closed `--detection-run` override. Palette may build the
paired fixtures. The full-duration Stage 1 runner and reduction are now
complete: all ten trials passed correctness and 700 FPS traversal, but the
hybrid failed the frozen first-overlay, absolute peak-RSS, and `0.25x`
traversal-byte gates. Its observed median traversal ratio was `0.606x`. Stage 2
does not run, the profile remains unpromoted, and the next Crimson work is
current-frame scheduler isolation plus full-duration memory attribution. The
result is in
`docs/diagnostics/crimson_macos_phase5o4_full_duration_stage1_2026-07-26.md`.

### Phase 5O.5: Cross-platform acceptance

- validate cold open, warm playback, forward/reverse traversal, random seeks,
  source changes, archive reload, and shutdown cancellation;
- run local-storage and mounted-PRFS production trials;
- verify bounded CPU/GPU cache memory over long traversal; and
- preserve scientific frame, coordinate, provenance, and missing-data behavior.

## Gate

Phase 5O is complete only when:

- selecting a large archive does not require full materialization of optional
  million-row products before the shell can remain responsive;
- no optional analysis cache miss blocks the video render/presentation thread;
- paused exact-frame inspection can settle deterministically or report a clear
  failure;
- current demand outranks lookahead and stale queued work is cancelled after a
  discontinuity;
- cache memory is bounded and reported in bytes across long sequential and
  random-seek trials;
- small-series full preload and large-series paging follow one documented,
  testable budget policy;
- Linux, Windows, and macOS consume the same portable scheduling contract, with
  explicitly deferred runtime validation where hardware is unavailable; and
- no adapter changes frame identity, coordinate meaning, provenance, archive
  schema, or write behavior.

## Related Documents

- `docs/crimson_zarr_cache_miss_design_notes.md`
- `docs/crimson_mask_overlay_async_buffering_notes.md`
- `docs/crimson_threading_architecture_notes.md`
- `docs/crimson_subject_mask_sparse_index_storage_concept.md`
- `docs/crimson_macos_phase5i_motion_tail_timelines.md`
- `docs/crimson_macos_phase5m_read_only_zarr_boundary_plan.md`
- `docs/crimson_macos_phase5o0_data_access_characterization.md`
- `docs/crimson_macos_phase5o1_portable_data_access_contract.md`
