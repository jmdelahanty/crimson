# Crimson Phase 5B Read-Only Vector Overlays

Date: 2026-07-13

Phase 5B introduces the first shared scientific-overlay scene and a direct
Metal renderer for its read-only vector primitives. It also moves the
maintained NVIDIA camera boxes, keypoint skeletons, markers, and headings onto
that scene without changing their ImPlot presentation backend.

This is a bounded Phase 5 slice, not the complete Phase 5 parity gate. Masks,
contours, text rasterization, edit handles, picking, crop keypoint editing,
insets, and remaining application surfaces stay in later checkpoints.

## Portable Scene

`src/read_only_overlay_scene.h` defines semantic detection inputs and three
backend-neutral primitive kinds:

- polylines for detection boxes and skeleton edges;
- markers with shared shapes, fill, outline, and quality-state alpha; and
- heading arrows with display-pixel stroke and arrowhead sizes.

The builder preserves clean, interpolated, and manual box provenance; selected,
added, and frame-modified states; keypoint labels and marker shapes; refined
keypoint usability, interpolation, and flip-correction states; skeleton edge
ordering; and the keypoint-derived heading calculation. Primitive labels are
retained as exact scene metadata, although Metal text rendering is outside this
slice.

Every scene carries the Phase 5A `FrameIdentity`. A stale, future, invalid, or
cross-view identity produces no primitives and is withheld by both backends.
Coordinates remain top-left source pixels. Stroke widths, marker radii, and
arrowhead sizes remain display-pixel quantities under zoom.

`tessellateReadOnlyOverlayScene` maps the immutable scene through a
`SourceViewportTransform` and emits a triangle mesh. It has no ImGui, ImPlot,
OpenGL, Metal, CUDA, TensorStore, or decoder dependency.

## NVIDIA Adapter

The maintained camera overlay functions now translate `FrameDetections` and
bounding-box edit state into the portable scene. One generic ImPlot drawer
consumes the resulting primitives in the existing
`kCameraOverlayLayerOrder`. The selected keypoint-edit detection still withholds
only its ordinary markers while retaining its skeleton, matching the previous
behavior.

The shared keypoint palette is also used by the crop/refined-keypoint style
adapter, removing a second independently maintained copy of those colors.

The edit-state `Manual` provenance is resolved before style selection. This
makes the manual label and teal manual color agree for a newly edited box; the
previous path could emit a manual label with the pre-edit color.

## Metal Renderer

`AppleOverlayMetalRenderer` owns one alpha-blended Metal render pipeline. For
each ready scene it uploads screen positions and colors as vertex buffers,
sets the camera viewport's display rectangle as a scissor, and draws the
tessellated triangles into the existing drawable render encoder. It performs
no GPU-to-CPU readback and does not own or retain decoded video surfaces.

The macOS shell currently builds a boxes-only scene from
`CropFrameGeometry::full_frame_detection` when all of these identities agree:

- the decoded and presented camera frame;
- the selected crop camera frame;
- the crop geometry camera frame; and
- the full-frame video and geometry source dimensions.

That is real TensorStore-backed recording metadata, not synthetic GUI data.
The camera box is therefore available for either acquisition-video or live
geometry crop selection when the selected row has a detection. The macOS shell
does not yet load the maintained NVIDIA `FrameDetections` keypoint repository,
so production keypoints, skeletons, and headings are not claimed as connected
macOS features in this checkpoint. The renderer and deterministic fixture
already cover those primitive types for the later repository adapter.

## Deterministic Fixture

`tests/fixtures/read_only_overlay_scene_fixture.h` fixes view 1, camera frame
137, a 640x360 source, two boxes, one heading, three skeleton segments, and
five markers. It includes clean and interpolated detections, a non-finite
keypoint, and refined-keypoint quality states. The expected scene has exactly
11 ordered primitives.

`read_only_overlay_scene_tests` verifies:

- exact primitive counts, order, labels, styles, and heading endpoints;
- stale identity and invalid-dimension withholding;
- feature enablement; and
- finite full-view triangle tessellation.

`apple_read_only_overlay_metal_tests` renders that same scene into a shared
offscreen BGRA texture. After command completion, test-only readback verifies
the clean box, alpha-blended interpolated box, heading, marker, full-view and
zoom mapping, scissor containment, and a completely untouched stale-scene
target.

## Phase 5B Gate

Phase 5B is complete when:

- the portable scene and tessellator tests pass on macOS and NVIDIA;
- maintained NVIDIA `redgui` builds and its production playback smoke passes;
- the Apple overlay pipeline and macOS app build;
- the offscreen Metal color, blend, zoom, scissor, and withholding checks pass;
- a production macOS crop smoke presents the metadata-derived camera box; and
- interactive rendering contains no synchronous full-resolution readback.

## Validation

The macOS Apple Silicon release preset built `Crimson.app`, the portable scene
test, and the offscreen Metal test. The complete GPU-enabled headless preset
passed 20 of 20 tests. The new Metal fixture rendered 11 primitives as 199
triangles and passed its color, alpha blend, zoom, scissor, and stale-scene
checks.

Both production macOS crop modes passed frames 1024 through 1324 against the
June 14 GoodCopBadCop recording over the mounted network volume. Acquisition
crop reported 203 exact metadata-overlay presentations; live geometry reported
194. Both ended on camera frame 1324 with zero camera skew and zero maximum
camera skew.

The cumulative source was also configured in an isolated NVIDIA validation
directory with CUDA 12.4, OpenCV 4.10, TensorRT 10.0.1.6, NVIDIA FFmpeg, and
the prebuilt TensorStore stack. `redgui` and the portable scene test built; the
test passed. The authenticated production playback smoke passed frames 0
through 300 after the final adapter update. Existing CUDA/FFmpeg/TensorRT
deprecation and FFmpeg version-family linker warnings remained non-fatal.
