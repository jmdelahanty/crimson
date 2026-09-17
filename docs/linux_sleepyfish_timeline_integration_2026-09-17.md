# August Sleepyfish Linux timelines — 2026-09-17

## Result

The four August 6 Sleepyfish recordings now show maintained eye-angle, motion/speed,
and swim-bout streams alongside rolling-clip camera playback on Linux/NVIDIA.
This extends the [rolling-clip adapter](linux_sleepyfish_rolling_validation_2026-09-16.md)
and [canonical detection integration](linux_sleepyfish_detection_integration_2026-09-16.md).

The Analysis Timeline panel displays left/right eye angles and vergence, filtered
and raw speed, bout intervals with core intervals, and the detector-response trace.
The time marker follows the camera's parent frame. The panel reads the original
analysis archive; no exported table is required. Storage, selectors, and video
files were not modified and no video was re-encoded.

Source worktree:
`/home/delahantyj@hhmi.org/gitrepos/crimson-linux-priority-20260916`,
branch `codex/linux-priority-20260916`, based on
`06ad87202dd9d057af3bef02c8557650f71bea22`.
The rolling, detection, and timeline integrations are recorded together in the
`Integrate August Sleepyfish clips and canonical analysis on Linux` commit on
this branch. The validation evidence below was collected before that commit;
local build artifacts and recordings are not included in the repository.

## Selection and identity

- A background `CanonicalTimelineSession` resolves small per-group selector
  metadata. Nonempty `latest` and `latest_complete` must agree. Selected run
  publication must be complete and selector-eligible.
- Eye schema v7 compact dense data is supported alongside the existing v5 reader.
  The v7 gate checks dense-array dimensions, frame-time support, channel
  availability, exact bound keypoint paths, and bound mask/shape lineage.
  Explicitly bound upstream runs need completion, not independent selector
  eligibility; they are not silently promoted to parent selectors.
- Motion opens one scope-qualified run and track (the selected August offline
  run, track 0), not independent latest runs from several scopes. Camera mapping
  uses `source_acquisition_frame_index`, not the sample's row number.
- Bouts use the exact selected compact v8 run and its authoritative reference
  frame axis from the bound motion run. Run/scope/track, advertised motion
  manifest digests, reference path, row count, and shape must agree.
- Motion/bout source disagreement fails that pairing closed. An eye failure does
  not suppress independently valid motion or bouts. Products have separate
  pending, ready, empty, and error states. Zero-interval bouts remain valid empty
  data and may still carry a drawable detector trace.
- The tests/probes validate source/frame mapping and compare real output values.
  This is not full cryptographic verification of every payload: advertised
  digests are compared, not recomputed across million-row arrays. Axis ordering
  is checked in accessed windows/cache blocks, not by a full eager scan.
  Motion checks track-sample-key shape and instance-key array existence; it does
  not validate all instance-key values/domain lengths or perform detection joins.

## Storage-aware loading

The eye and bout buffers now use the same shared data scheduler as motion.
Timeline work uses visible-window priority; the scheduler's reserved
current-frame worker remains available for playback/detection demand.
Cancellation/generations prevent old seek results from publishing.
The UI snapshot methods are cache-only.

Production defaults are 4,096-frame pages, 2,048-frame steps, at most 1,200
published points per trace, and three cached pages per product. Full-series
eye/motion preload is disabled. Bout-detector samples are now window reads,
rather than an initial full detector-array load. The follow control offers a
1–30 second half-window, within the interior coverage of this 30-FPS page policy.
Plots can be inspected locally; panning does not fetch a new recording region.
Use playback navigation to move the loaded window.

One archive context and its 64 MiB TensorStore cache are shared by these three
timeline readers. This is separate from the detection reader's context.
The new timeline path reads per-group/run/array metadata and does not itself
parse the consolidated root JSON. The existing GUI/detection opening route
still incurs its large root-metadata/offset costs described in the detection report.

Important distinctions:

- Small channel/candidate/signal/track indexes are read at open.
- All selected-run bout interval columns (start/end/core/gap and selection IDs)
  are also read at open, off the UI thread. Filtered interval vectors are retained
  with a 64 MiB estimated retained-vector cap. Actual August retained estimates
  were 5.25–7.5 MiB.
- Motion lazily caches 16K-entry frame-index blocks. Bout reference-axis lookup
  uses bounded binary-search reads plus the requested window.
- Logical window/point/page bounds do not cap physical compressed chunks read
  by storage, transient opening allocations, or total GUI RSS. Page retention is
  item-bounded, not one global hard-byte admission budget.
- Close/reopen cancels stale work but may wait for an already-running I/O call;
  this is not a strict UI-latency guarantee.

## Verification

Native Ubuntu 24.04.4 / RTX A6000 environment and dependency limitations are as
documented in the preceding detection report.

- Complete build passed (131 targets in the full incremental pass); subsequent
  small plot-alignment rebuild passed.
- Complete serial CTest: **94/94 passed**, zero failures.
- Four focused tests (eye repository, bout repository, canonical session, shared
  scheduler) passed ten consecutive runs each. Eye/bout buffer tests also passed
  ten consecutive runs.
- Added tests cover v7/v8 positive reads, wrong eye-source paths, mismatched motion
  manifest identity, zero-bout candidates, incomplete publications, independent
  product errors, close/reopen generations, cache-only snapshots, reader
  exceptions, source isolation, stale seeks, and a blocked timeline read not
  occupying the reserved current-frame worker.

The acceptance probe uses three 256-frame pages centered near frames 54,000,
2,592,030, and the final frame 2,937,603, with at most two retained pages.
Every product returned ready/valid-empty windows on all four cameras.
Eye payload reads totaled 768 rows per recording; motion and detector reads
totaled 738 for camera 94 (real acquisition gaps), 768 for the others.

An independent direct-array verifier bypasses timeline repositories, their
frame-index caches, and decimation helpers. It checks sampled eye channel values
and times; speed values and times at binary-searched acquisition IDs; detector
values; and returned bout table row identities/start/end.

| Camera | Direct comparisons passed | Timeline open, warm host | Probe peak RSS |
| --- | ---: | ---: | ---: |
| 2010093 | 151 | 632 ms | 111,432 KiB |
| 2010094 | 111 | 626 ms | 112,192 KiB |
| 2010095 | 127 | 667 ms | 115,484 KiB |
| 2010096 | 103 | 686 ms | 110,076 KiB |

These RSS figures include the independent verifier and its separate 16 MiB
cache, not the video decoder or full GUI. They are observations, not guarantees
of cold-NFS latency or a universal memory ceiling.

GUI verification:

- All four automatic-discovery playback smokes crossed 54,000 and ended at
  54,010 with matching canonical box/query/video frames and finite timeline
  windows ready during playback.
- All four authenticated GPU captures settled at exactly 54,000 for 60 stable
  frames with a full 100-slot video buffer and all three products ready.
  Images were visually inspected: video/box, eye traces, speed, bout lane, and
  detector plots were present. The final plot-alignment adjustment was separately
  rebuilt and captured.
- Camera 93's unequal late-clip boundary at 2,592,030 and the required
  GoodCopBadCop 0:300 control both passed on the final binary.
- Capture markers now contain canonical product identities, window bounds,
  finite-point counts, and interval counts. The script validates these on the
  canonical analysis route instead of requiring unrelated legacy eye-mask overlays.

Final GUI binary SHA-256:
`223f3fb95bcc0fc22a301e86577ec675b7bcbe61a3f98414c116583ccf609435`.

## Limits and next work

This is the first selected-stream integration, not the entire analysis UI.
Canonical keypoint/heading overlays, masks/shape overlay joins, tail traces,
quality/stimulus products, product/representation/candidate selection, and
multi-track selection remain separate work. Motion repositories return headings
and positions, but this panel currently plots speed. There is no plot-click seek
or write/edit/export workflow.

The earlier observed non-boundary paused seek problem (request 54,010 settling
at 54,000) has not been fixed or qualified by this change. Passing live playback
and exact clip-start captures do not establish arbitrary paused-seek correctness.
No long-session/endurance or colleague-machine portability qualification was done.
Workstation-specific TensorStore reuse and mixed FFmpeg ABI warnings remain.

The first raw-verifier attempt had a bug in its own handling of nonzero
TensorStore slice origins (one mismatch and three crashes). It was corrected;
the final raw comparisons all passed. The initial GUI capture loaded the streams
but failed the old legacy-overlay-only script validator; marker/script support
was updated and subsequent all-four captures passed. Failed artifacts are retained
and are not counted as acceptance passes.

## Reproduction and local evidence

Keep each complete recording directory together: rolling clips, clip index,
sidecars/frame-index manifests, and the analysis archive. From the active build:

```bash
release/redgui --zarr /path/to/2026_08_06_19_13_35_cam2010093/zarr/2026_08_06_19_13_35_cam2010093_analysis.zarr
```

Open the Analysis Timeline panel if it is hidden in the saved workspace.
The rolling index is discovered automatically.

Read-only production-window + independent raw-value probe:

```bash
build/linux-trt10-cuda12.4-release/august_timeline_window_probe \
  /path/to/2026_08_06_19_13_35_cam2010093/zarr/2026_08_06_19_13_35_cam2010093_analysis.zarr
```

This acceptance tool deliberately targets this batch's 2,937,604 frames at
30 FPS; it is not a generic recording importer.

Local evidence, not shipped in a clone:
`/tmp/crimson-august-timelines-20260917-hRbU7Q/`.
It contains build/test logs, final `cam*-qualified.json` source comparisons,
`boundary-*.log`, `reference-final/` all-four captures/markers, and the final
aligned-axis camera 93 capture in `reference-aligned/`.
The initial failed capture is preserved in
`/tmp/crimson-phase5l-reference.pJMm3m/`.
