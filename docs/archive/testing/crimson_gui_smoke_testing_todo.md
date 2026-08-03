# Crimson GUI Smoke Testing TODO

Lifecycle: **archived; superseded by maintained harnesses** in `scripts/`,
`scripts/README.md`, `AGENTS.md`, and semantic/headless test targets.

## Goal

Add lightweight, repeatable GUI smoke checks for `redgui` so a Codex agent or
developer can validate startup, data loading, and basic visual rendering against
known Palette canary Zarr archives.

## Current Manual Setup

On the workstation Xwayland session, GUI launch works from tmux with:

```bash
export DISPLAY=:1
export XAUTHORITY=/run/user/$(id -u)/.mutter-Xwaylandauth.<current>
```

Useful installed tools:

- `xwininfo`
- `xprop`
- `xwd`
- ImageMagick `import`
- ImageMagick `convert`
- `xdotool`

## First External Smoke Script

Add `scripts/gui_smoke_redgui.sh`.

Suggested behavior:

- Set or accept `DISPLAY` and `XAUTHORITY`.
- Launch `./release/redgui` against a default canary Zarr:

```bash
/nvme1/recordings/2026-01-28T23-15-10Z_arena_2_Feeding/zarr/2026-01-28T23-15-10Z_arena_2_Feeding_analysis.zarr/
```

- Use stable launch flags:

```bash
--swap-interval 0 --frame-cap-fps 120 --no-mask-perf-log
```

- Capture stdout/stderr to `/tmp/crimson_gui_smoke_<timestamp>.log`.
- Wait until required startup log lines appear, then capture a screenshot.
- Kill the app cleanly after validation.
- Print log path, screenshot path, and pass/fail checks.

Suggested log assertions:

- `Successfully loaded zarr file`
- `Loaded Zarr calibration from 'analysis/calibration'`
- `Homography Status: VALID`
- `Refined subject mask run`
- `Eye angle run`
- `Subject shape run`
- `[TrackKinematics] Loaded run`

Suggested visual assertions:

- A `redgui` window exists.
- Screenshot capture succeeds.
- Screenshot is non-empty and not a single flat color.

## Existing Focused Smoke Scripts

`scripts/gui_smoke_stimulus_inset.sh` launches `redgui` against a canary Zarr
and asserts stimulus-step/inset startup logs.

`scripts/gui_smoke_playback.sh` launches `redgui` against a canary Zarr and uses
the app-side playback hook:

```bash
./release/redgui \
  --zarr <analysis.zarr> \
  --playback-smoke 0:120 \
  --playback-smoke-timeout 20 \
  --swap-interval 0 \
  --frame-cap-fps 120 \
  --no-mask-perf-log
```

The app seeks to the start frame, starts playback, waits until the camera
presenter actually displays at least the end frame, logs `[PlaybackSmoke] PASS`,
and exits nonzero on timeout.

`scripts/gui_smoke_compact_swim_bouts.sh` launches `redgui` against the feeding
canary by default and asserts that:

- the Zarr loads successfully,
- track kinematics load,
- at least one compact-v2 swim-bout run is discovered,
- the compact loader reports the selected candidate id,
- swim-bout candidates are populated for the timeline.

`scripts/gui_smoke_compact_eye_angles.sh` launches `redgui` against the feeding
canary with an explicit compact-dense-v2 eye-angle run and asserts that:

- the Zarr loads successfully,
- the requested compact eye-angle run is loaded directly,
- the loader reports `layout compact_dense_v2`,
- overlay-critical fields resolve by channel name:
  `left_gaze_xy`, `right_gaze_xy`, `left_eye_angle_deg`,
  `right_eye_angle_deg`, `vergence_eye_angle_deg`, `left_gaze_deg`,
  `right_gaze_deg`, `vergence_gaze_deg`, and `valid_frame`.

These scripts accept an optional Zarr path as the first positional argument and
respect `CRIMSON_REDGUI`, `DISPLAY`, and `XAUTHORITY`-style environment
overrides. On the workstation Xwayland session, use:

```bash
CRIMSON_COMPACT_SWIM_BOUT_SMOKE_DISPLAY=:1 \
CRIMSON_COMPACT_SWIM_BOUT_SMOKE_XAUTHORITY=/run/user/$(id -u)/.mutter-Xwaylandauth.9MIFN3 \
scripts/gui_smoke_compact_swim_bouts.sh
```

## Clipped Sleepyfish Resolver Smoke

`scripts/smoke_clipped_sleepyfish_probe.sh` builds and runs the non-GUI clipped
resolver probe against the Palette sleepyfish clipped smoke archive. It asserts
the finalized collection id, selected run count, parent-frame coverage,
unselected frame-pair count, bbox coordinate representation, and several clip
boundary mappings.

Default archive:

```bash
/groups/johnson/johnsonlab/jeremy/palette_smoke/sleepyfish_2026_05_05_17_45_30_cam2010093/zarr/sleepyfish_2026_05_05_17_45_30_cam2010093_analysis.zarr
```

Run:

```bash
scripts/smoke_clipped_sleepyfish_probe.sh
```

To inspect clipped startup cost by phase, enable:

```bash
CRIMSON_CLIPPED_STARTUP_TRACE=1 scripts/smoke_clipped_sleepyfish_probe.sh
```

The trace is opt-in and should stay out of normal GUI output. Expected clipped
startup records include `resolver_detail`, `resolver_load_ms`,
`primary_detail`, and `primary_load_ms`. `primary_detail` reports the expensive
detection-array phases, including frame-index reads, bbox reads, score/class
reads, optional normalized-bbox fallback reads, optional source-kind reads, row
append, sort/reorder, offset build, and cache/apply time.

For the clipped playback/bbox synchronization investigation, the trace fields,
diagnostic commands, root cause, and follow-up playback unification plan are
documented in
`docs/archive/playback/crimson_clipped_playback_sync_debugging.md`.

Useful overrides:

```bash
CRIMSON_CLIPPED_COLLECTION_ID=sleepyfish_cam2010093_allclips_pynvvc_fixed_20260518_01 \
CRIMSON_BUILD_JOBS=8 \
scripts/smoke_clipped_sleepyfish_probe.sh <analysis.zarr>
```

## Compact-v2 Swim-Bout Visual Gate

The compact-v2 loader smoke confirms schema loading and candidate discovery, but
the final gate is visual/behavioral. For the fresh compact-v2-only feeding
canary, open:

```bash
./release/redgui \
  --zarr /nvme1/recordings/2026-01-28T23-15-10Z_arena_2_Feeding/zarr/2026-01-28T23-15-10Z_arena_2_Feeding_analysis.zarr/ \
  --swap-interval 0 \
  --frame-cap-fps 120
```

Use swim-bout run:

```text
bouts_tk_hyst4_low2_latch_s005_peak_event_exp_tau025_prom4_dist010_w098_compact_v2_fresh_20260509
```

Manual checks:

- Select the fresh compact-v2 candidate in the analysis timeline.
- Confirm the selected candidate label includes compact identity such as
  `candidate_id`, `signal_id`, signal role, and speed level.
- Confirm the `speed_exponential` detector trace appears when detector response
  is enabled.
- Confirm swim-bout spans render from compact `start_frame` / `end_frame`.
- Confirm core bout spans render from compact `core_start_frame` /
  `core_end_frame`.
- Switch between compact signal variants and confirm the timeline updates.
- Confirm hierarchical-v1 candidates remain selectable when present in the same
  archive.

Known data gate for this canary:

- `analysis/swim_bout_runs.attrs["latest"]` points to the fresh run above.
- The fresh run has `layout = "compact_tabular_v2"`.
- The fresh run has no hierarchical `speed_*` bout groups.
- Default candidate `0`, default signal `4` (`speed_exponential`) has 519 bouts.
- Strict JSON validation reports `bad_json_files 0`.

## Deterministic Workspace Reference Hook

The Phase 5L read-only reference hook is implemented. It separates exact app
state from external X11 window capture:

```bash
./release/redgui \
  --zarr <analysis.zarr> \
  --ui-reference-state overlays \
  --ui-reference-frame 56 \
  --ui-reference-ready-file /tmp/redgui-reference.json \
  --ui-reference-timeout 60
```

Supported states are `workspace`, `overlays`, `stimulus-debug`, `crop-preview`,
`analysis-eye`, and `analysis-tail-stimulus`. The three required state/frame/file
arguments are atomic as a CLI contract: incomplete or unknown combinations fail
before GUI launch.

Before publishing the marker, `redgui` proves all of the following:

- the requested camera frame is paused, selected, and actually presented;
- the camera decode ring is complete;
- state-specific data and visible presentation evidence are present, including
  completed optional eye geometry for overlay/eye-analysis presets;
- stimulus debug has the exact mapped stimulus texture presented and a
  configured decoder ring;
- the state remains unchanged for at least 60 rendered frames; and
- the post-swap OpenGL front buffer is written to `<ready-file>.png`.

The atomic JSON marker records frame identities, client/framebuffer dimensions,
buffer occupancy, overlay/crop/analysis evidence, the read-only contract, and
the rendered-image path. After publishing it, the app freezes that proven front
buffer until the harness terminates the process. This avoids treating a partial
Xwayland `xwd` capture as application pixels.

`scripts/capture_redgui_workspace_reference.sh` validates the marker, retains a
companion X11 capture for native-window diagnostics, and emits hashes and
metadata. Set `CRIMSON_REFERENCE_KEEP_RUN_DIR=1` to retain the temporary log and
marker after a failed run. Set `CRIMSON_REFERENCE_MASK_PERF_LOG=1` during an
overlay failure to preserve the individual mask and eye-geometry counters.

## Optional Interaction Automation

The Linux reference host now has `xdotool`. Add these as a separate scripted
interaction suite when coordinate-stable layouts are available:

- Toggle pause/play.
- Step backward and forward with keyboard shortcuts.
- Click overlay checkboxes.
- Switch timeline traces.
- Capture screenshots after each action.

Keep these tests separate from startup smoke checks because coordinate-driven
ImGui automation can be sensitive to window size and layout.
