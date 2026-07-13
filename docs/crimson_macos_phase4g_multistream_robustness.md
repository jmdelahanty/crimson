# Crimson Phase 4G Multistream Robustness

Date: 2026-07-13

Phase 4G hardens native macOS camera, stimulus, and crop presentation across
pause, paused stepping, backward and forward seeks, resumed playback, missing
metadata, and sustained PRFS access. It also closes the geometry-only recording
gap left by Phase 4F-E: Crop Preview no longer requires an acquisition crop
video repository before it can use analysis crop geometry.

## Geometry-Only Repository

`AnalysisCropGeometryRepository` is a portable C++17 metadata boundary. Its
TensorStore adapter opens `crop_runs/<run>` and reads:

- `frame_indices`;
- `roi_coordinates_full`;
- optional `bbox_norm_coords`; and
- `roi_size` from run attributes.

Older runs without `roi_size` can infer height and width by opening
`roi_images` metadata at rank three or four. This does not read image chunks.
Geometry-only Palette runs need no `roi_images` array at all.

When no run is requested, discovery follows the existing Crimson-compatible
pointer order:

```text
latest
latest_completed
latest_complete
latest_success
latest_any
```

The final fallback is needed by Palette groups that expose the newest run even
when no completed pointer exists. A requested run may be either a bare name or
`crop_runs/<name>`; paths and traversal are rejected.

Each row maps its camera-frame identity to a full-frame crop rectangle. If a
frame has multiple ROI rows, Crimson selects the first row, matching the legacy
unselected Crop Preview fallback. Sparse camera frames are reported as missing,
not silently replaced by a neighboring ROI. Normalized center/width/height
detection boxes are converted back to the full-camera coordinate system before
the shared crop contract transforms them into Crop Preview coordinates.

## Independent Source Initialization

The macOS application now opens stimulus, acquisition crop, and analysis crop
geometry independently from one shared `ArchiveContext`. A failed or absent
stimulus repository no longer prevents ordinary geometry-only viewing. A
failed or absent acquisition crop stream no longer destroys a valid analysis
geometry source.

When both geometry sources exist, the selected analysis crop run is the
canonical `Live geometry` source. Missing frames in that run remain missing;
they do not silently fall through to geometry associated with a different
derived acquisition stream. `--crop-run RUN` selects an explicit analysis run.

The Crop Preview controls disable unavailable sources. If acquisition video is
absent but analysis geometry is present, the initial preference changes to
`Live geometry`. The viewport uses the analysis run's declared output size, and
Metal samples the exact retained full-camera surface. No crop decoder is
created for a true geometry-only recording.

## Discontinuity Contract

`--multistream-smoke START:END` constructs a plan from frames that are actually
mapped by both the stimulus repository and the selected crop source. It then
performs this sequence:

1. timed playback to a pause target;
2. exact paused settlement;
3. exact one-frame forward step;
4. exact backward seek;
5. exact forward seek;
6. resumed timed playback; and
7. exact settlement at the requested end frame.

A stage advances only after camera, stimulus, and crop were all encoded into
the same Metal composite with the requested camera identity. The final smoke
requires five explicit exact settlements, zero stimulus/crop identity
mismatches, and zero camera skew.

Intentional seeks are presentation discontinuities, not decoder drops. The
viewer therefore excludes the requested jump and any retained pre-settlement
surface from steady-playback skipped-frame and lag counters. Once a new exact
frame commits, normal drop and lag accounting resumes.

## Resource Bounds

The production smoke enforces the configured queue capacities:

- main camera peak at or below its reported capacity;
- stimulus decoder peak at or below six frames; and
- acquisition crop decoder peak at or below six frames.

It samples the process physical footprint throughout the run and requires
valid telemetry. Peak growth from the first in-loop sample must not exceed
512 MiB. This is deliberately above the expected retained VideoToolbox surface
footprint but low enough to catch unbounded accumulation during the maintained
6,000-source-frame smoke.

## Automated Coverage

`analysis_crop_geometry_repository_tests` builds Zarr v3 fixtures and verifies:

- `latest_complete` discovery and explicit run selection;
- sparse frames, duplicate rows, out-of-range requests, and escaped geometry;
- ROI and detection coordinate conversion;
- geometry-only source capabilities; and
- ROI dimension inference from metadata when no `roi_images` chunks exist.

`apple_multistream_discontinuity_tests` uses one deterministic Apple video
fixture for camera, stimulus, and acquisition crop surfaces. It renders three
panels into an offscreen Metal target across ordered, forward, and backward
settlements. It also covers a missing stimulus frame, a mapped blank
acquisition row, a missing analysis-geometry frame, a final exact live crop,
zero skew/mismatch counters, and decoder peaks bounded by four.

The shared Apple fixture runner removes stale output before both the initial
AVAssetWriter attempt and its one allowed `Cannot Encode` retry.

macOS validation:

```text
cmake --build --preset build-macos-arm64-release -j 8
ctest --preset test-macos-arm64-headless --output-on-failure
17/17 passed
```

## Production PRFS Validation

The maintained command is:

```bash
scripts/macos_gui_smoke_multistream.sh [VIDEO] [ANALYSIS_ZARR] \
  [START:END] [acquisition|geometry] [CROP_RUN]
```

Both modes ran `1024:7024` directly from `/Volumes/johnsonlab` over the network
mount. Small steady-playback drops remain acceptable under variable PRFS/VPN or
Wi-Fi latency; the scientific acceptance point is exact settlement after each
scripted discontinuity and at the final frame.

Acquisition-video mode used the June 14 GoodCopBadCop recording:

- five of five exact discontinuity settlements;
- final camera, stimulus mapping, and crop camera identity at frame `7024`;
- zero stimulus or crop camera skew;
- camera/stimulus/crop decoder peaks `6/6`, `6/6`, and `6/6`;
- process memory `107.7` MiB at the first sample and `219.0` MiB peak;
- memory growth `111.3` MiB of the `512.0` MiB limit; and
- 6,000 source frames traversed in `38.376` seconds.

Geometry-only mode used the May 29 GoodCopBadCop recording. Its selected
Palette run declares `crop_storage_mode=geometry_only`, 143,305 ROI rows, a
512x512 ROI, and no acquisition-video stream:

- five of five exact discontinuity settlements;
- final camera, stimulus mapping, and live crop identity at frame `7024`;
- zero stimulus or crop camera skew;
- camera/stimulus decoder peaks `6/6` and `6/6`;
- crop decoder peak zero, with zero crop seeks and follows;
- process memory `87.2` MiB at the first sample and `198.0` MiB peak;
- memory growth `110.8` MiB of the `512.0` MiB limit; and
- 6,000 source frames traversed in `37.263` seconds.

## NVIDIA Validation

The cumulative patch was applied to the detached maintained NVIDIA worktree.
Configuration resolved CUDA 12.4 for architectures 80 and 86, OpenCV 4.10,
TensorRT 10.0.1.6, NVIDIA FFmpeg, and the prebuilt TensorStore stack.

Linux source discovery initially compiled the new repository files directly
into `redgui`, duplicating their library ownership and causing unresolved
symbols. CMake now excludes those owned sources from the legacy glob, as it
already does for the other repository implementations. `redgui` then linked
with only the existing FFmpeg/OpenCV version-family warnings.

All seven applicable portable and TensorStore tests passed. The authenticated
NVIDIA GUI smoke passed `0:300` with final presented frame `300`, 351
presentations, and `2.99246` seconds elapsed. The final corrected latest-run
repository test was rebuilt and passed again on that host.

## Next Checkpoint

Phase 4G completes the robust three-stream playback checkpoint and proves that
the native app can use a derived acquisition crop video or a real analysis-only
geometry run under the same exact presentation contract. The next checkpoint
should begin overlay parity with an inventory and deterministic fixtures for
the overlays visible in the maintained camera and crop views, without changing
the playback ownership or identity rules established here.
