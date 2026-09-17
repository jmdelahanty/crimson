# Linux August Sleepyfish detection integration — 2026-09-16

Follow-up: [the September 17 timeline integration](linux_sleepyfish_timeline_integration_2026-09-17.md)
adds selected August eye-angle, speed, and swim-bout streams. The remaining-work
inventory below records the state at the end of this earlier detection phase.

## Result and scope

The four August 6 recordings now display canonical raw detection boxes on Linux/NVIDIA through the existing rolling-clip playback machinery. Scores, classes, and source-scoped canonical row IDs are carried with each detection. This builds on [the rolling-index adapter](linux_sleepyfish_rolling_validation_2026-09-16.md); that earlier document describes the video-only phase.

This is a locally implemented and tested integration on `codex/linux-priority-20260916`, based on `06ad87202dd9d057af3bef02c8557650f71bea22`. It does not qualify all analysis products, arbitrary paused seeks, long-session endurance, or a portable colleague distribution. No recording data, selectors, exports, or encoded video were changed.

## Loading and identity contract

- `NvidiaDetectionRepository` bridges the validated canonical TensorStore repository into NVIDIA's `DetectionRepository` interface.
- Rolling sessions resolve a single exact raw run from the small `detect_runs/zarr.json`: nonempty `latest` and `latest_complete` must agree. A nonempty legacy-selected run must also agree. This metadata and the clip-index schema are inspected only when the session signature changes, not each rendered frame.
- The production selector permits raw fallback only when there is no approved refined authority. The bridge accepts only the requested canonical raw surface; it does not silently fall back to legacy data.
- Before becoming ready, it checks the validated manifest's recording identity against the clip index, total camera frames against the indexed video, and source dimensions against the decoded video. Matching dimensions alone cannot distinguish these four cameras.
- Archive opening, consolidated-metadata parsing, and offset loading run on an open worker. Current-frame demand and directional adjacent-page reads use the shared data scheduler. `resolveFrame` only inspects cached data; it does not read storage.
- The default page is 70 frames, with at most 32 cached pages. Full detection-column residency is disabled. This is a page-count bound, **not** a hard decoded-byte admission budget.
- Opening still reads large consolidated root metadata (135–161 MB) and one complete 23,500,840-byte frame-offset vector. TensorStore's shared cache is 64 MiB; neither that nor the page-cache byte counter describes total process memory.
- Storage boxes remain normalized center/size. The bridge uses the shared coordinate contract to produce continuous full-source-camera pixel XYXY for the Linux renderer. Window scaling/zoom remains a separate transform.
- Per-frame observations retain the canonical row index, frame-local ordinal, score, and class. The canonical row is scoped by archive/run; it is not a cross-product stable identity. This raw UI reader does not yet fetch `instance_key`.
- Pending/unavailable and ready-empty are distinct. Invalid geometry fails the frame closed; stale session results are rejected. Canonical detections are read-only.
- Initial open is asynchronous, but close/reopen can wait for an already-running storage read or metadata parse to drain. This is stale-safe, not a strict UI-latency guarantee for source replacement.

The renderer no longer requires a legacy `FrameDetections` object just to draw boxes. That old object remains for older archives and optional legacy overlays. The canonical path does not fabricate headings, masks, keypoints, or provenance arrays to satisfy it.

## Verification

Native build: Ubuntu 24.04.4, RTX A6000, driver 580.173.02; GNU 13.3, CUDA 12.4, OpenCV 4.10.0, TensorRT 10.0.1.6. Full incremental build passed 208/208; the subsequent run-selector correction rebuilt `redgui` successfully.

- Complete serial CTest: **92/92 passed**, zero skipped; initial run 9.61 seconds, final repeat 4.57 seconds.
- New bridge tests: ten consecutive passes. Coverage includes cache-only resolution, nonblocking demand submission, bounded paging, coordinate conversion, empty versus unavailable frames, row identity, malformed geometry, source isolation, wrong-recording/frame/dimension rejection, failed/thrown opens, and stale seek/reopen work.
- Bounded production-reader probes passed on all four archives at frames 53,990; 54,000; 54,010; 2,592,020; 2,592,030; 2,592,040; and 2,937,603. First/warm payloads were identical. Each selected frame had one class-0 detection.
- Cam2010094 frame 2,565,015 separately returned ready with zero rows and zero logical decoded payload bytes. This is reader evidence, not a successful GUI empty-frame capture.
- Real GPU smoke requires canonical frame readiness and a nonempty rendered box scene when rows are present; video/query-frame equality alone no longer passes.
- Logged GUI pixel boxes at frame 54,010 agree with the independent canonical probes for all four cameras within 0.005 pixel (console rounding).
- A paused workspace capture at cam2010093 frame 54,000 passed the exact-frame, 60-stable-frame, full-camera-buffer checks. Visual inspection confirmed the box around the fish.

| GPU recording | Requested range | Verified endpoint |
| --- | --- | --- |
| August cam2010093 | 53990:54010 | Frame/query 54010, one row, one box, one scene box |
| August cam2010094 | 53900:54010 | Frame/query 54010, one row, one box, one scene box |
| August cam2010095 | 53900:54010 | Frame/query 54010, one row, one box, one scene box |
| August cam2010096 | 53900:54010 | Frame/query 54010, one row, one box, one scene box |
| August cam2010093 unequal late clips | 2592020:2592040 | Frame/query 2592040, clip_000048, one rendered box |
| August cam2010093 automatic discovery | 53900:54010 | Frame/query 54010, one rendered canonical box |
| May cam2010095 materialized clips | 53990:54010 | Frame/query 54010, one rendered legacy box |
| Required June GoodCopBadCop control | 0:300 | PASS; presented frame 300 |

These processes retained two or three detection pages (13,440–20,160 reported cache bytes), read 140–210 rows, and reported no full-column residency. Total process peak RSS was approximately 1.37–1.53 GiB; most of that is not the small detection page cache. The smoke processes took 5.1–8.2 seconds, including startup/shutdown.

Final GUI binary SHA-256: `c4af847c0e5b35f8ffa956607a24d90860f0f294d139195815f899a887194d29`.

### Failures and limitations retained

1. The first August GUI smoke failed because the old metadata-only loader clears its selected raw-run name. The bounded authoritative selector lookup described above fixed it; all four subsequent real smokes passed.
2. An invalid attempt to use `--clipped-boundary-smoke 2565014:2565015` was rejected because it does not cross a clip boundary. The early-return shutdown then aborted with `terminate called without an active exception`. This is not an empty-detection test result; its log is retained.
3. Paused UI-reference startup at frame **54,010** timed out, repeatedly presenting **54,000** despite a full decoded buffer and a ready canonical reader. The clip-start capture at 54,000 succeeded. Evidence points to clipped seek settlement upstream of detection resolution, but no before/after baseline reproduction was performed. Exact non-boundary paused seeking remains an observed issue, not a qualified capability.
4. Existing mixed FFmpeg ABI warnings and the workstation-specific prebuilt TensorStore dependency remain as documented in the video-only report. This is not yet a portable packaged build.

## What a complete per-frame view still needs

A frame's logical view may include boxes, scores, classes, headings, keypoints, masks, shape, and provenance without materializing all those products at once. A visible panel should request its needed fields/window, keep readiness distinct from absence/failure, and publish only results with verified source and frame/observation identities.

Eye-angle and speed traces, swim-bout intervals/temporal segmentations, and related metrics are timeline products, not necessarily members of the legacy per-frame detection bundle. Trace panels need bounded frame/time windows; bout lanes need intervals intersecting the visible window. They share the recording's frame/time reference with playback but retain their own track, observation, and event identities. These are part of the desired complete visualization and are not implemented by removing the legacy box-rendering gate.

The August metadata audit found:

| Product | August state | Remaining Linux integration issue |
| --- | --- | --- |
| Keypoints / heading | Selected traditional five-point keypoints; fewer rows than raw detections | Needs bounded reader wiring and stable observation joins; canonical route currently suppresses legacy keypoints |
| Refined keypoints / body frame | Complete but unselected/selector-ineligible child runs | Must follow explicitly bound runs/contracts; do not silently promote or choose by filesystem order |
| Refined subject masks | Complete four-component bundle; unselected/selector-ineligible | Exact upstream-bound run required; no independent parent selector; legacy eye-mask groups are empty |
| Subject shape | Selected schema v5, stable instance identities | Current adapter expects the older row-axis/index layout |
| Eye angles | Selected compact schema v7 | Current bounded timeline reader requires v5 |
| Tail / track / bouts / quality | Products exist with their own selectors and lineage | Not qualified by this detection integration; legacy and bounded paths differ by product |

Selected eye-angle products explicitly bind particular keypoint, mask, and shape runs; shape and tail also bind an exact mask run. Some bound upstream runs are not independently selector-eligible. Picking every parent group's `latest` separately would break that provenance.

Cross-product observation joins must use `instance_key` with validated bound run/manifest identities, not row positions or within-frame ordinals. The current bridge preserves a source-scoped canonical row handle, but loading stable instance keys and implementing those joins is still follow-up work. Track/bout products additionally use `track_id`, `bout_id`, and frame bounds. Even though these four detection-quality manifests report no multi-detection frames, downstream row sets are strict subsets of the raw detections.

## Evidence and reproduction

Local evidence (not included in a repository clone):

- `/tmp/crimson-august-integration-20260916-iebflu/`: native build logs, all tests, GPU logs including failures, coordinate comparison, and captured frame.
- `/tmp/crimson-august-detection-probe-ZYGTiA/`: bounded real-data JSON, exact expected rows/coordinates, zero-row example, and storage timings.
- `/tmp/crimson-phase5l-reference.gA2dXk/`: preserved failed paused-54,010 capture.

From the worktree root, with authenticated GPU/X11 access:

```bash
release/redgui \
  --zarr /path/to/2026_08_06_19_13_35_cam2010093/zarr/2026_08_06_19_13_35_cam2010093_analysis.zarr \
  --clipped-boundary-smoke 53900:54010 \
  --swap-interval 0 --frame-cap-fps 120 --no-mask-perf-log
```

Keep the complete recording directory together, including video clips, sidecars, recording clip index, frame-index manifest, and analysis archive. These visualizations read the recording streams directly, not an exported Parquet dataset.
