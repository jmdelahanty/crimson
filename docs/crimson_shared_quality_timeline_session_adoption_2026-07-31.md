# Shared Quality Timeline Session Adoption

Date: 2026-07-31

Status: macOS and Linux/Windows application paths adopted

## Scope

`crimson::gui::QualityTimelineSession` is now the single backend-neutral owner
of detection- and keypoint-quality timeline lifecycle policy for both
application shells. It owns:

- lazy asynchronous repository opening;
- closed, opening, ready, and failed state transitions;
- bounded current-window and full-recording overview requests;
- discontinuity forwarding, page publication, and error state;
- cancellation and close behavior; and
- durable open, repository, and buffer diagnostic snapshots.

The macOS shell no longer implements separate detection and keypoint timeline
state machines. This adoption removes 196 net lines from
`src/platform/macos/crimson_macos_main.mm` in the checkpoint diff.

## Repository Boundary

The session retains its path-based exact-schema open behavior used by
Linux/Windows. It also accepts optional backend-neutral repository factories.
The macOS adapter uses those factories to:

- open detection quality from the already-open analysis `ArchiveContext`; and
- derive keypoint quality from the already-open `KeypointOverlayBuffer`.

This avoids repeating mounted-store discovery, metadata validation, or keypoint
repository initialization merely to display a timeline. Factory identity is
still governed by `QualityTimelineSessionRequest`; changing the selected
archive or run closes the old session before adopting the new request.

## Ownership Boundary

The shared session does not own:

- Metal, CUDA, OpenGL, decoder, or window resources;
- ImGui window placement or workspace visibility;
- the playback seek implementation; or
- production run selection and storage contracts.

Each application shell supplies artifact selection, enabled-window state, the
current camera frame, discontinuity state, and the platform seek callback. The
existing shared quality-timeline window remains responsible for plotting and
click-to-seek intent.

## Diagnostics

`QualityTimelineSessionMetrics` preserves the last successfully opened
descriptors plus open, repository, and page-buffer metrics. Metrics are
captured before buffers release their repositories, so closing a timeline no
longer erases its physical-read and cache evidence before application
shutdown.

## Validation

- macOS `crimson_macos_shell` builds and links.
- The new headless `quality_timeline_session_tests` covers lazy factories,
  concurrent detection/keypoint readiness, frame-window publication, and
  metrics retained after close.
- Existing detection- and keypoint-quality timeline tests pass.
- An isolated ws1 Linux build compiles and links `redgui` with CUDA 12.4 and
  architectures `80;86`.
- The same headless session test passes in that Linux build.

Native Windows validation remains pending.
