# Crimson Phase 5N Remaining Stimulus Overlay Parity

Date: 2026-07-18

Status: complete. The remaining camera-view stimulus event and moving-grating
direction overlays now use one read-only, exact-frame contract on maintained
OpenGL and native Mac Metal renderers.

Lifecycle: **archived completed checkpoint**. Preserve its acceptance
evidence under `docs/reference/phase5n/` when the document is moved.

## Production Source and Alignment

Both paths selected the current complete GoodCopBadCop stimulus source:

- group: `analysis/stimulus_runs`;
- run: `stimulus_external_ipc_20260616_01`;
- camera-frame count: 140,035;
- event count: 984;
- canonical step count: one; and
- production step: `CHASER`, camera frames 1024 through 139024.

The maintained compatibility adapter materializes an owned portable
`StimulusContextTimelineSnapshot`; render code no longer queries legacy event
or step arrays directly. An event with a declared camera frame retains it. If
the camera frame is missing, the adapter asks the maintained alignment resolver
for the event's stimulus frame with corrected mapping enabled. That resolver
uses corrected camera-to-metadata/direct arrays when present and the declared
legacy metadata order otherwise.

Synthetic Zarr fixtures prove both branches. The same missing event at
stimulus frame 20 resolves to camera frame 8 in the corrected fixture and
camera frame 6 in the legacy-only fixture. Every portable event, label,
event-type descriptor, moving/concentric step field, and sampled scene
signature then agrees with the TensorStore repository. The current production
run has no corrected arrays, so its expected path is the characterized legacy
fallback. The production comparison passed frames 0, 56, 1024, 7024, and
140034 with 984 events and one step.

## Exact-Frame Scene Contract

`resolveStimulusCameraOverlayFrame` accepts only the exact presented camera
frame. The maintained frame-context builder explicitly prefers
`presented_frame` and uses the current frame only when no presented frame
exists. Mac resolves from `AppleVideoFrameMetadata::frame_number`. Missing,
out-of-range, non-exact, invalid-viewport, and invalid-text-metric states fail
closed rather than borrowing a nearby camera frame.

The portable scene preserves the maintained controls and behavior:

- exact events plus the most recent event group for sticky event display;
- a 12-pixel top/left event-panel margin, 6/4-pixel text inset, black
  `180/255` fill, cyan `80/180/255` border, and light-blue text;
- a top-right 204x76 moving-grating panel when the viewport permits;
- `Grating motion N deg` text and a camera-coordinate direction arrow;
- explicit `event_panel`, `event_text`, `step_panel`, `step_text`, and
  `step_arrow` layer order; and
- a viewport-independent semantic signature using panel-local geometry.

The maintained adapter submits the finished scene after `ImPlot::EndPlot()`
and clips it to the camera viewport. This keeps ImPlot's late legend pass from
covering event context while retaining the normal camera-window ordering. The
Metal adapter tessellates the same rounded rectangles, lines, and triangles,
uses the camera scissor, and leaves text glyph rasterization to bundled ImGui.

## Deterministic Coverage

Portable tests cover timeline unavailable, out of range, valid-empty, exact
events, sticky events, step boundaries, control combinations, invalid inputs,
non-exact rejection, viewport-independent signatures, tessellation, and a
90-degree moving-grating arrow. The synthetic legacy probe covers corrected
precedence, legacy fallback, labels, every step field, adapter equality, and
scene equality. The Metal offscreen fixture proves event-panel and
moving-grating arrow pixels, camera scissoring, non-ready rejection, and
offscreen fail-closed behavior.

The macOS arm64 Release suite passed 44/44 tests. The isolated NVIDIA Release
suite passed 34/34 tests, including
`stimulus_context_timeline_legacy_characterization`.

## Controlled Cross-Platform Acceptance

Both applications captured production camera frame 1024 after 60 stable
frames in a 1920x1080 framebuffer. The Linux camera viewport is 430x298 and
the Mac viewport is 338x338, but both produced the same panel-local scene:

- event/source/requested camera frame: 1024;
- event panel: `(12, 12, 254, 68)`;
- primitives/text annotations: one/one;
- exact ordered text:

  ```text
  STEP_START - Chaser
  CHASER_PRE_PERIOD_START - Chaser
  CHASER_AT_PRE_POSITION - Chaser
  CHASER_PRESENTATION_START - Chaser
  ```

`tools/phase5n_stimulus_overlay_compare.py` passed all 17 checks recorded in
`docs/reference/phase5n/acceptance_report.json`. Descriptors, frame identity,
geometry, primitive fields, colors, text, anchors, layer order, and canonical
semantic signatures are identical. Both panel-border rasters achieved 1.0
coverage; the captures contain 1,384 Linux and 1,375 Mac light-blue text
pixels inside the panel. Font glyph shape and translucent fill channel values
are the only renderer-specific masks declared in the acceptance contract.
The selected production step is CHASER, so the moving-grating direction raster
is enforced by the portable scene and Metal threshold fixtures rather than
misrepresented as production evidence.

## Production Smokes and Numerical Policy

The native Mac smoke passed frames 1024:1324 in 3.630 seconds and presented
exact frame 1324. The authenticated NVIDIA smoke passed the same range in
2.99575 seconds with 350 presentations and exact frame 1324.

Generated macOS and isolated NVIDIA compile databases and Ninja graphs contain
none of `-Ofast`, `-ffast-math`, or `--use_fast_math`. NVIDIA Release targets
use CUDA architectures `80;86`. The existing OpenCV and TensorStore safeguards
remain active, keeping finite checks and cross-platform scientific comparisons
on normal IEEE semantics.

## Read-Only Proof

The production `analysis/stimulus_runs` and `analysis/enums/events` subtrees
were fingerprinted before adapter comparisons, captures, and smokes, then
fingerprinted again. All 6,950 files retained identical paths, sizes,
modification times, and SHA-256 content hashes. The combined before/after hash
is `aaefd0e77cbfd973ac89fa126de887ec5083fefbafc99410ac6379d5a16c00fb`;
the compact evidence is in
`docs/reference/phase5n/nonmutation_summary.json`.

No group creation, attribute update, storage schema, edit API, or write
repository was added. Phase 5N closes only this remaining stimulus camera
overlay gate. Production-tail acceptance, movement-trail coordinate policy,
any required chaser-data densification, and mutable edit/review workflows
remain open for explicit review before Phase 6.
