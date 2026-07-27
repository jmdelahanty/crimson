# Crimson Phase 5O.1 Portable Data-Access Contract

Date anchored: 2026-07-22.

Status: complete for the portable contract and policy primitives. Native Mac
archive initialization, keypoints, masks, motion, and tail now consume the
application-owned scheduler; remaining product buffers migrate incrementally.

## Purpose

Phase 5O.1 defines a backend-neutral vocabulary for bounded analysis reads. It
allows TensorStore, maintained `ZarrDetectionLoader`, local-file, and future
remote adapters to request the same scientific work without exposing Metal,
OpenGL, CUDA, ImGui, or storage-driver types.

This boundary does not depend on the pending movement coordinate contract. It
identifies work by exact camera-frame range and treats coordinate-bearing fields
as opaque payload. It does not convert, infer, or reinterpret coordinates.

## Portable Types

The implementation is in `src/data_access.h`:

- `SourceIdentity` identifies an archive, product, and selected run;
- `FrameRange` is an inclusive camera-frame range;
- `FieldSelection` is either all fields or a normalized set of named fields;
- `RequestPriority` distinguishes current frame, inspection, retained history,
  visible window, and speculative work;
- `AccessPattern` distinguishes forward, reverse, paused, random-seek, and
  review navigation;
- `DataRangeRequest` combines source, range, fields, priority, access pattern,
  and a nonzero generation;
- `DataRangeResult` reports status and decoded/retained CPU/GPU bytes; and
- `DataCacheBudget` and `DataPayloadCost` express CPU bytes, GPU bytes, and a
  secondary item cap.

Equivalent storage work has the same source, camera-frame range, normalized
field selection, and generation. Priority and access pattern may change while
the underlying work remains equivalent, allowing a queued prefetch to be
promoted when it becomes current demand.

Result states are intentionally not boolean. `Unavailable`, `Missing`, and
`ValidEmpty` describe different scientific and UI outcomes. `Stale` and
`Discarded` are terminal but not publishable. `Unavailable`, `Missing`,
`ValidEmpty`, `Ready`, and `Failed` are publishable so the UI can replace a
pending state with an explicit outcome.

## Queue Policy

`src/data_access_scheduler.h` provides the thread-safe `DataAccessQueue` policy
core:

- the pending queue is bounded;
- lower numeric priority wins, with FIFO order inside one priority;
- higher-priority incoming work may evict the worst pending request;
- lower-priority incoming work is rejected when the queue is full;
- equivalent pending work is deduplicated and may be promoted;
- equivalent active work is deduplicated;
- advancing a source generation cancels older pending and active tokens;
- source cancellation and shutdown cancellation are explicit; and
- completion accounting distinguishes discarded and failed results.

`DataAccessQueue` does not create threads. `DataAccessScheduler` wraps that
policy core with a bounded worker pool, cooperative cancellation, source-idle
draining, one-active-request-per-source admission, and a configurable cap on
active speculative requests. Visible work may use the remaining workers while
speculation is active. This prevents lookahead from consuming the whole pool
without serializing independent visible sources.

The Mac composition root currently shares one 64-request, four-worker scheduler
between optional repository initialization, keypoints, subject masks, motion,
and tail. At most one top-level speculative task may run. Repository
initialization uses visible-window priority, while a presented keypoint or mask
frame uses current-frame priority and therefore wins as soon as a worker is
available. Each source has at most one active request. Standalone buffer tests
retain a private one-worker fallback. Eye, swim-bout, and subject-shape buffers
still own their existing workers; video, crop, and stimulus decoders remain
intentionally separate.

The one-speculative-task setting is an initial application baseline, not a Zarr
constraint. Zarr chunks and TensorStore operations can execute independently.
The intended next refinement is an in-flight decoded-byte budget so several
small timeline prefetches may overlap while a 128- or 256-row dense mask chunk
consumes most or all of the speculative byte allowance.

Cancellation is cooperative. A storage adapter must check the token before
expensive stages and before publication. A driver call that cannot be
interrupted may finish, but its stale result must not be published.

## Cache Policy

`src/data_access_cache.h` provides `ByteBudgetLruCache`. Admission and eviction
are evaluated against hard CPU-byte, GPU-byte, and item budgets. Victims are
ordered by:

1. inactive before active working-set entries;
2. worse request priority before better priority; and
3. least-recent access within the same class.

An incoming low-priority entry is rejected if it would have to evict a better
or active entry. Oversized entries are rejected without changing the existing
cache. Metrics report current and peak bytes/items, released bytes, evictions,
replacements, and admission rejections.

The cache is deliberately not internally synchronized. Its owning repository
or scheduler must guard it, which keeps storage-specific publication atomic and
avoids imposing a second lock order.

## First Adapter Adoption

The native TensorStore subject-mask repository is the first consumer of the
byte-budget cache and the first Phase 5O.2 lazy adapter slice:

- when a run publishes `frame_counts`, opening retains TensorStore handles and
  shapes instead of reading all subject and crop mapping columns;
- first demand reads the compact per-camera-frame counts, validates that their
  sum equals the mask row count, and retains `uint32_t` counts plus a sparse
  4,096-frame prefix index;
- exact subject and crop mapping data is then read in physical chunk-aligned
  pages;
- subject pages have an 8 MiB/128-item budget and crop pages have a
  16 MiB/32-item budget;
- each returned row is verified against exact frame, detection, crop lineage,
  and finite placement metadata before publication; and
- dense mask chunks are still converted to sparse exact pixels, after which the
  temporary dense TensorStore result is released.

Runs without `frame_counts` keep the existing eager compatibility path. If a
run has valid counts but its rows are not in frame order, the adapter detects a
mismatch on demand and may build a compact exact `{frame,row}` compatibility
index under a hard 64 MiB cap. This preserves old unordered fixtures without
making full indexing the normal path.

The frame-count array is trusted as the producer-declared per-frame inventory
after its type, values, and total are validated. Requested nonempty ranges are
also checked against the authoritative row-level `frame_indices`. A malformed
count array fails once, retains no partial index, and is not reread on every
frame request.

The generic motion/tail TensorStore repository now also retains its sparse
frame-index blocks across adjacent timeline windows. Each distinct frame
mapping has a 2 MiB/16-block cache, and motion variants that share the same
mapping share one cache. Repository and Mac summary metrics report block reads,
hits, evictions, source bytes, current/peak retained bytes, and maximum block
read latency. This removes the previous per-window cache reconstruction without
materializing the complete timeline index.

Maintained keypoint runs now use the same indexed range model when they publish
both `frame_counts` and explicit `source_crop_row_ids`:

- opening retains TensorStore handles for keypoint, status, and crop-placement
  columns;
- a validated frame-count prefix index is the only million-frame structure
  retained by the repository;
- resolving one camera frame reads only its exact keypoint row range and the
  crop rows referenced by that range;
- frame, detection, and crop-row lineage are revalidated before publication;
  and
- the portable `KeypointOverlayBuffer` schedules current demand and bounded
  lookahead, caches 24 immutable frame resolutions, and discards stale seek
  generations.

Legacy keypoint runs without either required index/lineage array keep the eager
compatibility adapter. They remain scientifically supported but do not receive
the bounded-open performance guarantee until their producer contract is
upgraded.

## Compatibility and Limits

Phase 5O.1 does not remove or rewrite `ZarrDetectionLoader`. Compatibility
adapters can be migrated incrementally while consuming the same request and
result semantics.

The following remain open:

- add direction-aware request generation and measured concurrency limits;
- migrate remaining large timeline/frame-index paths to persistent bounded
  caches;
- make RLE `indptr`/`present` and ragged-contour pointer metadata lazy where
  production size warrants it;
- define the hybrid full-preload versus paging decision from measured bytes;
- add maintained Linux/Windows adapters; and
- run post-migration local and mounted-PRFS acceptance and long-traversal memory
  tests.

## Verification

`data_access_contract_tests` deterministically covers normalization, result
states, demand-first ordering, bounded-capacity eviction, duplicate promotion,
active duplicate suppression, generation/source cancellation, completion
accounting, CPU/GPU byte budgets, priority-aware LRU eviction, pressure
rejection, replacement, and cache release.

`subject_mask_overlay_repository_tests` covers lazy open, first-demand index
creation, chunk-aligned mapping pages, mapping-page cache hits, unordered-row
fallback, malformed-count failure, and the existing dense-to-sparse prefetch
path. The macOS arm64 headless preset passes 45/45 tests.

`analysis_series_timeline_repository_tests` verifies cold frame-index block
reads, warm reuse across repeated windows, source-width byte accounting, and
the 2 MiB retained-cache ceiling for both motion and tail mappings.

`keypoint_overlay_repository_tests` covers the maintained lazy indexed path,
exact crop lineage, legacy eager fallback, coordinate conversion, and the
scheduler buffer's cache and discontinuity behavior.

`apple_analysis_repository_loader_tests` verifies that archive readiness is
published first and that each requested product is delivered as an independent
completion event through the bounded scheduler.
