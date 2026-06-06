# Stimulus Alignment and Overlay Data Flow

This document explains how the visualization stack loads stimulus-aligned data from a Palette
Zarr archive, which datasets are consulted, and how the different “mapping variants” show up in
the UI.

## Stimulus Run Selection

All stimulus-specific assets live under `analysis/stimulus_runs/<run>/`. When a Zarr archive is
opened, `ZarrDetectionLoader`:

1. Tries to read `analysis/stimulus_runs/zarr.json` for a `latest*` attribute
   (`latest`, `latest_completed`, etc.).
2. If no attribute is found, falls back to the lexicographically last run present on disk.
3. Uses the chosen run as the root for all subsequent loads (alignment, events, chaser tracks).

Every time we point the loader at a different stimulus run, the entire stimulus subtree is
reloaded.

## Frame Alignment Data

Within the chosen run we inspect `frame_alignment/`:

- `camera_to_stimulus_frame_corrected` (optional): a dense array mapping each camera frame index
  to a sequential stimulus frame ID. This is the “corrected” timeline created by the importer.
- `camera_stimulus_frame_interpolated` (optional): a Boolean array flagging which entries in the
  corrected timeline were synthesized.
- `camera_to_metadata_index` and `camera_to_metadata_index_corrected`: legacy arrays that map
  camera frames to rows in `video_metadata/frame_metadata`.
- `camera_interpolation_mask` and `interpolation_mask`: used to flag camera frames/stimulus rows
  that were interpolated.
- `camera_frame_offset`: stored in run attrs, used to align the absolute camera frame IDs.

If the direct corrected arrays are available, we mark the mapping variant as “corrected” and use
them everywhere. Otherwise we fall back to the legacy path:

```
camera frame -> camera_to_metadata_index -> video_metadata/frame_metadata/stimulus_frame_num
```

The UI panel reflects this choice (“Mapping variant: corrected/legacy”).

## Stimulus Events and Metadata

Regardless of mapping variant we always try to load:

- Stimulus event enums from `analysis/enums/events/*`.
- Run-specific events from either the columnar layout
  (`analysis/stimulus_runs/<run>/events/<column>`) or the structured layout
  (`analysis/stimulus_runs/<run>/events`), depending on which exists.
- `video_metadata/frame_metadata` columns (`stimulus_frame_num`, `triggering_camera_frame_id`) as
  a fallback for the legacy mapping and for cross-referencing events/chaser tracks.

## Stimulus Timeline UI Surfaces

Crimson intentionally keeps two stimulus timeline surfaces because they serve different workflows:

| Surface | Code | Role |
|---------|------|------|
| Stimulus Event Timeline | `src/gui/stimulus_event_timeline_window.cpp` | Dedicated stimulus inspection window. Shows event-type filters, a larger event timeline, canonical step details, and a scrollable event list. Event clicks and list selections request a seek to the resolved camera frame. |
| Analysis Timeline Stimulus Context | `src/gui/analysis_timeline_stimulus_context.cpp` | Compact contextual row embedded in the Analysis Timeline. Its job is to align stimulus steps/events visually with motion, eye-angle, tail, and other analysis traces. |

The dedicated Stimulus Event Timeline is the richer event browser. It owns event selection state,
filter controls, and the event-list table. The embedded Stimulus Context should stay lightweight
and analysis-oriented: it is a reference lane for comparing stimulus timing against other traces,
not a replacement for the event browser.

When adding interactions, keep the surfaces consistent but not identical. Click-to-seek on an
event is appropriate for both surfaces because it uses the same resolved camera-frame target.
Detailed filtering, long labels, and event-table affordances belong in the dedicated Stimulus
Event Timeline unless there is a clear analysis-trace workflow that needs them inline.

## Chaser Data Sets

Each run can expose multiple chaser-tracking datasets under `tracking_data/`:

| Dataset                              | Contents                                                        | Usage                                                                 |
|-------------------------------------|-----------------------------------------------------------------|-----------------------------------------------------------------------|
| `chaser_states`                     | Sparse tracker output (stimulus frame, target/chaser positions) | Legacy overlays; used if no dense data is available                   |
| `chaser_states_interpolated` (new)  | Dense, corrected timeline of chaser/target states               | Preferred source for overlays; indexed by camera frame via corrected mapping |
| `chaser_bounding_boxes`             | Sparse bounding boxes (camera frame ID, bbox geometry)          | Used for drawing target boxes; still sparse unless producer densifies |

When `chaser_states_interpolated` is present we rebuild per-stimulus indices so that each
corrected stimulus frame can be resolved in O(1) time. For overlays we now query datasets in
this priority order:

1. `getChaserInterpolatedStatesForCameraFrame(frame)` – converts the camera frame to a corrected
   stimulus frame (via `camera_to_stimulus_frame_corrected`) and returns the dense row.
2. `getChaserStatesForStimulusFrame(stimulus_frame)` – legacy sparse states keyed by stimulus
   frame, used when the dense dataset is missing.
3. `getChaserStatesForFrame(frame)` – legacy sparse states keyed by camera frame, used last.

Bounding boxes still come from `chaser_bounding_boxes`. As long as that dataset remains sparse,
boxes may flicker even when the dense chaser states are present.

## Mapping Variant in the UI

The “Stimulus Alignment” box in the Frame Debug panel simply mirrors which lookup table
`ZarrDetectionLoader` is currently using for camera→stimulus translation:

- `corrected`: `camera_to_stimulus_frame_corrected` and
  `video_metadata/frame_metadata/stimulus_frame_num_corrected` are available and drive playback.
- `legacy`: the loader could not find the corrected arrays, so it falls back to the legacy
  camera→metadata→stimulus mapping.

This status applies to stimulus playback and any helper that calls
`getStimulusFrameForCameraFrame(..., prefer_corrected=true)`. Overlay code mirrors that behavior
by trying the corrected timeline first and falling back to legacy data when necessary.

## What’s Still Missing to be Fully Dense?

The new interpolated dataset gives us per-frame chaser/target positions, but bounding boxes are
still sparse. To eliminate every source of flicker the producer needs to extend
`chaser_states_interpolated` (or add a sibling dataset) with per-frame bbox geometry so the
visualizer no longer depends on the sparse `chaser_bounding_boxes` table.
