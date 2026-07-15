# Crimson Phase 5J Stimulus Context Timeline

Date: 2026-07-15

Phase 5J adds stimulus events and canonical stimulus-step context to the native
macOS analysis timeline. It preserves the two maintained workflow roles: a
compact context lane beside analysis traces and a richer event browser with
filters, step details, an event list, and exact-frame seeking.

This checkpoint does not claim full Phase 5 UI parity. Production-tail data
acceptance, swim-bout rectangles, edit/review workflows, docking, multiwindow
behavior, and the final screenshot/image-difference gate remain open.

## Maintained Semantics

Stimulus context is not modeled as another scalar time series. Events are
discrete records at exact resolved camera frames, while canonical steps are
inclusive camera-frame intervals. A portable immutable snapshot contains both
forms without requiring ImGui, ImPlot, Metal, CUDA, OpenGL, TensorStore, or
Zarr.

The contract preserves maintained behavior:

- events sort by stimulus frame, camera frame, event type, and source row;
- unresolved camera frames sort after resolved events and are never seek
  targets;
- event labels combine the enum name, contextual name, and short unstructured
  details while suppressing structured JSON payloads;
- step intervals include both endpoints;
- when adjacent steps share a boundary frame, the later step owns that frame;
- window queries include every event in the requested frame range and every
  step that intersects it; and
- event interaction seeks to the resolved camera frame, not a media-time
  approximation.

Canonical step modes are classified as moving grating, concentric grating,
looming dot, chaser, or other. Moving- and concentric-grating attributes needed
by the maintained UI are retained in typed context records; the raw protocol
JSON is also preserved.

## Zarr and TensorStore

`OpenStimulusContextTimelineRepository` selects an explicitly requested run or
prefers the archive's latest complete, latest completed, latest successful,
then latest stimulus pointer. If those attributes are absent, it uses the
lexicographically newest run directory, matching the maintained fallback. It
reads event enum labels, events, canonical step groups, and optional grating
attributes through the shared `ArchiveContext`.

Current recordings use column arrays below
`analysis/stimulus_runs/<run>/events/`, including:

- `stimulus_frame_num`;
- `camera_frame_id`;
- `event_type_id`;
- `timestamp_ns_session`;
- `name_or_context`; and
- `details_json`.

Integer columns accept the signed and unsigned widths used by production
archives. Text accepts native string arrays or fixed-width rank-two byte
arrays. An older recording may instead contain one packed structured event
array at `analysis/stimulus_runs/<run>/events`. Each legacy row contains the
same mixed fields in a fixed binary record. The adapter retains the maintained
read-only fallback for that layout; the columnar layout is preferred.

If an event has no usable `camera_frame_id`, the adapter reuses the shared
stimulus-alignment repository to resolve its stimulus frame through the
corrected mapping. An explicit event camera frame always wins. The entire path
is native C++ TensorStore code and does not use Python or Python Zarr.

## Native macOS UI

The bounded `Analysis timeline` window now exposes a `Stimulus` tab. It shows:

- the canonical step at the current camera frame and its available attributes;
- per-type event filters with event counts;
- a large context lane with colored step intervals, exact event ticks, and the
  current-frame cursor;
- event and step tooltips;
- a scrollable filtered event list; and
- selected event details.

Clicking an event tick or event-list row pauses playback and uses the existing
exact-frame video seek path. Motion, Eye angles, and Tail tabs also receive a
compact 78-pixel stimulus context lane controlled by the `Stimulus context`
checkbox. This gives the native Mac shell the same two workflow roles as the
maintained application without requiring a second top-level window in the
smaller Mac workspace.

Launch controls are:

- `--show-stimulus-timeline` opens the analysis window on the Stimulus tab;
- `--require-stimulus-context-timeline` adds a deterministic playback-smoke
  gate; and
- `--no-stimulus-context-timeline` disables the optional repository.

The context repository opens independently of stimulus-video decoding. A
recording can therefore expose events and step context even when it has no
usable stimulus movie.

## Deterministic Coverage

`stimulus_context_timeline_tests` covers descriptor construction, maintained
ordering, unknown event types, immutable snapshots, exact-frame event lookup,
inclusive step lookup, shared-boundary precedence, intersecting windows,
time/frame conversion, nearest-event mapping, labels, and step-mode
classification.

`stimulus_context_timeline_repository_tests` writes real Zarr v3 fixtures
through TensorStore. It verifies complete/success/latest pointer precedence,
filesystem run fallback, production integer and fixed-string representations,
enum labels and counts, structured-details suppression, explicit camera-frame
precedence, corrected-alignment fallback, canonical step discovery, both typed
grating contexts, boundary ownership, and window queries.

Both tests carry `headless;portable;repository;stimulus;timeline`; the adapter
fixture also carries `tensorstore;zarr`. The production repository probe is
built by both the standard Mac preset and the isolated NVIDIA build.

## Production Validation

The mounted Mac production archive was:

```text
2026-05-29T18-11-16Z_arena_1_GoodCopBadCop_analysis.zarr
```

The repository selected `stimulus_external_ipc_20260604_01` and loaded 33
events, one canonical `CHASER` step, 19 event types, and zero unresolved camera
frames. The first event resolved stimulus frame 0 to camera frame 729; the step
covered camera frames 731 through 138731. The final native smoke opened the
Stimulus tab directly from the mounted network recording and passed frames 720
through 900 with 113 presentations, zero final PTS error, nominal thermal
state, and no stimulus-context validation error. All 32 Mac tests passed.

The cumulative source also completed a clean NVIDIA build with CUDA 12.4,
architectures 80 and 86, TensorRT 10.0.1.6, OpenCV 4.10.0 with SFM, the NVIDIA
FFmpeg stack, NVDEC/OpenGL, all three CUDA translation units, and the maintained
`redgui`. All 20 portable tests passed.

The server-local production probe selected the same May 29 run and produced the
same 33 events, one step, 19 event types, and zero unresolved camera frames in
138.7 ms. The authenticated RTX A6000 maintained GUI smoke then loaded the June
14 server archive, rebuilt 984 event entries and one `CHASER` step with no
missing camera IDs, and passed frames 0 through 300 with 351 presentations in
2.995 seconds.

Both representative production archives use the columnar event layout. The
legacy structured fallback is retained from the maintained loader and compiles
on both platforms, but no mounted representative structured archive was
available for a production read gate; that is residual compatibility risk, not
evidence for the current columnar path.

## Remaining UI Parity

Phase 5J establishes the shared event/interval contract and the native stimulus
context workflows. Phase 5 remains active. A production archive containing
`analysis/tail_kinematics_runs` is still required for tail acceptance. The
remaining parity work also includes swim-bout rectangles, edit/review
interactions, the larger workspace and multiwindow surfaces, and accepted
screenshot/image-difference tolerances.
