# August swim-bout speed shading — 2026-09-23

Implemented on `codex/main-integration-20260922` after the contour/shape
checkpoint `dd41281`. That checkpoint is committed locally. This shading
increment is not yet committed, pushed, merged or packaged.

## Behavior

The canonical **Analysis Timeline** now defaults **Shade swim bouts** on.
Translucent green bands mark bout intervals behind the speed traces; an
additional stronger green band marks each valid core. The checkbox controls
speed shading only. The separate bout lane and detector trace remain available.
No intervals are inferred from speed or regenerated, and no recording data is
modified.

The renderer reuses the existing asynchronous, bounded in-memory motion and
bout windows. These logical timeline pages are not video-buffer slots or
physical Zarr chunks. Production defaults remain 4096 frames per page, a 2048
frame step, at most 1200 points per trace, and three cached pages per product.
Underlying repository indexes and TensorStore/OS caches are separate; the
three-page limit is not a total process-memory cap. This increment adds no
storage scheduling or payload reads to the draw path.

## Identity and timing

- Require a ready session, mapped windows covering the current frame, matching
  selected source/candidate identities, and compatible motion run, track and
  speed-level provenance. Pending, empty, failed, mismatched and out-of-window
  data produces no stale shading.
- Cached pages retain their original request anchor while serving subsequent
  playback frames. Page coverage and source identity—not anchor equality—decide
  whether the current frame can use them.
- Honor the motion window's finite, strictly increasing published frame/time
  mapping. Interpolate within it; only if both mapping arrays are absent use
  frame/FPS. Malformed mappings suppress shading.
- Preserve inclusive source frame intervals. Draw their frame extents as
  `[time(start), time(end + 1))`, without changing stored endpoints. The final
  mapped frame's exclusive boundary uses the last measured time slope for one
  frame only. Clip bands to both loaded windows, mapping coverage and visible
  plot limits. This follows the canonical bout lane's frame-extent convention;
  older native macOS direct-endpoint rendering was not changed.
- Validate core containment and recording bounds. Invalid cores are omitted
  without inventing replacement geometry. Generic plot bands draw before
  traces/markers and support inverted axes; callers with no bands are unchanged.

## Validation

- Pinned Ubuntu 22 / CUDA 12.4 full build passed; **108/108 CTests passed**.
- New deterministic tests cover single-frame inclusive intervals, clipping,
  cores, malformed mappings, nonuniform/offset timestamps, FPS validation,
  selected-source mismatches and cached-page reuse across frames 15–19 while
  the page anchor remains 10.
- All four August cameras (2010093–2010096) passed seven-window probes at
  0, 53990, 54000, 54010, 2565015, 2862000 and 2937603. Every emitted band was
  checked against clipped published boundaries and independently mapped motion
  times. Existing raw timeline comparisons and page bounds also passed. Empty
  bout windows legitimately emitted zero bands; this was not a whole-recording
  audit.
- Authenticated GPU captures on camera 2010093 passed at frame 54010 with
  shading enabled (**12 bout + 12 core rectangles**) and disabled (**0 + 0**),
  and at distant frame 2565015 (**17 + 17**). Counters measure clipped rectangles
  actually submitted, not just available intervals. Enabled/disabled captures
  were visually inspected; speed peaks and bands aligned with the bout lane.
- Required June legacy playback smoke (0–300) passed. Shell syntax checks and
  `git diff --check` passed. Native macOS/Windows were not tested.

Evidence: `/tmp/crimson-bout-shading-20260923.gVis3Y`; GPU captures:
`/tmp/crimson-canonical-bout-shading.spdtRD`,
`/tmp/crimson-canonical-bout-shading.m5qdRn`, and
`/tmp/crimson-canonical-bout-shading.TzRdRo`. Temporary evidence is not committed.
The tested `release/redgui` SHA256 is
`1dc7098f0b4a044bb6910051592c5908f89a3d4f014120c0a9b9df28b52a88cf`.

## Local test

Use the fresh `release/redgui` with the
[local launch instructions](linux_sleepyfish_contours_shape_controls_2026-09-23.md#local-visual-check),
not an older package. Open **Analysis Timeline** and toggle **Shade swim bouts**.

For an isolated automated capture, provide the runtime library path and an
authenticated display as in the launch instructions, then run:

```bash
bash scripts/gui_smoke_canonical_bout_shading.sh ARCHIVE.zarr 54010 on
bash scripts/gui_smoke_canonical_bout_shading.sh ARCHIVE.zarr 54010 off
```

Choose a frame with visible bouts and valid cores for this positive GPU gate.
The smoke owns and closes only its own window and isolates UI configuration.

Canonical ROI inset rendering and eye-geometry overlays remain outside scope.
