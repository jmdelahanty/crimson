# Crimson CLI Recording Path & Modularization TODO

Date anchored: 2026-02-09.

---

## Part A: CLI `--recording` Support

### Goal

Allow `crimson --recording /path/to/session` to auto-discover zarr archives, video files, and calibrations without any file dialogs.

### Tasks

- [ ] **A1. Parse `--recording <dir>` argument** (`red.cpp:1089-1101`)
  - Add `cli_recording_path` string alongside `cli_zarr_override_path`
  - Parse it in the same arg loop

- [ ] **A2. Mutual exclusion check**
  - If both `--zarr` and `--recording` are provided, prefer `--recording` (clear `cli_zarr_override_path`)
  - Print a warning to stderr

- [ ] **A3. Add recording directory loading block** (after `red.cpp:1373`)
  - Set `root_dir` and `skeleton_dir` from `cli_recording_path`
  - Call `loadZarrDetectionFromDirectory()` for zarr discovery
  - On success: set `zarr_loaded`, call `refreshDetectionDatasetOptions()`, clear bbox edit state, call `tryAutoLoadAffiliatedVideoFromZarr("--recording")`

- [ ] **A4. Fallback: load video without zarr**
  - If no zarr found, scan `<recording_dir>/cams/` then `<recording_dir>` for `.mp4` files
  - Pick first `.mp4`, create `FFmpegDemuxer`, allocate scene, spawn decoder thread
  - Mirror the setup from `tryAutoLoadAffiliatedVideoFromZarr` (demuxer, scene alloc, decoder thread, seek, calibrations)
  - Set `video_loaded = true`

- [ ] **A5. Handle nonexistent path**
  - If `cli_recording_path` is not a valid directory, print error to stderr, continue to GUI normally

### Verification

- `./redgui --recording /path/to/recording` -- video + zarr loaded, no dialogs
- `./redgui --recording /path/without/zarr` -- video only
- `./redgui --recording /nonexistent` -- error printed, GUI opens normally
- `./redgui` (no args) -- unchanged behavior
- `./redgui --zarr /path` -- unchanged behavior
- `cmake --build build` -- compiles cleanly

---

## Part B: Modularization Refactor

Each phase is one commit. Build + run after each to verify. Zero functional changes.

### Phase 1: Extract utility functions (~250 lines)

- [ ] **B1a. Create `src/ui_path_config.h`**
  - `UiPathConfig` struct, function declarations for `LoadUiPathConfig()`, `ResolveAffiliatedVideoPath()`, `InferRecordingRootPath()`, helper functions (`IsFiniteFloat`, `ToLowerCopy`, `IsRegularFileNoThrow`, `IsDirectoryNoThrow`, `ExpandUserPath`)

- [ ] **B1b. Create `src/ui_path_config.cpp`**
  - Move implementations from `red.cpp:50-297` (the anonymous namespace)

- [ ] **B1c. Update `red.cpp`**
  - Delete lines 50-297, add `#include "ui_path_config.h"`
  - Verify build

### Phase 2: Extract ZarrBBoxEditState (~250 lines)

- [ ] **B2a. Create `src/zarr_bbox_edit.h`**
  - `ZarrBBoxEditState` struct definition
  - `HitTestZarrBoxAtPlotPoint()` and `IsPlotPointInsideZarrBox()` declarations

- [ ] **B2b. Create `src/zarr_bbox_edit.cpp`**
  - Hit-test function implementations from `red.cpp:299-545`

- [ ] **B2c. Update `red.cpp`**
  - Delete lines 299-545, add `#include "zarr_bbox_edit.h"`
  - Keep `g_zarr_bbox_edit_state` instance in `red.cpp`
  - Verify build

### Phase 3: Extract StimulusPlayback module (~430 lines)

- [ ] **B3a. Create `src/stimulus_playback.h`**
  - `PlaybackState`, `StimulusPlayback`, `EyeOrientationSmoother` structs
  - Function declarations; `extern StimulusPlayback stimulus_player;`

- [ ] **B3b. Create `src/stimulus_playback.cpp`**
  - Move `destroyStimulusPlayback()`, `allocateStimulusBuffers()`, `initializeStimulusPlayback()`, stimulus frame helpers, `scheduleStimulusSeek()`, `seek_all_cameras()` from `red.cpp:595-635, 708-1086`

- [ ] **B3c. Update `red.cpp`**
  - Delete moved code, add `#include "stimulus_playback.h"`
  - Verify build

### Phase 4: Extract ReviewFrame types (~100 lines)

- [ ] **B4a. Create `src/review_frame_state.h`**
  - `ReviewFrameFilters`, `ReviewFrameCache` structs
  - `refreshDetectionDatasetOptions()` declaration
  - `extern` globals for `detection_dataset_ids`, `detection_dataset_labels`, `detection_dataset_choice`

- [ ] **B4b. Create `src/review_frame_state.cpp`**
  - `refreshDetectionDatasetOptions()` implementation, global definitions
  - Move from `red.cpp:558-593`

- [ ] **B4c. Update `red.cpp`**
  - Delete moved code, add `#include "review_frame_state.h"`
  - Verify build

### Phase 5a: Extract zarr_loader internal utilities (~890 lines) -- do first

- [ ] **B5a-i. Create `src/zarr_loader_internal.h`**
  - Shared helpers in `namespace zarr_internal`
  - Templates (`openArrayForWrite`) and small inlines

- [ ] **B5a-ii. Create `src/zarr_loader_internal.cpp`**
  - Move `zarr_loader.cpp:30-921` anonymous namespace functions
  - Larger functions: discovery logic, `collect_runs_fs`, write helpers

- [ ] **B5a-iii. Update `zarr_loader.cpp`**
  - Delete anonymous namespace, add `#include "zarr_loader_internal.h"`
  - Verify build

### Phase 5b: Extract eye mask, eye angle, keypoint loading (~1,260 lines)

- [ ] **B5b. Create `src/zarr_loader_eye_keypoint.cpp`**
  - Move: `loadKeypointHeadingData()`, `loadRefinedEyeMaskData()`, `loadEyeAngleData()`, `ensureEyeMaskChunk()`, `prefetchAdjacentEyeMaskChunks()`, `populateEyeMaskEntry()`
  - `#include "zarr_loader_internal.h"`
  - Verify build

### Phase 5c: Extract stimulus alignment & chaser data (~1,600 lines)

- [ ] **B5c. Create `src/zarr_loader_stimulus.cpp`**
  - Move: `loadStimulusAlignment()`, `loadStimulusEventEnums()`, `loadStimulusEventsForRun()`, `loadChaserBoundingBoxes()`, `loadChaserStates()`, `loadChaserStatesInterpolated()`, all `rebuild*Indices()`, `updateChaserCameraFramesFromAlignment()`, `loadStimulusFrameMetadataMapping()`
  - Verify build

### Phase 5d: Extract movement data loading (~700 lines)

- [ ] **B5d. Create `src/zarr_loader_movement.cpp`**
  - Move: `loadMovementData()`, `loadLegacyMovementData()`, `loadMovementTrack()`, `loadMovementCropRun()`, `finalizeMovementSelection()`
  - Verify build

### Phase 5e: Extract write operations (~440 lines)

- [ ] **B5e. Create `src/zarr_loader_write.cpp`**
  - Move: `writeManualRefinedDetections()`
  - Verify build

### Phase 5f: Extract detection run loading (~1,040 lines)

- [ ] **B5f. Create `src/zarr_loader_detections.cpp`**
  - Move: `loadPaletteInterpolationRun()`, `loadDetectionRuns()`, `loadDetectionRunFromGroup()`, `loadFlattenedRun()`, `loadRefinedDetectionsAsPrimary()`, `loadRefinedDetectRuns()`
  - Verify build

### Phase 7: Introduce CameraResources struct

- [ ] **B7a. Define `CameraResources` struct** (in `render.h` or new `camera_resources.h`)
  - `name`, `image_width`, `image_height`, `image_texture`, `pbo_cuda`, `display_buffer`, `seek_info`, `demuxer`, `decoder_thread`

- [ ] **B7b. Update `render_scene`**
  - Replace parallel C-style arrays with `std::vector<CameraResources> cameras`

- [ ] **B7c. Mechanical find-and-replace across all files**
  - `scene->display_buffer[j]` -> `scene->cameras[j].display_buffer`
  - `scene->seek_context[j]` -> `scene->cameras[j].seek_info`
  - `scene->image_texture[j]` -> `scene->cameras[j].image_texture`
  - `scene->image_width[j]` -> `scene->cameras[j].image_width`
  - `scene->image_height[j]` -> `scene->cameras[j].image_height`
  - `scene->pbo_cuda[j]` -> `scene->cameras[j].pbo_cuda`
  - Files: `render.h`, `red.cpp`, `decoder.cpp`, `stimulus_playback.cpp`
  - Verify build

---

## Execution Order

1. **A1-A5** -- CLI recording support (standalone feature, do first)
2. **B1** -- Phase 1: utility extraction (zero crimson deps)
3. **B2** -- Phase 2: ZarrBBoxEditState extraction
4. **B5a** -- Phase 5a: zarr internal utilities (must precede 5b-5f)
5. **B5b-B5f** -- Phases 5b-5f: zarr method splits (independent of each other)
6. **B3** -- Phase 3: StimulusPlayback extraction
7. **B4** -- Phase 4: ReviewFrame extraction
8. **B7** -- Phase 7: CameraResources struct (after all splits stable)

## Expected Result

| File | Before | After |
|------|--------|-------|
| `red.cpp` | ~7,582 lines | ~6,550 lines |
| `zarr_loader.cpp` | ~8,930 lines | ~3,000 lines |
| New files created | 0 | ~14 |

## Out of Scope

- Per-camera CUDA streams (separate effort)
- `AppState` extraction from `main()` (requires bundling ~dozens of locals, larger architectural change)
