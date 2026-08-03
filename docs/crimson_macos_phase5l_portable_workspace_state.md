# Crimson Phase 5L.1 Portable Workspace State

Date: 2026-07-16

Lifecycle: **archive-ready completed checkpoint**. The portable workspace state
is implemented and now maintained by source contracts and tests.

Phase 5L.1 introduces the backend-neutral state needed to keep the maintained
workspace rules consistent without moving playback, repositories, decode
buffers, textures, or native windows out of their existing owners.

## Ownership Boundary

`crimson_workspace_contracts` owns only value state and decisions:

- stable window submission rules derived from runtime capabilities;
- stable command enablement, including permanently disabled mutation commands;
- selected Frame Inspect view and analysis source/representation/event keys;
- the existing `CropSourcePreference` and `ReadOnlyOverlayControlState` values;
- versioned snapshot/restore with descriptor-derived selection catalogs; and
- playback intents for play, pause, seek, and bounded stepping.

The contract does not own or reference ImGui contexts, native handles,
Metal/OpenGL/CUDA resources, TensorStore repositories, decoder rings, playback
buffers, or a playback clock. Mac continues to execute intents through
`LogicalPlaybackClock` and `AppleVideoPlaybackBuffer`. Maintained redgui
continues to execute them through `PlaybackSessionController`.

No workspace state is written to disk in this phase. The snapshot is a
versioned value object for restoration and future persistence. ImGui window
position, size, and collapse persistence remains separate and is part of the
5L.2 composition work.

## Maintained Visibility Rules

The default `WorkspaceState` reproduces the maintained submission rules:

| Surface | Submission rule |
|---|---|
| File Browser | Always |
| Frame Inspect, Diagnostics, camera views | Loaded video |
| Frames in the buffer | Loaded, ready, paused playback |
| Advanced Crop Preview | Available crop presentation and explicit request; default hidden |
| Stimulus and Stimulus Frames in Buffer | Loaded stimulus video and explicit debug request; default hidden |
| Stimulus Event Timeline | Loaded Zarr archive |
| Analysis Timeline | Any supported analysis timeline capability |
| Help | Explicit request; default hidden |

Only the three maintained toggles are stored: Advanced Crop Preview, stimulus
debug, and Help. Always-submitted windows are capability-derived rather than
given invented close states.

## Commands and Playback

Read-only session, playback, diagnostics, preview, and help commands are
enabled only when their required capability is present. Detection, recording
data writes, and legacy manual labeling are always disabled by the portable
contract. This preserves their structural classification without creating a
write path.

`makePlaybackIntent` validates commands and clamps seek/step targets. It does
not advance time or retain a requested frame. Both frontends therefore keep
their existing logical playback authority and discontinuity handling.

The existing Mac crop source preference is restorable state, but 5L.1 does not
add a crop-source command to the maintained workspace. The maintained crop
preview toggle and its internal source selection remain distinct concerns.

## Selection and Restoration

The shared selection state carries stable keys rather than repository objects
or backend indices:

- Frame Inspect view;
- motion source;
- swim-bout candidate;
- eye-angle representation;
- tail-kinematics source; and
- optional stimulus event index.

Mac timeline controls use these shared values directly. Maintained redgui
publishes its exposed Frame Inspect view, stimulus event, eye representation,
and overlay selections into the same state while retaining its legacy timeline
implementation.

Restore accepts only snapshot version 1. Source and representation keys are
validated against a caller-provided catalog, with declared defaults and first
available entries used as fallbacks. Invalid tabs, event indices, crop
preferences, and overlay modes are reset to maintained defaults. An unknown
snapshot version is rejected without changing the current state.

## Deterministic Coverage

`workspace_state_tests` covers:

- empty and loaded default window visibility;
- paused/playing buffer-window behavior;
- read-only and deferred-mutation command enablement;
- play/pause, seek clamping, and one/ten-frame playback intents;
- propagation of timeline, crop, and overlay selections into snapshots; and
- valid, stale, invalid-enum, and unknown-version restoration behavior.

Validation on 2026-07-16:

- macOS Apple Silicon: `crimson_macos_shell` built; 34/34 headless CTest cases
  passed, including 23 portable cases;
- native macOS Metal playback smoke passed frames 0 through 300 against the
  mounted 4512x4512, 100 FPS production acquisition video;
- isolated NVIDIA CUDA 12.4/TensorRT 10 build: `redgui` and
  `workspace_state_tests` built; 24/24 CTest cases passed, including 23
  portable cases; and
- authenticated ws1 CUDA/OpenGL playback smoke passed for frames 0 through 300
against the representative production archive.

The Mac production smoke also exposed a sub-pixel negative viewport origin
from aspect-fit rounding. Viewport validation now uses the same small tolerance
on every edge, and all post-encoder render failures close the Metal command
encoder before leaving the frame.

Windows source compatibility remains represented by the shared C++17 target,
but a real Windows compile/runtime check is deferred at the user's direction.
It must be performed on the Windows laptop before Windows evidence is claimed.

## Next Boundary

Phase 5L.2 can now compose the Mac workspace from these rules. It should change
Mac window placement, styling, control ordering, and ImGui persistence without
moving repository or renderer ownership into `WorkspaceState` and without
adding a storage schema.
