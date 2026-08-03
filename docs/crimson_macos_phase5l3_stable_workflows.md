# Crimson Phase 5L.3 Stable Window and Workflow Coverage

Date: 2026-07-17

Status: complete on macOS and the isolated Linux/NVIDIA build. Windows build
and runtime validation remain explicitly deferred.

Lifecycle: **archive-ready completed checkpoint**. Windows validation is
tracked independently rather than as unfinished Phase 5L.3 work.

## Outcome

Phase 5L.3 wires the maintained stable playback and read-only workspace
workflows into the native Mac shell without introducing a Mac storage or edit
model. The portable workspace contract owns commands, selections, window
visibility, camera presentation state, and snapshot restoration. Decoder
rings, repositories, platform windows, and GPU resources remain in their
existing backend owners.

The Mac workspace now provides:

- native media, Zarr archive, and stimulus-video choosers; an archive chosen
  before media is staged for the next open, while a loaded session relaunches
  immediately with the new read-only archive;
- path-preset editing, configurable camera and stimulus decode-ring capacities,
  accurate seek, playback speeds from 0.1x through 1.0x, and honest fixed
  VideoToolbox/Metal/full-resolution backend status;
- play/pause, repeat, frame and ten-frame stepping, slider seeking, keyboard
  transport, pan, wheel zoom, double-click autofit, and paused buffer-identity
  selection with comma/period navigation;
- maintained Frame Inspect tabs and read-only selection/overlay controls,
  including ROI show/camera-overlay matching/width/label and stimulus-inset
  width, opacity, and
  aligned camera/stimulus frame label;
- acquisition-video and live-geometry crop presentation, camera-aligned
  stimulus presentation, optional Advanced Crop Preview, standalone Stimulus,
  and Stimulus Frames in Buffer windows;
- read-only analysis and stimulus-event selections with click-to-seek using the
  same requested/presented-frame rules as camera, crop, and stimulus content;
- Diagnostics decode-buffer dump and random-seek dump actions that write only
  local debug PNG/JSON artifacts; and
- Help, error-modal, close/reopen, clean replacement-process, and versioned
  workspace restoration behavior.

ROI inset presentation intent is shared through
`src/roi_inset_presentation.h`. The portable state defines visibility, width,
label display, overlay inclusion policy, orientation intent, and backend
capabilities. It deliberately does not define acquisition-to-crop coordinate
mapping, heading rotation, handedness, or overlay projection. Linux retains its
existing adapter behavior; macOS resolves unsupported overlay and
heading-normalization requests to no inset overlays and acquisition orientation
until the coordinate contract is finalized.

The acquisition/live-geometry selector is retained as a Mac diagnostic
extension established by Phase 4. The maintained Linux/Windows workspace
chooses live geometry before a persisted crop internally and does not expose
that selector. It is therefore excluded from the visual-parity comparison in
5L.4.

## Read-Only Boundary

All recording repositories exposed to Phase 5L use read-only interfaces. Every
TensorStore open in these repositories explicitly requests
`tensorstore::ReadWriteMode::read`. The portable workspace reports mutation
commands unavailable, and the workflow fixture asserts the read-only invariant
and absence of a write-repository capability.

The following maintained structure remains visible but disabled because a
stable read-only adapter is not currently available:

- detection-run catalog selection and review-filter index navigation;
- ROI heading normalization, because the current crop presentation contract
  does not publish heading;
- the motion trail, because repository positions cannot yet be treated as
  camera pixels when their units may be millimeters; and
- the chaser polar inset, which has no stable read-only scene adapter.

All annotation, review, inference, keypoint-edit, mask-edit, and Zarr mutation
commands remain disabled. Phase 5L.3 creates no recording groups and commits to
no provisional write schema. File Browser preview scale is also shown as fixed
at full resolution because the AVFoundation provider has no lower-resolution
publish contract.

## Deterministic Coverage

The `workspace_workflow_tests` fixture exercises:

- command enablement before and after media/archive/stimulus availability;
- play/pause, rate changes, seek, one/ten-frame steps, and buffered-frame
  identity navigation;
- camera pan/zoom/autofit, representation and event selections, overlay
  toggles, optional-window lifecycle, and versioned snapshot restoration; and
- the disabled mutation surface and read-only repository invariant.

The Mac headless preset passed 36/36 tests. This includes the portable workflow
fixture, repository fixtures, stimulus-ring identity coverage, and six Metal
tests. The Metal tests directly cover source-region sampling and alpha-blended
stimulus presentation, including a 0.5-opacity pixel result.

The same cumulative shared source passed 25/25 tests in the isolated
Linux/NVIDIA build. Linux was configured with CUDA 12.4 for architectures 80
and 86, OpenCV 4.10, TensorRT 10.0.1.6, and the NVIDIA FFmpeg build. Mac-only
stimulus-inset presentation and empty-session archive-staging changes made
after that validation do not enter the Linux build graph.

## Production Evidence

The Mac acquisition-video multistream smoke passed camera, stimulus, and crop
presentation with five exact settlements and zero presentation skew:

```text
/tmp/crimson_macos_multistream_acquisition_20260717_004719.log
camera=6/6 stimulus=6/6 crop=32/32 exact_settlements=5 elapsed_s=4.740
```

The Mac live-geometry multistream smoke also passed with five exact settlements
and zero presentation skew:

```text
/tmp/crimson_macos_multistream_geometry_20260717_004900.log
camera=6/6 stimulus=6/6 crop=0/32 exact_settlements=5 elapsed_s=3.564
```

The authenticated Linux/NVIDIA GUI playback smoke passed frames 0 through 300:

```text
/tmp/crimson_playback_smoke_20260717_004256.log
[PlaybackSmoke] PASS start_frame=0 end_frame=300 presented_frame=300 presented_slot=0 view_idx=0 presented_count=352 elapsed_s=2.99576
```

## Next Checkpoint

Phase 5L.4 captures equivalent Mac states at the maintained reference sizes and
performs structural and region-based visual acceptance. It must document every
platform-chrome mask and tolerance, rerun the portable suites and production
smokes, and keep Windows evidence open until the real Windows build is run.
