# Phase 5O NVIDIA Scheduler Adoption

Date: 2026-07-29

Status: first Linux/Windows application-entry compatibility slice complete;
Phase 5O remains open.

## Scope

The shared Linux/Windows application entry point now creates the same bounded
data-access scheduler policy used by the native Mac application:

- queue capacity: `64` requests;
- worker count: `4`;
- per-source concurrency: `1`; and
- reserved current-frame workers: `1`.

`ZarrDetectionLoader` accepts that application-owned scheduler. Refined
subject-mask cache misses submit one named physical mask-chunk request at
`CurrentFrame` priority for the displayed chunk and `Speculative` priority for
lookahead chunks. A discontinuous frame request advances the subject-mask
generation so queued work from the previous seek can be cancelled and stale
completion cannot be published.

This is a compatibility migration, not a subject-mask repository rewrite. The
existing exact TensorStore chunk decode and sparse contour cache remain the
service callback. Paused exact-frame inspection retains its deterministic
synchronous settle behavior. `ZarrDetectionLoader` also retains its original
single prefetch worker as a fallback for standalone consumers that do not
inject the shared scheduler.

The Apple and NVIDIA shells now use one backend-neutral scheduler diagnostic
formatter. It reports queue totals plus queue-wait and callback-service timing
by priority and source using `[AppleDataScheduler...]` or
`[NvidiaDataScheduler...]` prefixes.

## Build Ownership

The NVIDIA source glob no longer recompiles the shared data-access contracts or
TensorStore repository implementation objects that are already owned by their
static libraries. `redgui` links those libraries directly. The prebuilt Linux
TensorStore package carries its digest implementation in its link response but
does not export a separate CMake target, so Crimson links the optional digest
target only when that target exists. The fetched macOS TensorStore build still
uses its exported target.

## Verification

The clean local macOS release configuration built `Crimson` and
`data_access_contract_tests`. The focused portable/runtime suite passed `4/4`:

- `session_lifecycle_tests`;
- `recording_open_workflow_tests`;
- `data_access_contract_tests`; and
- `coordinate_catalog_contract_tests`.

An isolated NVIDIA tree on `ws1` completed all `181` build steps, including
`redgui`, scheduler tests, and legacy dependency probes. The rebuilt
`data_access_contract_tests` passed. The generated Ninja graph contains no
direct `redgui` compilation of `src/data_access_scheduler.cpp`; the contract is
linked once through `crimson_data_access_contracts`.

The authenticated GoodCopBadCop NVIDIA smoke reached frame `300`:

```text
[PlaybackSmoke] PASS start_frame=0 end_frame=300 presented_frame=300 presented_count=352 elapsed_s=3.00245
```

An explicit subject-mask smoke over frames `5000:5300` also passed. A far-seek
smoke over frames `50000:50300` passed in `3.218` seconds with `16` accepted
mask requests, one priority promotion, `9` cancelled requests, `2` discarded
stale completions, and no failures. The promoted current-frame request waited
`33.8` ms in the queue and used `206.6` ms of repository service time. Small
monotonic frame gaps within the active lookahead window are treated as ordinary
forward/reverse playback because the logical playback clock may advance faster
than the GUI render rate. A far jump or direction reversal advances the
generation and cancels stale work. The deterministic blocking scheduler tests
cover the stricter current-frame reservation and overtaking cases.

## Acceptance Boundary

The shared `red.cpp` source is the Linux and Windows entry point, so the
scheduler ownership and subject-mask adapter compile into both distributions.
This checkpoint includes a real NVIDIA/Linux build and GUI playback smoke. It
does not include a native Windows build or runtime smoke.

Still open:

- migration of the remaining legacy mask/timeline workers;
- direction-aware timeline read-ahead;
- decoded-byte-weighted in-flight admission;
- full-duration CPU/GPU memory attribution;
- native Windows execution; and
- the separate crop-v2 exact-schema read/performance harness.
