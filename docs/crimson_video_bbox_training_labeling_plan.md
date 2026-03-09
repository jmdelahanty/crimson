# Crimson Video BBox Training Labeling Plan

Date anchored: 2026-03-05.

## Purpose

Document what Crimson currently supports for bounding-box editing, what we confirmed during investigation, and what is required to support a true "label training data from video" workflow for detector training.

## Archive Roles (Decision)

- `_training.zarr` is the canonical training-label archive for direct dataset authoring.
- `_analysis.zarr` remains production/inference/review oriented.
- Manually cleaned detections from `_analysis.zarr` can be promoted into merged training archives as an ingestion source.
- Promotion is row-level only; an `_analysis.zarr` recording is never converted wholesale into a training archive.

## What We Confirmed

1. Crimson already supports in-UI bbox editing actions on loaded Zarr detections:
- select/move box
- draw new box
- delete selected box
- per-frame in-memory dirty state

2. Raw detect is intentionally read-only in editor UX:
- `detect_runs/<run>` is not mutated in-place.

3. Persisting bbox edits is implemented, but it writes to refined detect manual subgroup only:
- UI action: `Write Manual Payload to Zarr`
- write target: `refined_detect_runs/<latest>/manual` (or selected manual group)
- pointer/status updates: `manual_review_latest`, `detect_review_status`, `detect_review_status_latest`

4. Current save path is review/edit oriented, not a dedicated training annotation product:
- It assumes a refined detect run already exists.
- It is optimized for "manual refinement of model detections" rather than "annotate arbitrary video from scratch for training."

## Current Capability vs Training-Labeling Need

Current capability (today):
- Manual correction layer on top of existing detections in analysis Zarr.
- Contract-aligned writeback for refined-detect review workflows.

Training-labeling need:
- Start from video frames (with or without preexisting detections).
- Create explicit ground-truth annotations for detector training.
- Maintain class ontology and split metadata.
- Export directly to training formats (YOLO/COCO), with reproducible provenance.

Conclusion:
- Crimson is close on annotation interaction primitives, but missing dedicated data model + workflow boundaries for training-label authoring.

## Gaps To Close

1. Data model gap
- No dedicated "training bbox label run" namespace.
- Existing refined manual subgroup conflates review edits and training truth.

2. Bootstrap gap
- Cannot cleanly start a training labeling session from a video without existing refined detect context.

3. Class workflow gap
- No explicit class-management UX for human labeling (class picker/hotkeys/validation).

4. Dataset-management gap
- No explicit train/val/test split assignment per frame/sequence.
- No "label completeness" state machine for dataset curation.

5. Export gap
- No first-class Crimson action to export validated labels to YOLO/COCO manifests.

6. QA/audit gap
- No built-in stats panel for class balance, box size distribution, unlabeled-frame coverage, and duplicate/conflict checks.

## Recommended Architecture

### Recommendation: Add Dedicated Training Label Runs

In `_training.zarr`, add:
- `training_bbox_label_runs/<run_name>/...`

Store:
- `frame_indices` (`int32`, detection-row aligned)
- `bbox_norm_coords` (`float64`, `[cx, cy, w, h]`)
- `class_ids` (`int32`)
- optional: `scores` (`float32`, default `1.0` for human labels)
- per-frame counts:
  - `frame_counts`
  - `n_detections` (alias)
- optional provenance:
  - `reason` (`manual`, `auto_seeded`, `verified`, etc.)
  - `annotator_id`
  - `annotated_timestamp`
  - `source_video_path`
  - `source_frame_rate`
  - `label_schema_version`
  - `source_archive_role` (`training` or `analysis_promoted`)
  - `source_archive_path`
  - `source_run_ref` (e.g. refined run + group when promoted from analysis)

Run attrs:
- `intended_use = "training"`
- `label_status` (`in_progress`, `ready_for_export`, `locked`)
- `class_map` / ontology reference
- `latest` pointer at `training_bbox_label_runs` group level for deterministic run resolution

Rationale:
- Keeps review refinement data separate from training truth.
- Avoids accidental coupling to refined detect pointer semantics.
- Makes export and audit logic straightforward.

### Compatibility Path

Keep existing refined manual write path unchanged for review workflows.

Add an optional "promote to training labels" bridge:
- copy/transform from `refined_detect_runs/<run>/<manual_group>` into `training_bbox_label_runs/<run_name>`
- preserve provenance links back to refined run.
- selection unit is annotation rows (frame-indexed detections), not entire source archives.

### Split Assignment Policy (Decision)

- Do **not** require split assignment in source archives.
- Assign splits during merge/build of a merged training archive.
- Persist split assignment in merged training output (for reproducible exports), not as an editing prerequisite in source runs.

## UX/Workflow Plan

1. Add explicit mode switch:
- `Review Edit Mode` (existing refined manual flow)
- `Training Label Mode` (new dedicated flow)

2. Training Label Mode behavior:
- open video/zarr
- choose or create training label run
- label with draw/move/delete (resize can be a later phase)
- assign class via hotkeys/panel
- mark frame status (`unlabeled`, `partial`, `complete`, `skip`)

3. Save behavior:
- write directly to `training_bbox_label_runs/<run_name>`
- no reliance on refined detect pointers
- autosave and manual save

4. Export behavior:
- export filtered by run/split/status
- output YOLO and/or COCO
- include reproducibility metadata (class map, source, timestamp, commit hash)

## Minimum Viable Implementation

Phase 1: Data + persistence
1. Add loader/writer support for `training_bbox_label_runs`.
2. Add run create/select UI.
3. Add save/load for training label rows and frame counts.

Phase 2: Annotation UX
1. Class picker + hotkeys.
2. Frame status controls and next-unlabeled navigation.
3. Basic per-run counts (frames labeled, boxes, per-class counts).

Phase 3: Export + validation
1. Export command from Crimson (or companion CLI) to YOLO/COCO.
2. Validation checks:
  - box bounds
  - non-empty class IDs
  - split consistency
3. Run-level lock/freeze before export.

Phase 4: Merge/Ingest from analysis archives
1. Add ingest tool that reads approved/manual cleaned detections from `_analysis.zarr`.
2. Promote selected rows into merged training archives with explicit provenance fields.
3. Deduplicate/override policy by `(video_id, frame_index, track_or_box_identity)` with deterministic precedence.

## Implementation Touchpoints (Crimson)

Likely files/modules:
- `src/red.cpp`
  - mode switch, training-label panel, class/status controls
- `src/zarr_loader.h`
  - training label run structs + API
- `src/zarr_loader.cpp`
  - training label run read path
- `src/zarr_loader_write.cpp`
  - training label write path
- `tools/`
  - export validator/converter helpers
- `docs/`
  - training label write/read contracts

## Guardrails

1. Never mutate raw detect arrays in-place.
2. Keep review edits and training truth in separate namespaces.
3. Enforce coordinate and shape invariants on write:
- normalized boxes in `[0,1]` with finite values
- array length consistency across row-aligned fields
- per-frame counts sum to detection-row count
4. Record provenance for every run.
5. Treat merge-time split assignment as immutable once exported.

## Suggested Next Docs

1. `docs/crimson_training_bbox_manual_write_contract.md`
2. `docs/crimson_training_bbox_read_contract.md`
3. `docs/crimson_training_bbox_export_contract.md`

## Bottom Line

Crimson already has most of the interactive bbox-edit UX needed for labeling. The main missing piece is a dedicated training-annotation model centered on `_training.zarr`, plus a promotion/merge path that can ingest cleaned detections from `_analysis.zarr` with provenance and deterministic split assignment.
