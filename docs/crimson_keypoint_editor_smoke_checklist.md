# Crimson Keypoint Editor Parity Smoke Checklist

Date anchored: 2026-02-27.

## Purpose

Manual smoke checklist for validating `KP-PAR-*` behavior in `docs/crimson_keypoint_editor_parity_matrix.md`.

## Prerequisites

- Build and run Crimson with keypoint editing enabled.
- Open a dataset/session with:
  - at least 2 calibrated cameras (triangulation coverage)
  - at least one unlabeled frame and one labeled frame
  - more than one keypoint node in the active skeleton
- Open the image plot, `Labeling Tool`, `Keypoints` table, and status panel.
- Prepare save/load fixtures before running:
  - writable label output directory
  - one valid new-format labels run
  - one valid old-format labels fixture
  - one valid folder for `Load From Selected`

## Ordered Smoke Steps

| Step | Parity ID | Action | Expected Result |
|---|---|---|---|
| 1 | KP-PAR-001 | On an unlabeled frame, hover the image plot and press `C`. | A frame keypoint container is created and the frame becomes editable. |
| 2 | KP-PAR-002 | Hover the image plot and press `W` at a known cursor location. | Active node is set to cursor `(x,y)`, marked labeled/not-triangulated, and active node auto-advances until the last node. |
| 3 | KP-PAR-003 | From a non-zero active node, press/hold `A`. | Active node index decrements and clamps at `0` (no underflow). |
| 4 | KP-PAR-004 | From a non-final active node, press/hold `D`. | Active node index increments and clamps at `num_nodes-1` (no overflow). |
| 5 | KP-PAR-005 | Press `Q` while plot is hovered. | Active node jumps to first node (`0`). |
| 6 | KP-PAR-006 | Press `E` while plot is hovered. | Active node jumps to last node (`num_nodes-1`). |
| 7 | KP-PAR-008 | Click a labeled keypoint marker in the plot. | Clicked node becomes active in the table/highlight state. |
| 8 | KP-PAR-009 | Drag a labeled marker to a new location. | Marker follows drag; corresponding cell clears triangulated state (`T` removed). |
| 9 | KP-PAR-010 | Hover a marker and press `R`. | Hovered marker is deleted in the current camera only, and active node switches to that node. |
| 10 | KP-PAR-011 | Hover a marker and press `F`. | Hovered node is deleted across all cameras, and active node switches to that node. |
| 11 | KP-PAR-012 | Click `Triangulate` in `Labeling Tool`. | Reprojection runs; triangulated/labeled states update and eligible cells show `T`. |
| 12 | KP-PAR-013 | Press `T` in `Labeling Tool`. | Same triangulation/reprojection result as step 11. |
| 13 | KP-PAR-014 | Click `Save Labeled Data`. | Labels are saved to a timestamped output folder and `Last saved` updates. |
| 14 | KP-PAR-015 | Press `Ctrl+S`. | Same save behavior and `Last saved` update as step 13. |
| 15 | KP-PAR-016 | Set `Old format=false`, click `Load Most Recent Labels`. | Existing in-memory keypoints clear, newest-format labels load, and frame contents/count reflect loaded run. |
| 16 | KP-PAR-017 | Set `Old format=true`, click `Load Most Recent Labels`. | Existing in-memory keypoints clear and old-format labels load correctly. |
| 17 | KP-PAR-018 | Click `Load From Selected` and choose a valid labels folder. | Existing map clears and selected folder data loads; invalid selections raise error popup. |
| 18 | KP-PAR-019 | Click `Jump to Next Labeled Frame` repeatedly near end-of-range. | Playback jumps to next labeled frame and wraps to first labeled frame at end. |
| 19 | KP-PAR-020 | Open/inspect `Keypoints` window during keypoint mode. | Active cell highlight, labeled color, triangulated `T`, and focused row highlight render correctly. |
| 20 | KP-PAR-021 | Check status panel on one labeled frame and one unlabeled frame. | Status shows `[Manual] Keypoints: Found` for labeled frame and `None` for unlabeled frame. |
| 21 | KP-PAR-022 | Move cursor outside image plot, then press keypoint edit hotkeys (`C/W/A/D/Q/E/R/F/Backspace/T`). | No keypoint state mutation occurs while plot is not hovered. |
| 22 | KP-PAR-007 | Return cursor to hovered plot on labeled frame and press `Backspace`. | All keypoints for current frame are deleted, frame container removed, table context clears, and status switches to `None`. |

## Result Recording

Run metadata:

| Field | Value |
|---|---|
| Date |  |
| Tester |  |
| Build/Commit |  |
| Dataset |  |

Scenario results:

| Parity ID | Step | Pass/Fail | Notes / Bug ID |
|---|---|---|---|
| KP-PAR-001 | 1 |  |  |
| KP-PAR-002 | 2 |  |  |
| KP-PAR-003 | 3 |  |  |
| KP-PAR-004 | 4 |  |  |
| KP-PAR-005 | 5 |  |  |
| KP-PAR-006 | 6 |  |  |
| KP-PAR-007 | 22 |  |  |
| KP-PAR-008 | 7 |  |  |
| KP-PAR-009 | 8 |  |  |
| KP-PAR-010 | 9 |  |  |
| KP-PAR-011 | 10 |  |  |
| KP-PAR-012 | 11 |  |  |
| KP-PAR-013 | 12 |  |  |
| KP-PAR-014 | 13 |  |  |
| KP-PAR-015 | 14 |  |  |
| KP-PAR-016 | 15 |  |  |
| KP-PAR-017 | 16 |  |  |
| KP-PAR-018 | 17 |  |  |
| KP-PAR-019 | 18 |  |  |
| KP-PAR-020 | 19 |  |  |
| KP-PAR-021 | 20 |  |  |
| KP-PAR-022 | 21 |  |  |

## Automation Hooks

Best near-term automated coverage candidates:

- `KP-PAR-003/004/005/006`: active-node index transition and clamp unit tests.
- `KP-PAR-007`: frame delete-all state transition and status flip assertion.
- `KP-PAR-012/013`: triangulation button vs shortcut equivalence test.
- `KP-PAR-014/015`: save button vs `Ctrl+S` parity and timestamp/file presence checks.
- `KP-PAR-016/018`: load flows clear prior state and populate expected frames.
- `KP-PAR-019/022`: jump/wrap and hover-gating regression integration tests.
