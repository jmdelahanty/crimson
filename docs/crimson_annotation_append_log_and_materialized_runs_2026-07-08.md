# Crimson Annotation Append Log And Materialized Runs

Decision timestamp: 2026-07-08

## Problem

User edits can change annotation cardinality.

Moving an existing bounding box, keypoint, or mask component can often be
represented as an update to an existing row. Adding or deleting a bounding box
is different: it changes which annotation rows exist for a frame. Downstream
artifacts such as keypoints, crop ROIs, subject masks, contours, and tracks may
need to be created, deleted, or marked stale.

Zarr arrays are chunked and fixed-shape enough that inserting a row in the
middle of an existing sorted run is the wrong primitive. A middle insertion can
shift later rows, invalidate stable row ids, and force broad rewrites across
many chunks. It also makes provenance harder to reason about.

## Decision

Use two storage modes with different jobs:

1. An append-only correction log for live user edits.
2. Materialized frame-sorted runs for playback, training, and downstream
   workers.

The correction log is write-optimized and row-id stable. The materialized run is
read-optimized and frame-local.

Core invariant:

```text
append log = live edit/write surface
materialized run = durable playback/training/worker surface
```

The append log should not become the permanent layout that every reader has to
replay forever. It is a staging and audit surface. Once corrections are accepted
and useful, Palette should compact them into a new materialized run so playback,
training, and background workers regain frame-local reads.

## Write Path: Append-Only Correction Log

When a user adds, moves, deletes, or changes an annotation in Crimson, Palette
should persist that action as an append-only correction record rather than
splicing rows into the middle of the active run.

Example correction record fields:

```text
event_id
frame_index
operation            # add | update | delete
artifact_type        # bbox | keypoint | subject_mask | ...
target_run
target_row_id        # optional for add, required for update/delete
component_name       # optional, for component masks
payload              # bbox, keypoint coordinates, dense mask path/ref, etc.
reviewer
reason
created_at_utc
status               # pending | accepted | rejected | superseded
```

The log should also expose a frame lookup surface so Crimson can find relevant
edits for the displayed frame without scanning the appended tail:

```text
frame_index -> correction event row ids
```

The exact Zarr representation can be arrays, a table-like group, or a Palette
service-backed queue, but the read contract should make frame lookup explicit.

## Read Path During Live Review

Crimson should display the active materialized run plus relevant correction
records for the current frame:

```text
display frame F =
  materialized rows for frame F
  + accepted/pending correction records for frame F
```

Crimson should not assume appended correction rows are near the original frame's
materialized rows. The correction log is nonlocal in row space by design. Frame
lookup is what makes it usable for live display.

For local responsiveness, Crimson can keep the current session's edits in memory
and merge them into the displayed frame immediately, while Palette persists the
same edits through the correction log.

## Materialization Path

Palette should periodically, explicitly, or asynchronously materialize accepted
corrections into a new frame-sorted run.

Materialization should:

1. start from a chosen source run;
2. apply accepted correction events in deterministic order;
3. preserve stable identities where possible;
4. assign new row ids for added annotations;
5. omit or mark deleted annotations according to the target contract;
6. sort/group rows by `frame_index`;
7. recompute frame lookup/count arrays;
8. write a new run with provenance pointing to the source run and correction
   log.

After materialization, playback and training should prefer the new materialized
run rather than replaying an unbounded correction log forever.

## Background Workers

Palette workers can consume accepted correction events and materialized runs to
produce downstream artifacts asynchronously.

For example:

```text
manual bbox accepted
-> append correction event
-> Crimson displays materialized run + correction event
-> Palette worker creates/updates frame-sorted refined detect run
-> keypoint worker derives keypoints for new/changed rows
-> mask worker derives dense subject masks for new/changed rows
-> contour/metric/cache workers regenerate derived in-Zarr arrays
```

Workers should write new derived runs or mark existing derived arrays stale
rather than silently mutating stale outputs into apparent agreement.

## Why Not Append Directly To The Active Run Forever?

Appending directly to the end of the active artifact arrays is cheap to write
but poor for streaming reads.

If frame `5000` originally has detection rows near row `12000`, and a new bbox
for frame `5000` is appended at row `120000`, then displaying frame `5000` may
require reads from two distant chunk regions:

```text
base rows near row 12000
manual appended rows near row 120000
```

That defeats frame-local streaming and creates sudden chunk jumps during
playback. It also makes downstream arrays harder to keep aligned.

The correction log accepts this nonlocality because it is a short-term live-edit
write surface. The materialized run restores frame-local row ordering for
review, training, and worker input.

In this sense, materialization is also compaction. It turns a sequence of
possibly scattered correction events into one coherent, frame-sorted run that
ordinary readers can consume without special random jumps through the edit log.

## Why Not Insert Rows Into The Middle?

Middle insertion is the worst of both worlds:

- it can require rewriting many chunks after the insertion point;
- it shifts row ids unless an additional indirection layer is added;
- it invalidates downstream row references;
- it complicates provenance and review audit trails;
- it makes concurrent edits harder to merge.

Stable rows within a run are more important than preserving one mutable run name.
When row cardinality changes, prefer a correction log and then a new
materialized run.

## Training Promotion

Training Zarrs should be built from reviewed materialized runs, not directly
from an unbounded live correction log.

For subject masks, training promotion should consume dense `masks_roi` only.
For detections/keypoints, training promotion should consume the materialized
frame-sorted arrays that already include accepted corrections.

This keeps training datasets deterministic, frame-local, and reproducible.

## Crimson Implications

Crimson should:

- display materialized rows plus frame-local correction records;
- keep in-session edits responsive in memory;
- not insert rows into active run arrays itself;
- ask Palette to persist edits through an append/correction API;
- reload or switch to a newly materialized run when Palette publishes one;
- clearly show whether a displayed annotation is materialized, pending,
  accepted, or rejected.

Palette should:

- own correction-log writes and validation;
- expose frame lookup for corrections;
- materialize accepted corrections into new frame-sorted runs;
- mark or regenerate downstream keypoints, masks, contours, metrics, and compact
  caches after correction events;
- promote training datasets only from reviewed materialized runs.

## Open Questions

1. Should the correction log live inside the analysis Zarr, in a registry-backed
   service, or both?
2. How often should Palette materialize accepted corrections: immediately after
   each save, on user request, or in scheduled background batches?
3. Should pending correction events be visible by default in Crimson playback,
   or only accepted events?
4. What identity scheme should be used for added annotations before they are
   assigned materialized run row ids?
