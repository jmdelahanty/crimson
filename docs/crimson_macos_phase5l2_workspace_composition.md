# Crimson Phase 5L.2 Mac Workspace Composition

Date: 2026-07-16

Status: complete for the Mac composition checkpoint. Windows build and runtime
validation are deferred and are not inferred from the Mac or Linux results.

## Outcome

The native Mac application now presents the maintained Crimson workspace as
independent ImGui windows inside one GLFW/Cocoa window. Metal remains the video
renderer, but camera, crop, stimulus, overlay, diagnostics, and timeline
content are placed from the live ImGui content rectangles instead of a
Mac-specific full-canvas arrangement.

The first-use layout reproduces the audited 1920x1080 maintained profile and
uniformly scales it when the logical client is smaller. At the default
1280x800 Mac client, the same seven roles remain visible without reflowing the
maintained topology:

| Role | Maintained behavior on Mac |
|---|---|
| File Browser | Always submitted; maintained menu and playback status structure |
| Frame Inspect | Loaded-video role with read-only tabs and disabled mutation controls |
| Diagnostics | Loaded-video runtime and alignment status |
| Frames in the buffer | Paused-only window using an immutable snapshot of the real decoded frame identities |
| Camera | Recording-derived title, Metal content, and maintained transport order |
| Stimulus Event Timeline | Zarr-gated independent timeline role |
| Analysis Timeline | Independent motion, swim-bout, eye-angle, tail, and stimulus-context role |

Advanced Crop Preview remains the one closeable optional presentation window.
Stimulus and Stimulus Frames in Buffer remain optional diagnostic windows.
Crop and mapped stimulus presentation also use fitted camera insets, while the
standalone windows reuse the same decoded surfaces.

## Theme and Persistence

- ImGui uses `StyleColorsClassic()`.
- Roboto Regular is loaded at 15 px.
- Fork Awesome is merged at 15 px with pixel snapping for the maintained
  transport icons.
- Normal sessions persist ImGui placement and collapsed state under
  `~/Library/Application Support/Crimson/imgui.ini`.
- Smoke sessions remain transient and cannot alter the user's workspace.
- The camera window uses a versioned internal ImGui identity so geometry saved
  by the pre-parity Mac shell cannot override the maintained first-use size.

Window geometry is applied with `ImGuiCond_FirstUseEver`. Resizing or moving a
window therefore persists normally and later client resizes do not silently
reflow the user's arrangement, matching the maintained ImGui lifecycle.

## Rendering Boundary

`apple_workspace_layout` is a renderer-neutral geometry helper. The UI layer
captures content rectangles for the camera and optional preview windows, then
the existing Metal renderer encodes directly into those drawable regions.
Crop and stimulus insets are fitted from the camera media rectangle. Overlay
transforms use the same presented camera viewport.

This checkpoint adds no storage dependency, write repository, annotation
mutation, or synchronous full-resolution GPU readback. TensorStore repositories
remain read-only, decoder queues remain in their existing owners, and Metal
objects do not enter the portable workspace contract.

`--start-paused FRAME` provides a deterministic manual/reference launch without
changing normal startup. Frame 56 of the representative GoodCopBadCop recording
was inspected with all seven core roles visible, exact paused camera identity,
the six real buffered frame identities, crop inset, read-only overlays, and
both timeline columns.

## Validation

Mac build and headless suite:

```bash
cmake --build --preset build-macos-arm64-release -j6
ctest --preset test-macos-arm64-headless --output-on-failure
```

Result: 35/35 tests passed. This includes workspace geometry, portable
workspace state, Metal composition, crop/stimulus discontinuities, overlays,
and TensorStore repository tests.

Production multistream smokes:

```bash
scripts/macos_gui_smoke_multistream.sh VIDEO ZARR 1024:1324 acquisition
scripts/macos_gui_smoke_multistream.sh VIDEO ZARR 1024:1324 geometry
```

Both passed all five exact pause/step/backward/forward/end settlements with
zero stimulus and crop camera skew. Acquisition used its intended 32-slot
decode queue (`32/32` peak); live geometry used no crop decoder slots (`0/32`).
The smoke exposed and fixed an aggregate-test defect that had incorrectly
applied the six-slot stimulus bound to the 32-slot acquisition crop queue.

The cumulative CMake file was synchronized to the isolated NVIDIA checkout at
`/tmp/crimson-phase5k-nvidia-codex`. `redgui` reconfigured successfully and all
24/24 Linux headless tests passed. With the configured NVIDIA FFmpeg library
path and authenticated X display, the production GUI smoke passed:

```text
[PlaybackSmoke] PASS start_frame=0 end_frame=300 presented_frame=300
presented_slot=0 view_idx=0 presented_count=351 elapsed_s=2.99304
```

## Remaining Phase 5L Work

Phase 5L.3 will wire and exercise every stable read-only command and lifecycle
transition, including buffer-row selection, menu actions, camera navigation,
source/representation selection, overlay toggles, timeline seeking, and
optional-window close/reopen behavior. Phase 5L.4 will create controlled Mac
reference bundles and structural/region screenshot comparisons.

The new Windows laptop can run the existing PowerShell capture and CMake
procedure later. Until those commands are run and inspected on that host,
Windows remains an explicit evidence gap rather than a blocker for this
Mac-specific checkpoint.
