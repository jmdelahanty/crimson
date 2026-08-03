# Crimson Phase 5M.3 Shared Polar Scene and Platform Presentation

Date: 2026-07-18

Status: complete. This checkpoint moves chaser-distance polar presentation
behind one backend-neutral scene contract and connects both maintained ImGui
and native Mac Metal presentation. Cross-platform production visual acceptance
remains Phase 5M.4 work.

Lifecycle: **archived completed checkpoint**. Phase 5M.4 closed the stated
cross-platform acceptance work.

## Shared Scene Boundary

`src/chaser_distance_polar_scene.h` defines the complete portable presentation
result. `buildChaserDistancePolarScene` accepts only a canonical portable frame
sample, portable controls, and a logical viewport. It has no storage, ImGui,
Metal, CUDA, OpenGL, AVFoundation, or concrete-loader dependency.

The scene records a named, ordered set of primitives and annotations for:

- the inset background and border;
- radial rings and their millimeter labels;
- front, left, right, and behind orientation axes and labels;
- clamped chaser points with their resolved portable colors;
- the optional exact-frame distance/bearing readout; and
- the camera-relative clip rectangle and logical display bounds.

Portable controls preserve visibility, width, opacity, label visibility, and
readout visibility; the scene preserves the maintained fixed marker sizing.
Scene construction withholds all presentation for unavailable, unsupported,
missing, or failed samples. A valid-empty exact frame remains distinguishable:
it draws the inset and an empty-state readout but no scientific points.
`tessellateChaserDistancePolarScene` converts the same primitive stream to
portable colored triangles for raster adapters.

## Maintained ImGui Adapter

The maintained camera-frame context now resolves an exact portable sample
through `ChaserDistancePolarRepository`. `camera_view_window.cpp` builds the
shared scene at the current logical plot size, and
`camera_view_stimulus_overlay.cpp` is a thin ImGui draw-list adapter over the
scene primitives and annotations.

`red.cpp` owns a `ChaserDistancePolarLegacyRepository` over the maintained
loader and refreshes it whenever the archive is loaded, reloaded, or cleared.
The camera-view and Frame Debug presentation paths no longer consume
`ZarrDetectionLoader::ChaserDistancePolarFrame` or call the legacy polar
getters directly. Unmigrated loader responsibilities remain behind the
maintained compatibility facade.

## Native Mac Adapter

The Mac application opens `TensorStoreChaserDistancePolarRepository` from its
existing read-only `ArchiveContext` and feeds it through a bounded
`ChaserDistancePolarBuffer` with exact-frame requests, generation-based seek
discard, bounded lookahead, and bounded caching. Dataset absence or an optional
polar read failure does not stop base video playback.

For each presented camera frame, the application builds the same portable
scene in camera logical coordinates. `AppleOverlayMetalRenderer` tessellates
and uploads its vector primitives through the existing Metal overlay pipeline,
using a scissor rectangle derived from the camera viewport. The Mac ImGui layer
draws only the shared scene's semantic text annotations. Live controls expose
the same visibility, width, opacity, label, and readout policy carried by the
portable scene contract, while marker sizing remains fixed by the scene.

Runtime metrics report polar presentations, points, availability results,
buffer requests/cache behavior, discarded work, source/published points,
resolve latency, and optional-runtime failure independently of the video
provider.

## Deterministic Coverage

`chaser_distance_polar_scene_tests` covers layer names and order, all
availability states, maintained geometry, cardinal orientation, ring and point
placement, clipping, controls, labels/readout, empty data, and portable
tessellation.

`apple_read_only_overlay_metal_tests` renders a ready shared polar scene
offscreen through the real Metal adapter. It verifies substantial raster
coverage, camera-viewport clipping, point color placement, non-exact-frame
withholding, and fully offscreen withholding. This proves that the native
renderer consumes the shared scene rather than reconstructing polar semantics.

A deterministic native application smoke used a generated 64x48, 12-frame
video and a generated read-only polar Zarr fixture. Starting at exact camera
frame 10 produced four polar presentations and eight rendered points, with two
ready resolves, zero failed resolves, and no runtime failure. The video smoke
also reached frame 11 with zero skipped source frames and passed.

## Checkpoint Verification

The complete macOS arm64 Release suite passed 43/43 CTest tests. The Metal
fixture test reported 11 primitives and 199 triangles. The generated macOS
compile commands contain neither `-Ofast` nor `-ffast-math`.

The isolated Linux/NVIDIA build compiled the full `redgui` target and passed
32/32 CTest tests. Its authenticated production playback smoke passed:

```text
[PlaybackSmoke] PASS start_frame=0 end_frame=300 presented_frame=300 presented_slot=0 view_idx=0 presented_count=352 elapsed_s=2.99164
```

The production chaser-distance subtree was fingerprinted immediately before
and after the comparison and smoke. All 4,908 files had identical sizes,
modification times, and SHA-256 hashes. The direct-loader include policy remains
at its allowed 28-file baseline.

The production archive was not mounted on the Mac during this checkpoint, so
the same-frame maintained-versus-Mac production image capture is deliberately
not claimed here. That comparison, the full controlled visual tolerances, and
the final production Mac smoke are the Phase 5M.4 acceptance gate.
