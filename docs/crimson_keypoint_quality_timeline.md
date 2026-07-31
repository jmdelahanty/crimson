# Crimson Keypoint Quality Timeline

Date: 2026-07-30

Status: read-only keypoint-v2 checkpoint implemented; production selection and
editing remain separate work

## User Surface

The existing Frame Inspect `Keypoints` tab now lists every keypoint observation
in the presented camera frame. Each observation reports its model pose
confidence, valid-landmark count, and raw/refined state. Selecting an
observation preserves its `instance_key` and exposes per-landmark confidence,
validity, and edit state. Row order is not interpreted as subject or track
identity.

`Keypoint Quality Timeline` opens a separate dockable window. It presents:

- raw model pose confidence and independently selectable per-landmark
  confidence series;
- observation, source-success, refined-success, usable, and proposed-usable
  counts;
- catalog-declared pose and keypoint quality metrics;
- catalog-declared quality flags, refined edit counts, and review findings; and
- click-to-seek through the maintained playback-clock discontinuity path.

Confidence is the model's reported certainty, not measured accuracy or ground
truth. Raw confidence remains a source observation even when a refined surface
is selected. Refinement success, edits, review state, and reason codes are
presented as separate facts rather than rewriting that confidence.

## Read Contract

The repository, aggregation, and page buffer are backend-neutral. The macOS
ImGui/Metal layer consumes only immutable published frame windows.

- Raw keypoint v2 supplies exact `float32 keypoint_confidences`, `uint8
  keypoint_valid`, `float32 pose_confidences`, and source-success arrays.
- Refined keypoint v2 adds exact refined-success, per-landmark edit, flip,
  usable, review-state, and reason-code arrays. Source confidence still comes
  from the bound raw run.
- Keypoint quality v1 supplies exact cataloged metric, validity, flag, and
  proposed-validity arrays.
- The selected and quality `frame_row_offsets` must agree. Quality offsets are
  read exactly once when the timeline is first opened and retained for its
  lifetime.
- Current-frame overlay reads include confidence needed by the inspector, but
  ordinary playback does not open any keypoint-quality payload array.
- Timeline field ranges are issued concurrently. The default buffer uses
  4,096-frame pages, a 2,048-frame step, and a three-page cache on the shared
  application scheduler.
- Scheduler generations cancel superseded seeks, and stale page results cannot
  publish.
- Per-frame medians and counts are calculated in the repository layer. Plot
  vectors are prepared once for each published page.

Catalog declarations and refined code registries are authoritative. The
consumer rejects unregistered flag bits/codes, invalid confidence ranges,
invalid finite/NaN metric encodings, key mismatches, and malformed offsets. It
does not infer quality metrics, review semantics, or thresholds from names or
paths.

## Scope

This path is read-only. It does not write corrections, select a production
run, infer longitudinal animal identity, or merge keypoint and detection
observation identities. Legacy keypoint layouts remain behind the existing
compatibility adapter and do not silently satisfy this v2 timeline contract.

The full-duration mounted gate uses Palette's explicit raw-v2, quality-v1,
refined-v2, and body-frame-v1 Sleepyfish candidates. It verifies current-frame
confidence payloads, exactly one retained quality-offset read, a 4,096-frame
timeline window, identity agreement, lazy ordinary-playback quality access,
and concurrent field reads. Its timings are mounted-network observations, not
portable latency guarantees.

The final gate resolved 4,096 frames and rows, retained the 1,188,001-entry
offset vector in 9,504,008 bytes, and issued 20 field reads concurrently. The
observed lazy open was 200.7 ms and the first 4,096-frame window was 398.5 ms on
the current Wi-Fi/VPN-mounted path. The window decoded 430,080 logical bytes;
ordinary overlay metrics still reported zero quality-payload reads.

## Remaining Work

- Exercise the dockable window in a user-driven source-matched GUI smoke.
- Add friendly model identity and numeric thresholds only when a validated,
  digest-bound provenance envelope declares them.
- Route selected observations into future multi-subject ROI inspection without
  conflating `instance_key` with subject/track identity.
- Validate equivalent presentation in the maintained Linux UI and on native
  Windows. The repository and scheduler contracts have no rendering-backend
  dependency.
