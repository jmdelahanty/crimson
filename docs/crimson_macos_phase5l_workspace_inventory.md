# Crimson Phase 5L.0 Maintained Workspace Inventory

Date: 2026-07-16

This inventory is the source and runtime baseline for Phase 5L workspace
parity. It describes the maintained Linux/Windows `redgui` application, not
the current Mac shell. `W` means stable playback or read-only behavior, `D`
means diagnostics, and `X` means a mutation, inference, or legacy workflow
that remains structurally deferred.

## Evidence Boundary

- Source was audited from the cumulative Crimson tree represented by the
  isolated NVIDIA build at `/tmp/crimson-phase5k-nvidia-codex/release/redgui`.
- The final exact-state Linux runtime used executable SHA-256
  `a3320bf8f80e5351815fad95b63f7e1f0796ec0d22793171e1929b422bbf2187`
  (91,976,032 bytes) on an NVIDIA RTX A6000 with driver `580.173.02`.
  Earlier empty/default/observational captures retain historical executable
  SHA-256 `e42803cff3a2731c98c88c162de974e2d2b4ab7d5386c0a47c623d62f8554610`.
- The loaded reference used the June 14 GoodCopBadCop analysis archive listed
  in `docs/reference/phase5l/manifest.json`.
- The cumulative build checkout is intentionally dirty because it contains the
  Mac-port work packages applied to the maintained source. The executable hash,
  capture metadata, source references, and captured `imgui.ini` are therefore
  the authoritative runtime identity; the checkout HEAD alone is insufficient.
- A Windows runtime host is not available. Windows source topology is shared
  with Linux, but Windows pixels, DPI behavior, and native-window geometry are
  explicitly unverified. `docs/reference/phase5l/windows/README.md` records the
  missing evidence and capture procedure. The PowerShell harness is ready for
  that host but is not itself runtime evidence. Linux screenshots are not a
  Windows substitute.
- A metadata-only scan of all 114 production analysis archives mounted under
  `/Volumes/johnsonlab/jeremy/recordings` on 2026-07-16 found no tail+stimulus
  fixture: 103 have stimulus only, one has tail only, and ten have neither.
  The sole tail archive is Sleepyfish camera 2010095 and has no stimulus run.
  Crimson's TensorStore timeline probe successfully opened its 1,188,000-frame
  `tail_kinematics_hardened_w2_canary_20260715_01` run with four traces. The
  maintained `redgui` runtime then loaded that tail run and rejected the
  combined preset with exit code 2 because stimulus analysis is absent.
- Source line ranges below identify the audited maintained workspace before the
  additive reference hook shifted `red.cpp`; the hook does not change the
  classified window modules or their ordinary interactive behavior.

## Host Topology

Maintained `redgui` has one GLFW/OpenGL native window titled `Red`. Its client
is created at `1920x1080` (`src/gx_helper.h:45-67`, `src/red.cpp:1128-1137`).
All application surfaces are independent, movable and collapsible ImGui
windows inside that client. Docking and platform viewports are commented out,
and no active dockspace or main menu bar exists (`src/gx_helper.h:77-95`,
`src/red.cpp:6688-6697`).

Ordinary windows call `ImGui::Begin` without a close pointer. They may be
collapsed but cannot be closed. Advanced Crop Preview is the only normal
window with a live close boolean. Modal dialogs have their own dismissal
controls.

The maintained theme is `ImGui::StyleColorsClassic()`. Roboto Regular is
loaded at 15 px and Fork Awesome is merged at 15 px with pixel snapping
(`src/gx_helper.h:86-134`). ImPlot otherwise uses defaults with a purple
crosshair (`src/red.cpp:1281-1283`).

The application leaves `ImGuiIO::IniFilename` at its default, so window
position, size, and collapsed state persist to CWD-relative `imgui.ini`.
Visibility booleans, selected tabs, overlay toggles, and analysis trace choices
are not serialized and reset to their C++ defaults. The clean loaded profile
demonstrates that File Browser, Frame Inspect, Diagnostics, and Frames in the
buffer all initially occupy `Pos=60,60`; submission order therefore causes
overlap. The 1280x800 capture also proves that shrinking the native client does
not reflow those persisted ImGui positions.

## Window Inventory

| Window | Reachability and default geometry | Owner and behavior | Class |
|---|---|---|---|
| File Browser | Always submitted; only source-proven initial window. No explicit size/position or close control. `src/gui/file_browser_window.cpp:232-235`, `src/red.cpp:2796-2839` | Function-static `FileBrowserWindowState`; owns file dialogs, path editor request, and legacy enable flag. Inputs are path/config/playback state; outputs are load/config/action requests. | W, with X menu groups |
| Frame Inspect | Submitted when video is loaded. First-use `760x840`; no close control. `src/gui/frame_debug_window.cpp:10-44`, `src/red.cpp:2939-3081` | `FrameDebugWindowState`; default tab Detect. Reads frame/Zarr status and owns transient presentation/review state. Stable controls output selections, seeks, and overlays; mutation panels are X. | W/X |
| Diagnostics | Submitted after Frame Inspect when video is loaded. No explicit geometry or close control. `src/gui/diagnostics_window.cpp:5-88`, `src/red.cpp:3080-3083` | Stateless action surface over runtime/decode state. Dump commands write local debug artifacts, never Zarr. | D |
| Frames in the buffer | Visible while loaded and paused, including the default paused state. First-use `500x440`; no close control. `src/red.cpp:3725-3998`, `src/stimulus_playback.h:59-78` | `PlaybackState` plus local selected-row state. Reads decode-ring metadata; row selection changes the displayed frame. | W |
| Camera view, one per camera | Visible when video is loaded. First-use `500x400`; fewer than eight cameras use nominal 500 px columns at y=200/600. `src/red.cpp:4012-4125` | Renderer texture plus ImPlot pan/zoom state and transport controls. Collapsing a camera suspends its decode demand and is an intentional performance control. | W |
| Advanced Crop Preview | Default hidden; requires its keypoint-review toggle, loaded Zarr, and suitable crop/keypoint/mask data. First-use `300x300`, constrained `120x120` through `420x700`. `src/red.cpp:6163-6315`, `src/gui/crop_preview_window.cpp:1255-1283` | `CropPreviewWindowState` and `ChainedCropImageProvider`; live geometry precedes persisted crop. Presentation is read-only, but the window is coupled to a keypoint edit workflow. | W presentation, X edits |
| Stimulus | Default hidden; appears with Stimulus Frames in Buffer when stimulus is loaded and debug windows are enabled. First-use `480x360`. `src/red.cpp:6318-6329`, `src/gui/stimulus_playback_windows.cpp:288-369` | Stimulus decoder/presentation state; independent diagnostic texture view. | D |
| Stimulus Frames in Buffer | Same gate as Stimulus. First-use `500x440`. `src/gui/stimulus_playback_windows.cpp:370-443` | Stimulus decode-ring inspection and buffered-frame selection. | D |
| Stimulus Event Timeline | Submitted for every loaded Zarr. Content determines whether events exist. No explicit top-level geometry. `src/red.cpp:6394-6410`, `src/gui/stimulus_event_timeline_window.cpp:324-719` | Static timeline state plus shared `TimelineScrollState`; default scrolling off with a five-second half span. Event selection seeks playback. | W |
| Analysis Timeline | Submitted when any supported movement, eye, tail, step, or event data is available. No explicit top-level geometry. `src/red.cpp:6433-6451`, `src/gui/analysis_timeline_window.cpp:611-616` | Static `AnalysisTimelineWindowState`; owns read-only source/representation/trace selections and shared scrolling. | W |
| Legacy Keypoints | Default hidden behind legacy manual labeling enablement. No explicit geometry. `src/gui/keypoints_window.cpp:5-9`, `src/red.cpp:6336-6369` | `LegacyLabelingState`; old manual keypoint presentation. | X |
| Legacy Labeling Tool | Same gate as Legacy Keypoints. No explicit geometry. `src/gui/labeling_tool_window.cpp:37-45` | `LegacyLabelingState`; CSV/manual labeling and triangulation workflow. | X |
| Help Menu | Default hidden, toggled by H. No explicit geometry or close control. `src/gui/auxiliary_windows.cpp:5-49`, `src/red.cpp:6461-6466` | Runtime `show_help_window`; informational, with mutation shortcuts treated as unavailable on Mac until their contracts exist. | W structure |
| Error popup | Event-driven modal, auto-sized, dismissed by OK. `src/gui/auxiliary_windows.cpp:52-71` | Runtime error string and visibility flag. | W |
| Edit Path Presets | Modal opened from File > Path Preset. First-use `900x500`; explicit Close. `src/gui/file_browser_window.cpp:92-227` | `UiPathConfig` editor. Save writes user preferences only, never recording data. | W |

`Refined Keypoint Review` and `Interpolation Debug` have implementations but no
maintained `red.cpp` call sites. They are dormant code, not workspace windows,
and must not be copied into the Mac topology.

## Command Inventory

### File Browser and Session

| Command/control | Input and output | Source | Class |
|---|---|---|---|
| File > Open | Opens a modal media chooser and creates decoder sessions from selected media. | `src/gui/file_browser_window.cpp:240-252`, `src/red.cpp:3483-3616` | W |
| File > Load Zarr Archive | Opens a single-directory chooser and loads read-only archive context. | `src/gui/file_browser_window.cpp:253-265`, `src/red.cpp:3483-3616` | W |
| File > Load Stimulus Video | Available after video load; opens an MP4 chooser and creates the stimulus decoder. | `src/gui/file_browser_window.cpp:266-275`, `src/red.cpp:3665-3689` | W |
| File > Path Preset > entries | Changes the next dialog root. | `src/gui/file_browser_window.cpp:276-294` | W |
| File > Path Preset > Edit Path Presets | Opens preference editor; Apply is transient, Save writes user config, Reset reloads defaults. | `src/gui/file_browser_window.cpp:92-227,295-298` | W |
| Buffer Type / Buffer Size | Pre-load CPU/GPU and decode-ring allocation selection. | `src/gui/file_browser_window.cpp:384-403` | W |
| Playback Preview Scale | Full, half, or quarter preview. Paused inspection remains full resolution. | `src/gui/file_browser_window.cpp:405-435` | W |
| Playback Renderer | Standard or Lightweight Playback Renderer; maintained default is lightweight. | `src/gui/file_browser_window.cpp:437-459`, `src/red.cpp:1289-1290` | W |
| Stimulus Buffer Size / Type / Decode Backend | Pre-load stimulus ring and platform decoder configuration; loaded values become status text. | `src/gui/file_browser_window.cpp:461-486`, `src/red.cpp:1291-1297` | W |
| Seek Step / Seek Accurate | Configures fast-step delta; editing the accurate field emits an exact seek request. | `src/gui/file_browser_window.cpp:488-495`, `src/red.cpp:2925-2930` | W |
| Set Playback Speed | Visible during playback; adjusts target speed from 0.1x through 1.0x. | `src/gui/file_browser_window.cpp:497-519` | W |
| Application FPS and stimulus status | Runtime status only. | `src/gui/file_browser_window.cpp:378-386,478-486` | D |

### Camera, Buffer, and Timeline

| Command/control | Input and output | Source | Class |
|---|---|---|---|
| Camera pan/zoom/autofit | ImPlot-owned camera transform; affects presentation only. No maintained camera rotation command exists. | `src/gui/camera_view_window.cpp:530-645` | W |
| Back 10, back 1, play/pause/repeat, forward 1, forward 10 | Updates `PlaybackState` through the session controller. | `src/gui/camera_view_transport_controls.cpp:30-87` | W |
| Frame slider and time readout | Drag emits preview/inaccurate seeks; release emits accurate seek. | `src/gui/camera_view_transport_controls.cpp:89-117` | W |
| Space, Left/Right, Shift+Left/Right | Playback toggle and one/ten frame steps. | `src/gui/camera_view_transport_controls.cpp:120-131` | W |
| Buffer row selection, comma/period stepping | Selects a decoded slot and displayed frame while paused. | `src/red.cpp:3792-3998` | W |
| Stimulus timeline scroll span, filters, event list | Filters events and emits click-to-seek through shared timeline state. | `src/gui/stimulus_event_timeline_window.cpp:324-719` | W |
| Analysis source/candidate/load controls | Selects read repositories and triggers lazy read-only loads. | `src/gui/analysis_timeline_window.cpp:611-900` | W |
| Analysis motion/eye/tail/stimulus toggles | Selects visible traces and representation; does not write source data. | `src/gui/analysis_timeline_window.h:11-39` and analysis timeline modules | W |

Default analysis traces are smoothed speed, smoothed heading, swim bouts,
detector response, track X/Y, all eye traces, tail angle/deflection, and
stimulus context. Instantaneous speed, raw heading, distance, and curvature are
off (`src/gui/analysis_timeline_window.h:11-39`).

### Frame Inspect Stable Controls

| Panel/control | Input and output | Source | Class |
|---|---|---|---|
| Detection dataset selector and summaries | Selects a read-only detection run and displays current-frame metadata. | `src/gui/frame_debug_status_panel.cpp:196-359` | W |
| Review filters and Prev/Next Review Frame | Filters cached review indices and seeks playback. | `src/gui/frame_debug_review_panel.cpp:7-60` | W |
| ROI inset show/match-camera-overlays/heading-normalize/width/label | Changes backend-neutral inset presentation intent. Show, camera-overlay matching, width 240, and label are default. Coordinate transforms remain adapter-owned. | `src/roi_inset_presentation.h`, `src/gui/frame_debug_status_panel.cpp:145-194` | W |
| Keypoint status, skeleton/heading contract, marker and heading toggles | Reads refined keypoint data and changes overlay visibility. | `src/gui/refined_keypoint_review_panel.cpp:26-178`, `src/gui/overlay_debug_panel.cpp:11-128` | W |
| Subject/eye mask summaries, component selection, eye geometry | Reads mask/geometry rows and changes overlay presentation. | `src/gui/frame_debug_subject_mask_tab.cpp:508-620`, `src/gui/overlay_debug_panel.cpp:129-280` | W |
| Subject-shape and tail overlay/QC/navigation | Reads subject-shape/tail rows; selection and Prev/Next emit seeks. | `src/gui/overlay_debug_panel.cpp:281-378`, `src/gui/frame_debug_tail_kinematics_tab.cpp:21-190` | W |
| Eye representation and QC/navigation | Selects eye-angle representation/rows and emits seeks. | `src/gui/frame_debug_eye_angle_tab.cpp:110-330` | W |
| Motion trail/duration/valid-only, stimulus inset, polar inset | Changes presentation over read-only motion/stimulus/chaser data. | `src/gui/overlay_debug_panel.cpp:379-489` | W |
| Show stimulus debug windows | Opens standalone stimulus decoder and ring-inspection windows. Default off. | `src/gui/overlay_debug_panel.cpp:456-470` | D |

Maintained top-level overlay defaults are keypoints and headings on, eye masks
off unless enabled by CLI, body/component masks on, eye beams/rays/arcs/labels
on, movement trail on for two seconds with valid samples only, and stimulus
debug windows off (`src/red.cpp:1182-1202`).

### Diagnostics and Deferred Mutation

| Command/control | Effect | Source | Class/Phase 5L treatment |
|---|---|---|---|
| Dump Decode Buffers | Writes local debug videos under `CRIMSON_BUFFER_DUMP_DIR`; never mutates Zarr. | `src/gui/diagnostics_window.cpp:64-88`, `src/red.cpp:3294-3338` | D, stable |
| Random Seek + Dump | Changes playback target and writes a local debug dump. | `src/gui/diagnostics_window.cpp:74-88` | D, stable |
| BBox edit/draw/reset/delete/drag and payload preview | Mutates in-memory annotation state. | `src/gui/frame_debug_bbox_panel.cpp:6-68` | X, preserve disabled structure |
| Write Manual Payload to Zarr / detection review writes | Writes detection instances and review metadata. | `src/red.cpp:3319-3475` | X, disabled |
| Full-frame keypoint edit, Save/Mark/Reset and review write | Mutates and writes refined keypoint rows/review metadata. | `src/gui/refined_keypoint_review_panel.cpp:179-318` | X, disabled |
| Crop keypoint dragging and Save/Mark/Reset | Mutates keypoint editor state and invokes the write workflow. | `src/gui/crop_keypoint_editor.cpp:406-620` | X, disabled inside crop presentation |
| Subject Mask Editor tools | Brush/lasso/polygon/paint/erase/reset mutate an edit session; Save is already disabled. | `src/gui/frame_debug_subject_mask_tab.cpp:173-435` | X, disabled |
| Legacy Manual Labeling and skeleton menu | Enables legacy windows, CSV/manual labeling, and skeleton selection. | `src/gui/file_browser_window.cpp:298-357` | X, disabled menu structure only |
| Detection > YOLOv5/YOLOv8/YOLOv8Pose | Starts inference threads and depends on platform model/runtime contracts. | `src/gui/file_browser_window.cpp:359-379`, `src/red.cpp:2840-2918` | X, disabled |

The maintained build has no visible acquisition/live/derived crop source
selector. Advanced Crop Preview internally chooses live geometry before a
persisted crop, but it is coupled to the keypoint edit workflow. Phase 5L must
not invent a maintained crop-source command.

## Runtime Capture Matrix

| Evidence | Size | State | Status |
|---|---:|---|---|
| `linux/initial_empty_1920x1080.png` | 1920x1080 client | Fresh CWD, no media; File Browser only | Deterministic |
| `linux/file_menu_1920x1080.png` | 1920x1080 client | Empty workspace with File menu open | Observational; one recorded click |
| `linux/initial_loaded_1920x1080.png` | 1920x1080 client | Fresh CWD, June archive, paused startup/default overlap | Deterministic |
| `linux/initial_loaded_1280x800.png` | 1280x800 client | Same live session externally resized; clipping behavior | Observational |
| `linux/playback_active_nongolden_1920x1080.png` | 1920x1080 client | Active playback after a recorded Space key; frame 200/201 visible while advancing | Observational, non-golden |
| `linux/workspace_roles_arranged_1920x1080.png` | 1920x1080 client crop | June archive with documentation-only ini profile separating stable windows | Deterministic profile, non-default layout |
| `linux/workspace_exact_f000056_1920x1080.png` | 1920x1080 framebuffer | Paused arranged workspace, exact camera frame 56 | Deterministic exact state |
| `linux/overlays_exact_f000056_1920x1080.png` | 1920x1080 framebuffer | Frame 56 with eye-mask overlay and four component fills visible | Deterministic exact state |
| `linux/crop_preview_exact_f000056_1920x1080.png` | 1920x1080 framebuffer | Frame 56, live texture crop, 512x512 ROI | Deterministic exact state |
| `linux/stimulus_debug_exact_f001024_1920x1080.png` | 1920x1080 framebuffer | Camera 1024 mapped to and presenting stimulus frame 0 | Deterministic exact state |
| `linux/analysis_eye_exact_f000056_1920x1080.png` | 1920x1080 framebuffer | Frame 56 with alternate eye representation index 1 and visible masks | Deterministic exact state |

The arranged capture retains its decorated outer-window source PNG and exact
crop record. It is for role/content inspection, not a default-layout golden.
The active-playback capture remains observational because its pixels advance.

The five exact captures use the application-produced, post-swap OpenGL front
buffer as the primary PNG. Each bundle also retains an `.x11.png`; Xwayland
`xwd` sometimes contains incomplete direct-rendered OpenGL regions and is not
the pixel authority. The atomic ready marker proves exact requested/presented/
current/slider frame identity, a complete 100-slot camera ring, and 60 stable
rendered frames. Stimulus frame 0 is exact in the persistent texture while its
consumed decoder slot is correctly released. Ring occupancy is diagnostic and
is not a readiness condition; exact texture presentation is authoritative.
Overlay and eye-analysis readiness also waits for optional contours/ellipse
axes and proves that contours, axes, angle labels, and eye-direction geometry
were drawn. The direction geometry may be line-style gaze rays or visual
cones; this frame renders two cones and one overlap. Their presence therefore
cannot race the asynchronous optional-data loader.

`analysis-tail-stimulus` is implemented but not captured: this representative
archive has stimulus context and no tail kinematics. The runtime rejects that
state cleanly with exit code 2. The complementary tail-only Sleepyfish archive
also loads successfully and rejects the state with exit code 2 because it has
no stimulus analysis. A combined production bundle remains data-gated.

## 5L Contract Consequences

1. Preserve one native window containing independent ImGui windows; do not add
   a dockspace or native multiwindow model.
2. Share window visibility, command enablement, selection, and playback intent
   through backend-neutral state. Keep Metal/OpenGL/CUDA and native handles out
   of that contract.
3. Match labels, command order, default presentation toggles, and window roles.
   Represent X controls as disabled where their structure matters.
4. Keep ImGui layout persistence behavior explicit. The current Mac shell
   disables ini persistence and must change deliberately in 5L.1/5L.2.
5. Do not treat Advanced Crop Preview as evidence for a source selector and do
   not implement a provisional write repository.
