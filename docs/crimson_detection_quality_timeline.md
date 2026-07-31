# Crimson Detection Quality Timeline

Date: 2026-07-30

Status: detection-first read-only checkpoint implemented; editing remains a
separate follow-up and keypoint quality is documented independently

## User Surface

The existing Frame Inspect `Detect` tab now identifies the selected canonical
or refined run and lists every observation in the currently presented camera
frame. Each row reports confidence, class, and raw/manual/edit provenance. The
row selector preserves `instance_key`; it does not treat row order as subject
or track identity.

`Detection Timeline` opens a separate dockable timeline window. It presents:

- raw source confidence as minimum, median, and maximum per camera frame;
- accepted confidence separately for refined snapshots;
- source, accepted, filtered, duplicate/manual-clear, and manual-row counts;
- the digest-bound refined reason-code registry; and
- click-to-seek using the existing playback clock and discontinuity path.

Confidence is the model-reported score, not accuracy or ground truth. Numeric
refinement thresholds are not inferred from external paths and remain absent
until a validated provenance contract declares them.

## Read Contract

The repository and buffer are backend-neutral. ImGui and Metal only consume a
published timeline window.

- Canonical v3 opens exact `float32 scores` and `int64 frame_row_offsets`.
- Refined v1 opens exact source `scores`, `decision_codes`, `reason_codes`, and
  independent source/instance offset indexes. It reads instance
  `source_kind_codes` to distinguish manual rows.
- Each offset vector is read exactly once when the user opens the timeline.
- Refined source-audit arrays remain unopened during ordinary overlay playback.
- Visible data uses 8,192-frame pages with a 4,096-frame step and a three-page
  cache. Field reads for refined pages are issued concurrently.
- Scheduler generations cancel superseded seeks, and stale page results cannot
  publish.
- Plot vectors are prepared once per published page rather than rebuilt every
  GUI frame.

This path is read-only. It does not route selection to an ROI inset, write
review decisions, infer a longitudinal subject identity, or silently fall back
from an invalid explicit refined run.

## Full-Duration Gate

Both mounted Sleepyfish candidates contain 1,188,000 camera frames. The final
headless gate passed against the explicit canonical-v3 and refined-v1 runs:

| Surface | Rows | Offset reads | Retained offsets | Observed open | Observed 8,192-frame window | Decoded window bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Canonical | 1,186,376 | 1 | 9,504,008 B | 518.1-773.0 ms | 112.5-151.5 ms | 32,768 B |
| Refined | 1,186,376 source / 1,169,010 accepted | 2 | 19,008,016 B | 785.9-1,037.7 ms | 111.2-137.4 ms | 65,536 B |

These are mounted-network observations, not universal latency guarantees. The
refined gate reached four concurrent field reads. Synthetic tests cover empty
and multi-observation frames, score and reason aggregation, malformed offsets,
out-of-range requests, and cancellation of a blocked superseded page.

## Remaining Work

- Route the selected observation to the ROI inset once the multi-observation
  presentation contract is shared across detection and keypoint surfaces.
- Keep the corresponding keypoint-quality surface independent so detection and
  keypoint observation identities are not conflated. Its implemented contract
  is documented in `docs/crimson_keypoint_quality_timeline.md`.
- Display friendly model names and numeric refinement cutoffs only after they
  are part of a validated, digest-bound provenance envelope.
- Validate equivalent presentation in the maintained Linux UI and on native
  Windows. The repository and buffer have no rendering-backend dependency.
