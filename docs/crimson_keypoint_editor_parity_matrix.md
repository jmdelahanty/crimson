# Crimson Keypoint Editor Parity Matrix

Date anchored: 2026-02-27.

## Purpose

Define the concrete behavior parity target for the upcoming standalone Zarr keypoint review/editor.
The standalone editor must match the existing CSV editor behavior and keybindings unless explicitly marked as a deliberate deviation.

## Source Anchors

- Keybinding and per-view edit loop: `src/red.cpp:5063-5138`
- Keypoint marker interaction (drag/click/hover delete): `src/gui.h:116-203`
- Triangulation action and labeling tool panel: `src/red.cpp:6106-6155`
- Save/load/jump controls: `src/red.cpp:6171-6234`
- Status text and keypoint table windows: `src/red.cpp:2036-2042`, `src/red.cpp:6004-6102`
- Reprojection implementation: `src/gui.h:244-319`
- CSV save/load helpers: `src/gui.h:457-522`, `src/gui.h:841-1000`
- Help text: `src/red.cpp:7455-7475`

## Parity Matrix

| ID | Scenario | Trigger | Preconditions | Expected State Change | Expected UI/Output | Code Anchor(s) | Test Type |
|---|---|---|---|---|---|---|---|
| KP-PAR-001 | Create frame keypoint container | Press `C` while hovering image plot | `plot_keypoints_flag=true`, frame has no entry (`!keypoints_find`) | Allocate `KeyPoints`, insert `keypoints_map[current_frame_num]` | Subsequent UI treats frame as labeled/editable | `src/red.cpp:5066-5077` | Integration |
| KP-PAR-002 | Place active keypoint | Press `W` while hovering image plot | `keypoints_find=true` | Set active keypoint `(x,y)` to mouse pos, set `is_labeled=true`, `is_triangulated=false`, auto-advance active index until last node | Active keypoint marker moves; keypoint table cell updates | `src/red.cpp:5083-5100` | Integration |
| KP-PAR-003 | Active keypoint decrement | Press/hold `A` while hovering image plot | `keypoints_find=true` | Decrement active index with lower clamp at `0` | Active highlight moves left; no underflow | `src/red.cpp:5102-5107` | Unit + Integration |
| KP-PAR-004 | Active keypoint increment | Press/hold `D` while hovering image plot | `keypoints_find=true` | Increment active index with upper clamp at `num_nodes-1` | Active highlight moves right; no overflow | `src/red.cpp:5109-5114` | Unit + Integration |
| KP-PAR-005 | Jump active to first | Press `Q` while hovering image plot | `keypoints_find=true` | Set active index to `0` | First node highlighted active | `src/red.cpp:5123-5128` | Integration |
| KP-PAR-006 | Jump active to last | Press `E` while hovering image plot | `keypoints_find=true` | Set active index to `num_nodes-1` | Last node highlighted active | `src/red.cpp:5116-5121` | Integration |
| KP-PAR-007 | Delete all frame keypoints | Press `Backspace` while hovering image plot | `keypoints_find=true` | `free_keypoints`, erase frame from `keypoints_map`, `keypoints_find=false` | Manual status flips to `None`; table clears frame context | `src/red.cpp:5131-5138`, `src/red.cpp:2036-2042` | Integration |
| KP-PAR-008 | Select node by clicking marker | Click a keypoint marker | Marker is labeled and draggable | Set `active_id[view_idx]=node` | Active highlight tracks clicked node | `src/gui.h:185-187` | Integration |
| KP-PAR-009 | Drag marker | Click+drag marker | Marker is labeled and draggable | Update marker position and force `is_triangulated=false` for that node/view | Marker position updates live; triangulation marker `T` clears for affected cell | `src/gui.h:135-142`, `src/red.cpp:6066-6087` | Integration |
| KP-PAR-010 | Delete hovered marker in current view | Press `R` while marker hovered | Marker hovered in plot | Set marker pos to `{1E7,1E7}`, set `is_labeled=false`, `is_triangulated=false`, set active node to hovered node | Marker disappears in current view only | `src/gui.h:158-167` | Integration |
| KP-PAR-011 | Delete hovered marker in all views | Press `F` while marker hovered | Marker hovered in plot | For all cameras: set pos `{1E7,1E7}`, clear labeled+triangulated, set active node | Marker disappears in every camera for node | `src/gui.h:169-181` | Integration |
| KP-PAR-012 | Triangulate via button | Click `Triangulate` | `scene->num_cams>1`, `keypoints_find=true` | Run `reprojection(...)` and mark triangulated/labeled for in-FOV projections where enough views exist | `T` appears in table for triangulated cells | `src/red.cpp:6138-6143`, `src/gui.h:244-319`, `src/red.cpp:6089-6092` | Integration |
| KP-PAR-013 | Triangulate via shortcut | Press `T` in Labeling Tool | `scene->num_cams>1`, `keypoints_find=true` | Same as button path (`reprojection`) | Same as button path | `src/red.cpp:6149-6155` | Integration |
| KP-PAR-014 | Save labels via button | Click `Save Labeled Data` | Keypoint root dir available | Persist timestamped folder with `keypoints3d.csv` and per-camera CSVs | `Last saved` timestamp updates | `src/red.cpp:6171-6182`, `src/gui.h:457-522` | Integration |
| KP-PAR-015 | Save labels via shortcut | Press `Ctrl+S` | Labeling Tool visible | Same save path and files as button | Same `Last saved` UI behavior as button | `src/red.cpp:6171-6178`, `src/gui.h:457-522` | Integration |
| KP-PAR-016 | Load most recent labels (new format) | Click `Load Most Recent Labels` with `Old format=false` | Date-time folders exist | Clear all current keypoints, resolve newest folder, load keypoints | Labeled frame count and frame contents reflect loaded folder | `src/red.cpp:6185-6207`, `src/gui.h:841-868`, `src/gui.h:870-1000` | Integration |
| KP-PAR-017 | Load most recent labels (old format) | Click `Load Most Recent Labels` with `Old format=true` | Old-format files exist | Clear all current keypoints, load deprecated format parser | Data appears from deprecated layout | `src/red.cpp:6185-6193` | Integration |
| KP-PAR-018 | Load from selected folder | Click `Load From Selected` and choose folder | Valid selection | Clear existing map, load selected folder via `load_keypoints` | Loaded frames visible; errors raise popup | `src/red.cpp:6214-6221`, `src/red.cpp:7416-7433`, `src/gui.h:870-1000` | Integration |
| KP-PAR-019 | Jump to next labeled frame | Click `Jump to Next Labeled Frame` | `keypoints_map` non-empty | Seek to `upper_bound(current_frame_num)`, wrap to begin when at end | Playback jumps to expected labeled frame | `src/red.cpp:6224-6233` | Integration |
| KP-PAR-020 | Keypoint table state rendering | Open `Keypoints` window during keypoint mode | `plot_keypoints_flag=true` | None (visualization) | Active node cell highlighted, labeled cells colored, triangulated cell shows `T`, focused row highlighted | `src/red.cpp:6004-6102` | Visual |
| KP-PAR-021 | Manual status indicator | View status panel | `plot_keypoints_flag=true` | None (visualization) | Shows `[Manual] Keypoints: Found` when current frame present else `None` | `src/red.cpp:2036-2042` | Visual |
| KP-PAR-022 | Hover gating for edit hotkeys | Press keypoint edit keys while plot not hovered | `plot_keypoints_flag=true` | No keypoint edit mutation occurs | No state changes from keypress | `src/red.cpp:5063-5142` | Integration |

## Safety and Edge Matrix

| ID | Edge Case | Expected Behavior for Parity | Current Behavior | Anchor |
|---|---|---|---|---|
| KP-EDGE-001 | `Jump to Next Labeled Frame` when `keypoints_map` empty | No crash; show disabled button or explanatory text | Unsafe dereference of `begin()` when empty | `src/red.cpp:6224-6233` |
| KP-EDGE-002 | Help text for `A/D` matches actual behavior | Help text must match implementation | Help currently inverted relative to code (`A` decrements, `D` increments) | `src/red.cpp:5102-5114`, `src/red.cpp:7458-7459` |
| KP-EDGE-003 | Concurrent CSV load safety | Deterministic loading without shared-map races | Per-camera threads mutate shared `keypoints_map` without synchronization | `src/gui.h:963-980` |
| KP-EDGE-004 | `find_most_recent_labels` when no dated folders | Return error and preserve existing state | Returns explicit error message (expected) | `src/gui.h:857-863` |

## Acceptance Criteria for Phase 0

1. Every parity row in `KP-PAR-*` is testable and traced to current source lines.
2. Known deviations/bugs are captured in `KP-EDGE-*` and linked to source lines.
3. Standalone Zarr editor implementation must explicitly reference this matrix for sign-off.

