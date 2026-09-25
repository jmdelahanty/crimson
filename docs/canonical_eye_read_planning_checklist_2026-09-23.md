# Canonical eye read planning

Baseline: `fe0c9f4`. This follow-on changes read planning, not stored measurements
or the validated selection. Linux remains the tested backend; legacy full-read
interfaces remain available to macOS and older recordings.

## Implementation gates

- [x] Measure per-array logical calls/bytes/failures and future elapsed time;
  keep repository opening separate from per-frame payload work. These are not
  physical NFS-byte or decode-only measurements.
- [x] Share one immutable, validated mask frame-offset index across mask,
  contour, shape, and eye readers in one archive/selection epoch. Retain each
  consumer's own row-limit and identity checks. Do not share unlike hash schemes.
- [x] Derive eye fields from visible camera features and the active inspector
  representation, union same-frame demands, and retain distinct-frame consumers.
- [x] Distinguish loaded coverage from scientific validity. Cache upgrades must
  not satisfy a richer request with partial data, and unloaded gaze must not
  trigger the invalid-gaze rendering fallback.
- [x] Batch independent reads with bounded in-flight work, preserving the
  identity-validation barrier before geometry/scalar payloads. Check cancellation
  between waves and retain/drain work across canceled generations.
- [x] Preserve ellipse conventions, ROI/camera coordinate transforms, complete
  body-frame validity (including the origin), exact instance keys, and stale
  frame/source rejection.

## Verification

- [x] Reader fixtures: feature subsets, upgrades/downgrades, zero disabled reads,
  identity mismatch, invalid scientific values, cancellation, bounds, metrics.
- [x] Shared-index fixtures: exact binding/epoch, real schema-v5 offset reads,
  digest/corrupt-payload rejection, shared handles and independent row limits.
- [x] Buffer/session tests: camera plus inspector (same/different frames),
  toggles during reads, partial hits, seek cancellation, legacy full-read callers.
- [x] Scene/inspector tests: partial coverage never becomes a scientific failure
  or a fabricated fallback; full-demand scene remains equivalent.
- [x] Approved Ubuntu builder: targeted tests, full suite, `redgui`.
- [x] Actual August archive: axes-only versus full eye demand, repeated and
  distant reads with per-array metrics, and authenticated GUI playback smoke.

Deferred: lazy repository opening, a broad cross-product mapping-page cache,
archive rechunking, and speculative-lookahead tuning. Keep the existing bounded
lookahead until measurements justify a separate change.

## Evidence

- Builder: `/tmp/crimson-eye-readplan-final2-build.log`, followed by the final
  UI-mode wiring rebuild `/tmp/crimson-eye-readplan-mode-build.log`.
- Full regression suite: **119/119 passed** in
  `/tmp/crimson-eye-readplan-final-tests.log`, including the final deterministic
  blocked-read field-upgrade and disable tests. Their incremental build is
  `/tmp/crimson-eye-readplan-inflight-build.log`.
- Required authenticated playback smoke: frames 0–300 passed, log
  `/tmp/crimson-eye-readplan-required-playback.log`.
- Actual August full-eye captures passed at frame 0
  (`/tmp/crimson-canonical-overlay-smoke.6A1LLE`) and frame 54,000
  (`/tmp/crimson-canonical-overlay-smoke.hbzSOV`): matched source/instance keys,
  3 angle labels, 2 cones and 1 overlap. Frame-0 screenshot inspected. Eyes-off
  capture (`/tmp/crimson-canonical-overlay-smoke.yAWCnO`) verified zero eye
  payload reads. These isolated captures exercise the shared mask/shape/eye
  consumers together; there is not yet one local fixture asserting every
  consumer's borrowed-offset counter in a single session.
- Final field probes: `/tmp/crimson-eye-readplan-{axes,full}-probe.json`.
  Frames 0, 1, 0, 8191, 8192, 54000, 600000, 600000. Both plans reused the
  common index (eye-owned offset reads/retained bytes both zero); peak wave was
  four futures. Uncached one-observation frames used **17 calls/136 logical
  bytes** for both-eye axes versus **28 calls/197 logical bytes** for full eyes.
  Repeat frame 0 and 600000 issued no payload reads. Axes omitted angles, gaze
  and all body-frame arrays, retaining identity/crop/QA validation. Full frame-0
  angles matched baseline: left 29.482826°, right 31.607246°, vergence 61.090073°.
- Final UI-mode build playback pair:
  `/tmp/crimson-eye-readplan-playback.J8xmV1/{off,on}.{log,jsonl}`. Frames
  8180–8480, source 30 FPS, UI cap 60 Hz, 10-second paused warmup, full masks/
  contours/shapes and bout shading. Both passed; each yielded 629 measured
  overlay events and 628 performance-loop samples. Eye-on had zero pending,
  incomplete, stale, label-mismatch or cone-mismatch events. Both had zero
  missing-mask draw events; eye-off had zero eye payload reads.

| UI measurement | Eyes off | Eyes on |
| --- | ---: | ---: |
| Active loop median | 2.84 ms | 2.65 ms |
| Active loop p95 | 4.43 ms | 4.54 ms |
| Active loop maximum | 6.07 ms | 6.08 ms |
| Capped loop median | 16.84 ms | 16.84 ms |

Active loop means measured loop minus cap sleep, not a GPU timer. This single
fresh-process pair is a calibration/smoke check, not full NFS performance
qualification; OS/server caches were uncontrolled and not flushed. Probe
dispatch-to-consumption durations include batching/queueing and cannot be summed
as isolated disk service. Per-chunk admission plus four futures does not bound
all transient decoded memory. The earlier ~4.9-second distant-read outlier remains
unattributed; these later warm/repeated reads do not explain it retroactively.

Richer field upgrades currently read the normalized union again; this is not yet
a missing-fields-only fetch/merge optimization. Failed upgrades preserve an
already mapped subset without claiming the failed fields were loaded. A private
attempted-field signature suppresses repeated failure reads until demand grows
or a seek/reset occurs. Legacy full-read methods remain intact; macOS and Windows
runtime testing and repackaging are not part of this Linux verification.
