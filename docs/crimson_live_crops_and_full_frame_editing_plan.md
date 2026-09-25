# Crimson Live Crops And Full-Frame Editing Plan

Date anchored: 2026-04-06.

## Purpose

Document the current crop-preview runtime behavior in Crimson, explain why the
runtime should move away from persisted crop images, and define why full-frame
editing should become the first-class interaction model for keypoints, eye
masks, subject masks, and related overlay/edit types.

This plan is about Crimson runtime architecture and UI behavior. It does not
argue that Palette should never materialize crops for training or export
artifacts. It argues that Crimson should not depend on persisted crop images as
its primary editing model.

## Core Conclusion

Crimson should treat the full camera frame as the canonical visual source for
interactive review and editing.

Crop Preview should remain an important tool, but it should become a derived
view produced from:

- the current full-frame image,
- detection or geometry metadata,
- and one explicit crop specification.

This fits better with first-class full-frame editing because the operator can
reason about all objects in one shared image space, while still getting precise
zoomed editing where needed.

## Current State

### Crop Preview Uses Persisted Zarr Crop Images

Crimson's current Crop Preview path does not crop directly from the full-size
video frame at render time.

Instead:

- `zarr_loader_movement.cpp` loads `crop_runs/<run>/roi_images`,
- the loader copies that crop dataset into memory up front,
- `zarr_loader_detections.cpp` returns per-ROI crop views from that in-memory
  buffer, and
- `red.cpp` uploads the selected crop into the crop-preview texture.

The rotated crop preview is then derived locally from that persisted crop
image. It is not itself a canonical stored artifact.

So the current runtime model is:

1. persisted crop image in Zarr,
2. eager in-memory crop cache in Crimson,
3. UI preview from that cached crop,
4. derived rotated preview on top.

### What This Means

The current runtime path duplicates information that already exists elsewhere:

- the full frame exists,
- the detection or geometry placement exists,
- the crop image is a view derived from those two things plus a crop policy.

This also means that Crop Preview currently encourages a crop-first mental model
even when the operator really wants to reason about full-frame context.

## Problems With Keeping Persisted Crops As The Primary Runtime Model

### 1. It Makes The Runtime Depend On A Derived Artifact

Persisted crops are not the primary scientific object. They are a convenient
materialization for training and review, but they are still derived from:

- frame image data,
- ROI placement,
- crop dimensions,
- padding policy,
- interpolation policy,
- and optional orientation rules.

If Crimson depends on the derived artifact for editing, it becomes harder to
keep the runtime aligned with the real full-frame geometry.

### 2. It Encourages Crop-First Editing Instead Of Frame-First Editing

A crop-only workflow is acceptable for some focused tasks, but it is not the
right first-class model for the application as a whole.

Operators often need to:

- compare multiple detections in the same frame,
- understand occlusion and neighboring subjects,
- assess whether a detection is even the right target,
- edit masks or points while seeing the surrounding image,
- switch between detailed local editing and global frame context.

That is naturally a full-frame workflow with optional zoomed derivations, not a
crop-first workflow.

### 3. It Duplicates Runtime Storage And Memory Cost

At runtime, persisted crops add:

- Zarr datasets that must exist for the UI path to work,
- eager in-memory loading of crop arrays,
- more state to keep aligned with geometry metadata,
- another place where stale or mismatched data can appear.

For Crimson's runtime UI, this is a poor trade if the same preview can be
derived from the current frame and the canonical ROI geometry.

### 4. It Makes Cross-Type Editing Harder To Unify

The app should move toward one editing model that works across:

- bounding boxes,
- keypoints,
- eye masks,
- subject masks,
- future ROI- or subject-scoped annotations.

That shared model is easier to design around full-image coordinates plus type-
specific local detail views than around multiple unrelated persisted crop
artifacts.

## Why Full-Frame Editing Fits Better

### Shared Image Space Is The Right Top-Level Editing Space

A first-class editor should let the operator work in the coordinate system that
the scene actually uses.

That means:

- bounding boxes remain native in full-image coordinates,
- keypoints should always have canonical image-space positions,
- masks should be placeable and inspectable in full-frame context,
- derived crop views should simply zoom into the current full-frame selection.

This makes different data types compose naturally in the same frame:

- detections,
- keypoints,
- heading overlays,
- eye masks,
- subject masks,
- future per-subject or per-component overlays.

### Crop Preview Still Matters, But As A Derived Precision Tool

This plan does not remove Crop Preview.

Crop Preview is still valuable for:

- precise point dragging,
- dense mask inspection,
- oriented subject views,
- high-magnification editing.

The change is architectural:

- Crop Preview should be a derived view from full-frame state,
- not the canonical runtime source that the rest of the editor depends on.

### This Is Especially Important For Masks

For masks, first-class full-frame editing matters even more.

Operators need to see:

- how the mask sits relative to the actual subject,
- whether it bleeds into neighbors,
- whether it aligns with the detection box,
- whether it is consistent with other overlays in the same frame.

That is difficult to judge from an isolated crop alone.

The right long-term model is:

- full-frame display as the primary editing context,
- crop/zoom views as precision assistants,
- canonical writes still mapped back to the correct run-local storage.

This applies to:

- refined eye masks,
- refined subject masks,
- future component masks,
- and any similar ROI-local bitmap outputs.

## Important Clarification About Canonical Storage

First-class full-frame editing does not mean every data type must be stored as a
full-frame raster.

For example:

- keypoints can still be canonically stored as image-space coordinates,
- eye masks can still be canonically stored as ROI-local mask planes,
- subject masks can still be canonically stored in their current contract shape,
- bbox edits can still write back to detection-aligned ROI records.

The requirement is not "store everything as a full-frame artifact."

The requirement is:

- edit from full-frame context,
- maintain one canonical write contract per data type,
- provide reversible mapping between full-frame interactions and canonical row
  or component storage.

## Runtime Architecture Recommendation

### 1. Make The Full Frame The Canonical Display Source

The main camera image should be the authoritative visual substrate for review
and editing.

All interactive overlays should be expressed relative to that frame first.

### 2. Introduce A Shared Live Crop Service

Crimson should have one crop service that derives preview images from:

- the current full-frame image,
- ROI or geometry metadata,
- and one explicit crop specification.

This service should feed:

- Crop Preview,
- rotated preview variants,
- optional thumbnail or side-panel previews.

### 3. Keep Type-Specific Editors Above A Shared Frame Model

Each editor can then specialize on top of the same frame-level selection:

- bbox editor
- refined keypoint editor
- eye-mask editor
- subject-mask editor

All of them should share:

- current frame identity,
- current selected detection or subject identity,
- full-image coordinate mapping,
- live crop derivation when zoomed editing is requested.

### 4. Avoid Requiring `crop_runs/<run>/roi_images` For Runtime Editing

Persisted crop datasets should become optional for Crimson runtime behavior.

They may still exist for:

- training inputs,
- offline reproducibility,
- export/debug artifacts,
- compatibility with existing batch workflows.

But the UI should not need them in order to provide editing surfaces.

## One Explicit Crop Specification Is Required

If Crimson moves to live crops, it must not do so with implicit or hand-wavy
crop rules.

One explicit crop specification should define:

- source image space,
- ROI origin source,
- crop width and height policy,
- square vs rectangular behavior,
- padding or scale margin,
- out-of-bounds fill behavior,
- interpolation mode,
- rotation convention for oriented previews.

Without this, full-frame and crop-preview behavior will drift apart and produce
subtle mismatches between review, refinement, and training.

## Proposed Migration Path

### Phase 1: Introduce A Crop Abstraction

- add a runtime crop service interface,
- keep the current persisted-crop backend temporarily,
- stop letting UI code depend directly on raw crop-array ownership.

### Phase 2: Add A Live-Crop Backend

- derive crop previews directly from the current full frame,
- validate that crop geometry matches the existing persisted-crop behavior,
- keep rotated previews derived locally.

### Phase 3: Make Full-Frame Editing First-Class

- ensure edit tools can operate from the main frame view,
- keep Crop Preview as a zoomed precision surface,
- make mask editing work from both main frame and crop view.

### Phase 4: Remove Runtime Dependence On Persisted Crops

- stop requiring `roi_images` for editor workflows,
- keep persisted crops only where they are still justified as optional pipeline
  artifacts,
- simplify loader/runtime state accordingly.

## Relationship To Other Plans

- The texture-backed eye-mask plan remains valid, but it fits even better when
  the main frame is the primary editing space:
  [crimson_eye_mask_texture_and_editing_plan.md](./crimson_eye_mask_texture_and_editing_plan.md)
- The refined-keypoint editor and review window plans should treat Crop Preview
  as a derived precision surface rather than the canonical workflow anchor:
  [crimson_zarr_keypoint_editor_plan.md](./crimson_zarr_keypoint_editor_plan.md)
  and
  [crimson_zarr_keypoint_review_window_plan.md](./crimson_zarr_keypoint_review_window_plan.md)

## Bottom Line

Persisted crops are useful artifacts, but they are the wrong primary runtime
abstraction for Crimson.

If the application wants first-class editing across keypoints, eye masks,
subject masks, and related overlay types, it should:

- treat the full frame as the canonical editing context,
- derive crop views live from geometry,
- and keep canonical writes mapped back into each data type's proper storage
  contract.
