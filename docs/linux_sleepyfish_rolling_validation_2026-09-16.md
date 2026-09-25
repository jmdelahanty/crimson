# Linux Sleepyfish rolling playback validation — 2026-09-16

This records the video-only adapter phase. The subsequent [canonical detection integration report](linux_sleepyfish_detection_integration_2026-09-16.md) supersedes the detection-blocker status below and records rendered-box validation plus remaining product/seek limitations.

## Scope and result

The August index-schema blocker is fixed locally on `codex/linux-priority-20260916`, based on `06ad87202dd9d057af3bef02c8557650f71bea22`. The adapter translates consolidated Palette rolling indexes into the existing clip descriptors and reuses NVIDIA playback. It does not rewrite recordings, re-encode video, or change metric loaders/selectors.

All four August 6 Sleepyfish recordings now pass a real NVIDIA clip-boundary smoke. This is **video playback qualification, not full data/metric visualization qualification**: the current NVIDIA detection bootstrap still opens these archives in metadata/stimulus-only mode.

See [the reader contract](recording_clip_index_media_provider.md) for exact schemas, explicit frame-index offsets, completion/continuity/keyframe validation, and relocatable paths.

## Build and verification

Host: Ubuntu 24.04.4, NVIDIA RTX A6000, driver 580.173.02. Build: GNU 13.3, CUDA 12.4, OpenCV 4.10.0, TensorRT 10.0.1.6.

- Clean August-source baseline: full native build passed 596/596; baseline CTest passed 91/91.
- Adapter rebuild: full default incremental build passed 63/63 with eight jobs.
- Final complete CTest run: 91/91 passed serially, zero skipped, 7.24 seconds.
- Focused parser/provider tests passed under C++17 AddressSanitizer and UndefinedBehaviorSanitizer; leak detection was disabled.
- Independent real-data probe checked every index mapping and provider-vector entry: 2,937,604 frames × four recordings = 11,750,416 frame identities, all passed. This checked mapping metadata, not every decoded video frame or CSV payload.
- All four indexes have 55 clips at 30 FPS. Late clips 47/48 contain 54,030/53,970 frames; the probe verified their actual declared ranges, not nominal ordinal-based ranges.
- Before/after size and modification-time checks on cam2010093's root Zarr metadata, clip index, and frame-index manifest were unchanged. No data or selector edits were performed.

Final GUI binary SHA-256: `a948eba893ff675b3c4159623b6af2deeb77b144dc90061c4149c3e5f52c365a`.

### Authenticated NVIDIA smokes

| Recording / route | Requested range | Result | Limitation |
| --- | --- | --- | --- |
| June GoodCopBadCop required smoke | 0:300 | PASS | Short playback control |
| May cam2010095 materialized clips | 53990:54010 | PASS; presented/query 54011 | Existing detection layout loaded, max detections 1 |
| August cam2010093 explicit index | 53990:54010 | PASS; presented/query 54010 | Metadata-only archive load; no detection overlays |
| August cam2010094 explicit index | 53990:54010 | PASS; presented/query 54010 | Metadata-only archive load |
| August cam2010095 explicit index | 53990:54010 | PASS; presented/query 54010 | Metadata-only archive load |
| August cam2010096 explicit index | 53990:54010 | PASS; presented/query 54010 | Metadata-only archive load |
| August cam2010093 late unequal clips | 2592020:2592040 | PASS; switched at 2592030 | Metadata-only archive load |
| August cam2010093 automatic discovery | 53990:54010 | PASS without an index argument | Metadata-only archive load |

The smoke's `bbox_query_frame` equality proves the query uses the displayed parent frame; it does not prove a nonempty bounding box was loaded or rendered. August initial-boundary processes took about 4.8–5.4 seconds and used about 0.89–1.00 GiB peak process RSS. Their low startup cost does not qualify full metric loading because the detection-dependent loaders were skipped.

### Test failures retained in evidence

The first post-change full run passed 90/91: a new test incorrectly expected an unused path alias to invalidate a correct canonical path. Only that test was corrected to prove that a valid alias cannot mask an invalid canonical path; the parser was unchanged.

A subsequent parallel run passed 90/91, with an intermittent cancellation-count assertion at `tools/subject_mask_overlay_repository_tests.cpp:751`. The test, subject-mask buffer, and scheduler were not modified. Inspection found a scheduling window between cache publication and removal of an active speculative request; a reverse current-frame request can increment cancellation metrics in that interval. The final serial run passed all 91. This timing-sensitive test remains a separate follow-up, not a silently discarded failure.

## Remaining blocker: detection and metric integration

The actual August selected run is `detect_native_sleepyfish_2026_08_06_detection_only_20260821_v001_sleepyfish_cam2010093` (analogous terminal camera IDs for the other recordings). Its canonical arrays are under `detect_runs/<run>/instances/`, with `frame_row_offsets` indexing sparse observations.

`ZarrDetectionLoader::loadDetectionRuns` / `loadFlattenedRun` in `src/zarr_loader.cpp` still probe root-level arrays. NVIDIA constructs `LegacyDetectionRepository` in `src/red.cpp`; the resulting descriptor is unavailable, so it produces no boxes. Failure to load the detection layout also skips the current keypoint, eye, subject-shape, and tail bootstrap.

Read-only metadata inspection found the selected run's schema and nested array declarations compatible with the existing `TensorStoreCanonicalDetectionRepository`. That modern repository was not runtime-opened on the August payload during this work. It is currently selected through the macOS analysis path, not the NVIDIA path, and implements a different interface from NVIDIA's `DetectionRepository`.

Next bounded task: connect the canonical repository to NVIDIA using an exact selected run and a repository-interface adapter, retaining legacy handling for older flat archives. Do not simply prepend `instances/` in the eager legacy loader: that would duplicate canonical contract logic and still materialize the long-recording arrays. Subsequent metric/heading/tail alignment and selection must be qualified separately. Acquisition crop-video routing, all-metric correctness, long-session endurance, and repeated random seeks remain unqualified.

## Native development build is not a portable distribution

The build reused TensorStore artifacts from `/home/delahantyj@hhmi.org/gitrepos/crimson/build`; compiler ABI and dependency existence were checked, but this is a workstation development shortcut.

The binary still loads two FFmpeg ABI families: direct avcodec/avformat 58 and avutil 56 from `/opt/orange`, plus transitive avcodec/avformat 60 and avutil 58 through OpenCV. Linker warnings recur. Passing smokes does not resolve this dependency-closure risk for a colleague's workstation. `ENABLE_TENSORSTORE_ZARR3_CHECK=OFF`; that checker was not run. NVENC was off; NVDEC was used.

## Evidence and reproduction

Baseline evidence: `/tmp/crimson-linux-baseline-20260916-2nfaz6/BASELINE.md` and native-build logs under the worktree's ignored `evidence/linux-native-build-20260916/`.

Post-change evidence: `/tmp/crimson-rolling-validation-20260916-mTTXiz/`, including `build.log`, `provenance.log`, `ctest-serial-final.junit.xml`, all earlier test-failure logs, `real_mapping_probe.cpp`, `real-mapping.jsonl`, sanitizer log, and named GPU smoke logs. These `/tmp` paths are local evidence, not included in a clone.

From the Linux worktree root, after building, an August smoke can be reproduced using the actual local recording path:

```bash
release/redgui \
  --zarr /path/to/2026_08_06_19_13_35_cam2010093/zarr/2026_08_06_19_13_35_cam2010093_analysis.zarr \
  --clipped-boundary-smoke 53990:54010 \
  --swap-interval 0 --frame-cap-fps 120 --no-mask-perf-log
```

Use a real authenticated X display and GPU access as described in `AGENTS.md`. Keep the entire recording directory together, including clip sidecars and `recording_frame_index_manifest.json`; an isolated Zarr archive is not the video recording.
