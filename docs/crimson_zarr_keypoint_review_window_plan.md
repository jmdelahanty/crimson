# Crimson Zarr Keypoint Review Window Plan

Date anchored: 2026-04-04.

## Purpose

Define the next UI step for Crimson's refined-keypoint workflow:

- keep the current Zarr-native read/write path
- stop treating the legacy CSV `Labeling Tool` as the main keypoint workflow
- build a dedicated refined-keypoint review window that borrows the good parts
  of the old labeling UI without inheriting its storage model

This plan is intentionally UI- and workflow-focused. The lower-level refined
Zarr write behavior is already covered in
[crimson_zarr_keypoint_editor_plan.md](./crimson_zarr_keypoint_editor_plan.md).

## Problem

Crimson currently has three different keypoint-related UI surfaces:

1. `Keypoints`
   - legacy `keypoints_map` status table
   - not Zarr-backed
   - useful only for the old manual labeling path

2. `Labeling Tool`
   - legacy CSV save/load + triangulation workflow
   - persists to `labeled_data/`
   - has a good compact workflow layout, but the wrong backend model

3. Current refined-keypoint UI
   - split across `Frame Debug` and `Crop Preview`
   - Zarr-backed and directionally correct
   - but fragmented and less discoverable than the old labeling flow

The result is that the data model is moving in the right direction while the
operator workflow is still spread across too many windows.

## Core Goal

Make the refined-keypoint workflow feel like a first-class task window.

The operator should be able to:

- understand what refined run is loaded
- see whether the current detection is editable
- perform row-scoped manual correction
- mark `fish_present_no_keypoints`
- mark `detection_issue`
- approve or reject review status
- navigate to the next frame or detection needing attention
- see queue/status counts

without needing to mentally stitch together `Frame Debug`, `Crop Preview`, and
the old `Labeling Tool`.

## Design Principles

### 1. Keep Zarr As The Source Of Truth

The new review window must operate on:

- `refined_keypoints_runs/<run>`
- `keypoint_review_status`
- the current refined-keypoint repository seam

It must not:

- read/write `labeled_data/`
- use `keypoints_map` as its primary state model
- reuse the CSV save/load path

### 2. Borrow Workflow Shape, Not Storage Semantics

The old `Labeling Tool` has good UI traits:

- dedicated task window
- obvious primary actions
- compact status text
- navigation controls
- "what do I do next?" feel

Those are worth preserving.

What is not worth preserving:

- CSV snapshot persistence
- frame-snapshot mental model
- `keypoints_map` ownership
- triangulation-driven action layout

### 3. Keep Crop Preview As The Precision Editing Surface

The new review window should not replace Crop Preview.

Crop Preview should remain the detailed editing surface for:

- draggable ROI-local keypoints
- heading-normalized preview
- precise manual correction

The new review window should act as the workflow shell:

- state summary
- actionable buttons
- review queue navigation
- operation status

### 4. Separate Edit Actions From Review Acceptance

Manual correction and review acceptance should stay separate.

The UI must keep this distinction explicit:

- edit actions mutate row data and derived fields
- review actions mutate acceptance metadata

Do not blur those into one "save everything" button.

## Proposed UX

## Window Name

Recommended initial name:

- `Refined Keypoint Review`

This is clearer than reusing `Labeling Tool`, which implies the legacy path.

## Layout

### Section 1: Run / Selection Status

Show:

- active refined run name
- whether current selection is `raw` or `refined`
- selected frame number
- selected detection index
- selected ROI index
- whether the current selection is editable

If no valid selection exists, say that directly and explain how to get one:

- select a detection with refined keypoints
- use the current frame review controls

### Section 2: Current Row State

Show a compact summary of the current row:

- quality label
- reason tag summary
- review status
- usable / geometry-valid / confidence-valid
- whether the row was manually corrected

This should be read-only status, not a dense dump of all arrays.

### Section 3: Edit Actions

Primary action group:

- `Save Keypoint Edit`
- `Mark No Keypoints`
- `Mark Detection Issue`
- `Reset Unsaved Edit`

These actions should call the current refined-keypoint repository seam and show
clear success/failure status.

### Section 4: Review Actions

Separate group:

- `Mark Reviewed`
- `Mark Needs Follow-up`
- `Clear Review Status`

The exact button names should match the contract vocabulary already used by
Palette and Crimson's review metadata.

### Section 5: Navigation

This is where the old `Labeling Tool` UX is worth borrowing most directly.

Recommended controls:

- `Jump to Next Unreviewed`
- `Jump to Next Failure`
- `Jump to Next Detection Issue`
- `Jump to Next No Keypoints`

Show counts beside or below these controls:

- total refined rows in current run
- current review queue size
- current frame's refined detections

This replaces the old:

- `Next labeled frame`
- `Total labeled frames`

with Zarr-native review queue concepts.

### Section 6: Operation Status

Show one compact status area for:

- last manual edit result
- last review write result
- reload failures
- no-op saves
- stale eye-mask invalidation summary

This should become the main "what happened?" feedback channel for the workflow.

## Proposed Architecture

### New Window Module

Add a dedicated wrapper:

- `src/gui/refined_keypoint_review_window.h`
- `src/gui/refined_keypoint_review_window.cpp`

This module should own:

- the window shell
- section ordering
- action button layout
- queue/status presentation
- returned workflow requests

It should not own:

- Zarr write logic
- crop-image rendering
- drag-handle editing behavior

### Recommended Context

Introduce a narrow context struct, for example:

- `RefinedKeypointReviewWindowContext`

Suggested contents:

- selected refined keypoint selection
- current frame number
- current review counts / queue info
- current row summary
- current manual-write status text
- whether playback is active

Dependencies should be passed explicitly, not through a mega app context.

### Recommended State

Introduce a feature-local state struct, for example:

- `RefinedKeypointReviewWindowState`

Suggested state:

- sticky filter mode
- last navigation target kind
- whether detailed row status is expanded
- transient local button state if needed

### Action Result Type

Return a narrow result object, for example:

- requested manual edit action
- requested review-status action
- requested navigation target kind

`red.cpp` should apply the result, not host the full UI logic inline.

## Relationship To Existing Modules

### Keep

- `RefinedKeypointRepository`
- `RefinedKeypointReviewPanel`
- `CropKeypointEditor`

### Refactor Direction

Use the new window as a composition shell:

- `RefinedKeypointReviewWindow`
  - may reuse pieces from `RefinedKeypointReviewPanel`
  - may trigger actions already handled by the current crop editor path
  - may eventually absorb the current keypoint-specific `Frame Debug` panel

### Likely Outcome

After this lands:

- `Frame Debug` should carry less keypoint-review-specific UI
- `Crop Preview` should remain the edit surface
- `Labeling Tool` can remain as legacy/manual-CSV tooling, but should no longer
  be the recommended path for refined-keypoint work

## Phased Plan

### Phase 1: Window Shell

Goal:

- create a dedicated `Refined Keypoint Review` window
- show run/selection summary and current status
- no new behavior yet

Implementation:

- add new window module
- feed it existing refined-keypoint state already shown in `Frame Debug`
- keep all current action handling where it already exists

Success criteria:

- operator can discover the refined workflow from one obvious window
- no data model changes required

### Phase 2: Move Review Controls Out Of Frame Debug

Goal:

- move keypoint review buttons and summary out of `Frame Debug`

Implementation:

- migrate refined-keypoint review controls into the new window
- keep Crop Preview editing actions functional

Success criteria:

- `Frame Debug` stops being the primary keypoint-review surface

### Phase 3: Add Queue Navigation

Goal:

- replace the old "next labeled frame" concept with refined-review navigation

Implementation:

- add helpers for:
  - next unreviewed row
  - next failure row
  - next `detection_issue`
  - next `fish_present_no_keypoints`

Success criteria:

- a reviewer can progress through the run from the new window alone

### Phase 4: Integrate With Crop Preview More Cleanly

Goal:

- make the workflow feel unified between review window and crop editor

Implementation:

- shared status text
- shared selected ROI/detection summary
- clear prompt when Crop Preview is required for precise editing

Success criteria:

- the window drives the task
- Crop Preview performs the detailed edit

### Phase 5: Retire Keypoint Review From Legacy Surfaces

Goal:

- reduce confusion between old and new keypoint workflows

Implementation:

- trim keypoint-review-specific controls from `Frame Debug`
- keep `Labeling Tool` clearly marked as legacy/manual CSV workflow

Success criteria:

- a new operator can identify the Zarr-native path immediately

## Immediate First PR

Recommended first PR boundary:

1. add `refined_keypoint_review_window.*`
2. move the current refined-keypoint status summary into it
3. move the current review-status buttons into it
4. keep manual edit drag/save in Crop Preview
5. leave navigation for the next PR

This keeps the first slice mostly UI-shell extraction rather than mixing UI
reorganization with new queue semantics.

## Non-Goals

This plan does not propose:

- removing the legacy CSV labeling tool immediately
- rewriting Crop Preview
- changing the refined-keypoint write contract
- moving refined-keypoint data back into `FrameDetections`
- merging manual edit save and review acceptance into one action

## Success Criteria

The plan is successful when:

- the Zarr-native keypoint workflow has one obvious entry window
- refined-keypoint review no longer feels hidden inside `Frame Debug`
- Crop Preview remains the precision edit surface
- the old `Labeling Tool` is no longer mistaken for the canonical Zarr path
- `red.cpp` shrinks because another workflow shell has moved out into a module
