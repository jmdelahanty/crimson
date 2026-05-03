# Crimson GUI Smoke Testing TODO

## Goal

Add lightweight, repeatable GUI smoke checks for `redgui` so a Codex agent or
developer can validate startup, data loading, and basic visual rendering against
known Palette canary Zarr archives.

## Current Manual Setup

On the workstation Xwayland session, GUI launch works from tmux with:

```bash
export DISPLAY=:1
export XAUTHORITY=/run/user/$(id -u)/.mutter-Xwaylandauth.9MIFN3
```

Useful installed tools:

- `xwininfo`
- `xprop`
- `xwd`
- ImageMagick `import`
- ImageMagick `convert`

`xdotool` is not currently installed, so automated clicks/key presses are
deferred.

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

## Future App-Side Smoke Hooks

External X11 smoke tests are useful but brittle. A more robust path is to add
explicit app-side smoke-test flags, for example:

```bash
./release/redgui \
  --zarr <analysis.zarr> \
  --ui-smoke-test calibration_subject_masks \
  --ui-smoke-frame 56 \
  --ui-smoke-screenshot /tmp/redgui_smoke.png \
  --ui-smoke-exit-after-ready
```

Potential hooks:

- Seek to a deterministic frame after all data loads.
- Open a specific panel or timeline window.
- Enable specific overlay groups.
- Render one frame.
- Save a screenshot.
- Exit with nonzero status if required data or overlays are missing.

## Optional Interaction Automation

If `xdotool` is installed later, add scripted interaction checks:

- Toggle pause/play.
- Step backward and forward with keyboard shortcuts.
- Click overlay checkboxes.
- Switch timeline traces.
- Capture screenshots after each action.

Keep these tests separate from startup smoke checks because coordinate-driven
ImGui automation can be sensitive to window size and layout.
