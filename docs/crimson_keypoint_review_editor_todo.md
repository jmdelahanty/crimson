# Crimson Keypoint Review Editor TODO

Date anchored: 2026-02-27.

## Goal

Build a standalone keypoint review/editor flow for Zarr-backed data that matches current CSV keypoint editing UX and behavior (same keybindings, same editing semantics), while adding robust writeback and review-acceptance support.

## Scope

- In scope:
  - standalone keypoint review/editor UI module (not continued expansion of `gui.h`)
  - behavior parity with CSV keypoint editor (`C/W/A/D/Q/E/R/F/Backspace/T/Ctrl+S`)
  - in-memory edit state + apply/discard lifecycle for Zarr keypoints
  - keypoint writeback path to refined keypoint run (contract-aligned)
  - keypoint review acceptance metadata write path (contract-aligned)
  - frame/reason-based review navigation for keypoint issues
- Out of scope for this effort:
  - redesigning keypoint interaction model
  - introducing remote API auth/RBAC
  - full dataset creation when no refined keypoint run exists (fail-closed initial policy)
  - generalized metrics framework (PCK/OKS/MPJPE) beyond minimal verification hooks

## Product Requirements (Parity Contract)

The new review editor must preserve existing CSV editing behavior:

1. Same keybindings and actions:
- `C`: create frame keypoint container
- `W`: place active keypoint at cursor and advance active index
- `A/D`: active keypoint decrement/increment
- `Q/E`: jump active keypoint to first/last
- `R`: delete hovered keypoint in current camera
- `F`: delete hovered keypoint in all cameras
- `Backspace`: delete all keypoints for current frame
- `T`: triangulate/reproject
- `Ctrl+S`: save/apply edits

2. Same visible behaviors:
- active keypoint highlighting
- labeled/triangulated state visuals
- hover tooltip for triangulated 3D point
- same table semantics (camera rows, node columns, active/labeled/triangulated indicators)
- same interaction guards (hover requirements, clamping, no crash on empty frame)

3. Same lifecycle semantics:
- deterministic per-frame edit persistence while navigating
- explicit apply/discard operations
- unsaved-change protection before dataset/source switches

## Baseline (Current State)

- CSV/manual keypoint editor exists and works in `src/gui.h` + `src/red.cpp`.
- Zarr keypoint overlays and review status display are read-oriented in `src/red.cpp` + `src/zarr_loader_eye_keypoint.cpp`.
- Detect editing has a useful architectural precedent (`src/zarr_bbox_edit.h`, detect write flow in `src/zarr_loader_write.cpp`).
- Keypoint write and acceptance contracts exist:
  - `docs/crimson_keypoint_manual_write_contract.md`
  - `docs/crimson_keypoint_review_acceptance_contract.md`
- Missing today:
  - no keypoint write API in loader
  - no keypoint review-acceptance writer
  - no dedicated keypoint editor core (behavior currently coupled to immediate-mode UI and global state)

## Architecture Decision

Use dedicated modules; do not keep extending `gui.h`.

Planned module boundaries:

1. `src/keypoint_editor_core.h/.cpp`
- shared command/state transitions for CSV and Zarr modes
- keybinding handling + state mutation semantics
- no direct Zarr IO

2. `src/keypoint_review_panel.h/.cpp`
- standalone review/editor UI panel/window
- owns panel-specific controls, preview/apply/accept buttons, and status text
- thin integration surface from `red.cpp`

3. `src/zarr_keypoint_edit_state.h/.cpp`
- in-memory dirty state for Zarr keypoint edits
- per-frame/per-detection row edit records
- apply/discard bookkeeping + unsaved counters

4. `src/zarr_keypoint_review_core.h/.cpp`
- pure logic for quality recompute, reason/tag merge, summary updates, signature creation

5. `src/zarr_loader_keypoint_write.cpp`
- keypoint writeback and acceptance writes
- parallels `zarr_loader_write.cpp` style and error handling

6. `src/review_keypoint_state.h/.cpp`
- keypoint-focused frame index/cache and filters (reason/status-driven navigation)

## Work Plan

### Phase 0: Lock Behavioral Parity Spec

- [x] P0.1 Create a behavior matrix from existing CSV editor codepaths
- [ ] P0.2 Resolve and document current help-text mismatch for `A/D` semantics
- [ ] P0.3 Add parity test checklist (manual + automated smoke hooks)

Phase 0 artifact:
- `docs/crimson_keypoint_editor_parity_matrix.md`

Definition of done:
- A written parity matrix exists and is referenced by implementation/review.

### Phase 1: Extract Reusable Editor Core

- [ ] P1.1 Introduce `keypoint_editor_core` state/actions API
- [ ] P1.2 Move CSV keybinding/action logic behind core calls (no behavior change)
- [ ] P1.3 Move drag/delete hover state out of function-static locals into explicit editor state
- [ ] P1.4 Add guardrail fixes discovered during extraction:
  - empty-map safety for "jump to next labeled frame"
  - synchronize or de-thread unsafe shared-map load path in CSV loader

Definition of done:
- CSV flow still behaves identically while using shared core interfaces.

### Phase 2: Standalone Zarr Keypoint Review Editor UI

- [ ] P2.1 Add `keypoint_review_panel` and wire from `red.cpp`
- [ ] P2.2 Use same editor-core keybindings/semantics as CSV mode
- [ ] P2.3 Provide same keypoint table/active-row/triangulation visuals
- [ ] P2.4 Add edit lifecycle controls:
  - apply frame
  - apply all pending
  - discard frame
  - discard all
- [ ] P2.5 Add unsaved-change guard before dataset/archive switches
- [ ] P2.6 Ensure editor can run standalone (independent UI path, not CSV mode piggyback)

Definition of done:
- Zarr review editor looks and behaves like CSV editor for core edit operations.

### Phase 3: Zarr Keypoint Write Path (Manual Refined)

- [ ] P3.1 Add loader API surface for keypoint write operations in `zarr_loader.h`
- [ ] P3.2 Implement `zarr_loader_keypoint_write.cpp` with contract-aligned targets
- [ ] P3.3 Add partial row/slice write helper(s) needed for per-ROI keypoint updates
- [ ] P3.4 Implement reason synchronization writes (`reason_bytes` + `reason` when available)
- [ ] P3.5 Implement post-edit recompute pipeline:
  - heading finite/usable
  - confidence/geometry/usable flags
  - reason-tag updates and normalization
  - `summary_statistics.postprocess` refresh
- [ ] P3.6 Reload and verify round-trip visibility in UI

Definition of done:
- Manual keypoint edits persist to the refined keypoint target and reload cleanly.

### Phase 4: Review Acceptance Metadata Path

- [ ] P4.1 Implement acceptance payload builder/validator (fail-closed)
- [ ] P4.2 Write `keypoint_review_status` and related signatures
- [ ] P4.3 Update latest pointers/attrs required by acceptance contract
- [ ] P4.4 Add explicit UI action for approve/reject/needs-attention status update
- [ ] P4.5 Add audit logging in Crimson action history

Definition of done:
- Acceptance metadata can be authored from Crimson and is contract-compatible.

### Phase 5: Keypoint-Focused Review Navigation

- [ ] P5.1 Add `review_keypoint_state` frame cache and filters
- [ ] P5.2 Add filter presets (example: unusable/manual/reason-tagged/failure)
- [ ] P5.3 Add deterministic next/prev issue navigation
- [ ] P5.4 Display multi-reason details in panel (not first-tag-only summary)

Definition of done:
- Reviewers can quickly traverse keypoint-problem frames with meaningful filters.

### Phase 6: Hardening and Test Coverage

- [ ] P6.1 Unit tests for editor-core action semantics (index bounds, state transitions)
- [ ] P6.2 IO tests for write helpers and contract-required attrs/arrays
- [ ] P6.3 Golden round-trip test: load -> edit -> write -> reload equivalence checks
- [ ] P6.4 Concurrency and safety checks for loader/editor edge cases
- [ ] P6.5 Regression checklist against CSV parity matrix

Definition of done:
- No known parity regressions and write/acceptance paths are validated by tests.

## Missing Pieces Checklist (Repository Gaps to Close)

- [ ] Shared keypoint editor core abstraction (currently coupled to UI/global state)
- [ ] Dedicated standalone keypoint review/editor panel module
- [ ] Keypoint manual write APIs in `ZarrDetectionLoader`
- [ ] Keypoint review-acceptance writer path
- [ ] Slice/partial write primitives for row-level keypoint overwrite
- [ ] Keypoint quality/reason/signature recompute utilities
- [ ] Stable editor-facing row identity mapping for Zarr keypoint rows
- [ ] Keypoint-specific review filter/cache module
- [ ] Unsaved-change lifecycle guardrails for new editor path

## Verification Plan

Manual verification:

1. CSV parity sanity:
- run through full keybinding set and compare against baseline behavior

2. Zarr review editor parity:
- same keybinding sequence on representative frames yields same local state changes

3. Writeback:
- edit multiple rows across frames, apply, reload, verify arrays/attrs and UI rendering

4. Acceptance:
- write approve/reject states and verify signatures + pointers update as expected

5. Safety:
- switch dataset with unsaved edits and confirm guard behavior
- verify no crash on empty/no-keypoint frames

Automated verification targets:

- editor-core state transition unit tests
- zarr write/read round-trip tests
- acceptance metadata contract tests

## Risks / Watchouts

- Row identity mismatches between displayed detection rows and write target rows.
- Divergence risk between CSV and Zarr behavior if parity is not enforced via shared core.
- Contract drift: implementation details may diverge from current manual/acceptance docs.
- String dtype support variance for `reason` write compatibility.
- Existing CSV loader race can contaminate parity tests if not fixed early.

## Open Questions

1. If no refined keypoint run exists, do we hard-block write (initial policy) or scaffold one?
2. Should acceptance require `reviewer` for approved state by default?
3. For manually edited previously-interpolated rows, do we relabel as `manual` unconditionally?
4. Should standalone keypoint review also ship as an optional separate binary under `tools/`?

## Execution Order (Recommended)

1. Phase 0 (parity spec)
2. Phase 1 (extract shared core, no behavior changes)
3. Phase 2 (standalone review panel + lifecycle controls)
4. Phase 3 (manual write path)
5. Phase 4 (review acceptance)
6. Phase 5 (review navigation improvements)
7. Phase 6 (hardening and tests)
