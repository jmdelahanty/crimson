# Crimson Phase 5G Read-Only Overlay Controls

Date: 2026-07-14

Phase 5G connects native macOS controls to Crimson's shared read-only overlay
scene. It covers the camera annotations already ported through Phase 5F:
keypoints and headings, semantic subject-mask components, eye geometry, and
subject-shape details. Control changes are applied after exact-frame repository
adapters populate the scene input and before the shared scene is built, so they
do not change row identity, asynchronous publication, or renderer ownership.

This is not full UI parity. The maintained Linux/Windows application still has
the complete `redgui` workspace, while macOS has a purpose-built playback shell
with a movable overlay-control window. Analysis plots and timelines, the
representation selector, mutation/review tools, docking, and other multiwindow
workflows remain later Phase 5 work.

## Shared Contract

`ReadOnlyOverlayControlState` and `ReadOnlyMaskOverlayMode` are backend-neutral
C++ contracts. They contain no ImGui, Metal, OpenGL, CUDA, decoder, TensorStore,
or Zarr dependency. The maintained `CameraViewMaskOverlayMode` is now an alias
of the same mode type, and its label helper delegates to the shared name.

The mode behavior preserves the maintained camera-overlay contract:

- `Realtime` draws enabled mask fills but withholds mask contours and all eye
  geometry;
- `Review` draws enabled fills, contours, and eye geometry; and
- `Debug` has the same read-only visibility behavior as `Review` and remains a
  distinct mode for the maintained diagnostic workflow.

Master switches gate subject masks, eye geometry, and subject shape. Detail
switches independently select body, left-eye, right-eye, and swim-bladder mask
components; visual cones, gaze rays, signed-angle arcs, and angle labels; and
the maintained subject-shape landmarks, centerline, spline, body axes, debug
points, tail samples, and tail normals. Hiding a left or right eye also hides
that eye's refined axes and derived geometry. Unknown future mask labels remain
visible so adding a production component does not silently remove it.

## Native macOS UI

The playback toolbar has an `Overlays` command that opens or closes a movable,
resizable `Overlay controls` ImGui window. Controls are grouped by scientific
overlay family and disabled when the corresponding repository is unavailable.
The window opens only on request and provides a reset to the Phase 5G defaults.

The control state is persistent for the process and is applied to every exact
presented frame. Repository availability is refreshed from the active Mac
adapters. Smoke mode remains noninteractive and therefore continues to measure
the default production overlay set.

Metal vector and raster annotations remain in the camera scene and retain its
scissor. Crop-preview borders and scientific text use ImGui's background draw
list so normal UI windows always remain above annotations. This prevents a
camera overlay from crossing the controls window without changing its camera
coordinates or layer order.

## Deterministic Coverage

`read_only_overlay_controls_tests` exercises the shared mapping without a GUI.
It verifies default, `Realtime`, `Review`, and `Debug` behavior, master gates,
null-input handling, every subject-shape detail switch, semantic mask filtering,
and per-eye geometry filtering. The semantic fixture also verifies that an
unknown component is retained.

The existing read-only scene and offscreen Metal tests continue to cover exact
frame withholding, scene order, crop transforms, zoom, scissor, raster masks,
eye polygons and labels, subject shape, keypoints, and headings. The new test is
labelled `controls;headless;overlay;portable` and is included in the standard
macOS release build preset.

## Validation

The complete macOS preset passed all 26 tests, including the new controls test,
the TensorStore repository fixtures, and all offscreen Metal tests. The mounted
May 29 production recording passed camera frames 100 through 160. It reached
frame 160 with zero PTS error and zero late presentations while publishing
exact masks, subject shape, and eye geometry. The initial unavailable derived
acquisition crop correctly fell back to live geometry.

The cumulative source also configured and built in an isolated NVIDIA worktree
on `ws1`. The build retained CUDA 12.4 with effective architectures 80 and 86,
TensorRT 10.0.1.6, OpenCV 4.10.0 with SFM, the NVIDIA FFmpeg stack, and the
maintained CUDA/NVDEC/OpenGL application. `redgui` and all affected GUI sources
linked successfully. All 14 portable CTest targets passed.

The authenticated maintained playback smoke then loaded the June 14 production
Zarr and passed frames 0 through 300 on the RTX A6000. It reached frame 300 with
357 presentations in 2.998 seconds. This validates that sharing the mode type
and label contract does not regress the maintained renderer or its production
playback path.

## Remaining UI Parity

Phase 5G establishes a shared state boundary that later Mac surfaces can reuse;
it does not replace or clone the entire `redgui` UI. The next UI parity slices
must connect the analysis timeline and representation selection, then carry
over the required edit/review panels and multiwindow workflows. Final Phase 5
acceptance still requires the complete workflow inventory and the established
screenshot/image-difference gate.
