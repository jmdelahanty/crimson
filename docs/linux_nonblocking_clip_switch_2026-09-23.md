# Linux nonblocking clip switches — September 23, 2026

Historical report of the nonblocking-switch stage. The subsequent
[prewarm follow-up](linux_clip_prewarm_2026-09-23.md) adds next-clip decoding and a
visible-frame hold gate; the limitations and measurements below describe this
earlier implementation, not the later prewarm results.

This change removes the measured long UI-thread waits during NVIDIA cross-clip
seeks. It does **not** implement seamless next-clip playback, speed up complete
seeks, or fix the separately measured intermittent mask-readiness failures.
Work remains uncommitted on `codex/main-integration-20260922`; no package was
published as part of this change.

## Implementation

`MediaSessionLoader` prepares the target demuxer/keyframe information in a worker,
then drains the old decoder in another worker. The render loop polls those stages
through `WaitingMedia`, keeps the previous texture and parent-frame identity,
and commits the replacement on the owner thread. Scene, buffer, GL/CUDA and
calibration activation remain owner-owned. Calibration/path resolution and GPU
activation have not all been moved off that thread; measured owner commits were
roughly 9–14 ms, not a hard upper bound.

Same-clip seeks retain their existing path. Superseding seek targets are resolved
before committing prepared media. The boundary coordinator reports loaded only
after commit, and presented only after the new frame is actually displayed.
Session-open barriers cancel pending seeks, drain transition workers while the
loading modal pumps, and restore the old decoder when needed. A rejected loading
worker launch still requires a synchronous drain before mutating the session.
Shutdown drains workers before resource destruction. Decoder stop signaling is
atomic; workers do not own or mutate the current archive/session selection.

## Measured responsiveness and remaining video hold

Evidence root: `/tmp/crimson-async-clips-20260923.O8mm7d`.
Baseline evidence: `/tmp/crimson-august-perf-20260923.dDBOMp/repeated-baseline`.
Both are local test artifacts, not checked-in data or release packages.

Three fresh-process repetitions each of repeated paused seeks and playback across
frame 54,000 passed the existing targeted gates (`final-cross-clips/result.json`).
This subset is explicitly **not qualified** as a full-suite performance pass.

| Measurement | Before | Nonblocking implementation |
| --- | ---: | ---: |
| Repeated-seek render tick p99, median of three trials | 136.94 ms | 17.20 ms |
| Subsequent exact-seek readiness p95, median of three trials | 534.43 ms | 747.76 ms |
| Clip-boundary render tick p99, median of three trials | 17.20 ms | 17.15 ms |
| Boundary: last old frame to next valid new frame | 379–402 ms | 391–408 ms |

The new boundary traces repeat frame 53,999 for 23–24 draws before frame 54,000.
Ordinary 30 FPS frame holds were about 33–34 ms. The baseline traces instead
include seven negative-presented-frame draws before frame 54,001; they do not
establish whether the old texture stayed visible or went blank. These are
comparable presentation gaps, not evidence that the pause first appeared in this
change. The user observed a stutter during live testing, consistent with this gap.

UI responsiveness and video continuity are distinct: the new path keeps rendering
at roughly 17 ms intervals while the video image stays unchanged for about 0.4 s.
It also takes longer to reach fully ready paused-seek targets in these runs.
Neither improvement in complete seek latency nor seamless playback is claimed.
One before/after same-clip control stayed at about 17.2 ms tick p99 and 408 ms
subsequent seek p95; one paired trial cannot establish general non-regression.

The current checker gates absent frame identities and skipped frames but **not
extended repetition of a valid frame**. The reporting-only
`analyze_frame_holds.py` in the evidence directory reproduces these intervals;
it is not an added passing acceptance gate. A follow-up should add an explicit
presentation-gap gate and prepare/decode the next clip before reaching the end
of the current clip. Thresholds must account for source frame rate and deliberate
pauses/seeks. No acceptance budget was relaxed to obtain these results.

## Reproducibility and validation

All binary revisions below were built from `dd41281` plus the existing uncommitted
shading/performance work and, except the baseline, this clip-switch change.

- Baseline SHA256: `e2b5ac7de424e0795fe21856712b78439a5f3875e67bef46e525bb9bccf33792`.
- Six repeated targeted trials (`final-cross-clips`):
  `2fc222182ef46483ea5d0790652e311c26aadf25bfc4fcbf3898f3edc5aca5a8`.
- Session-open seek-state hardening and test cleanup, followed by one repeated-seek
  and one boundary spot check (`accepted-cross-clips`) and one same-clip control
  (`accepted-same-clip`):
  `78628af81be9f8159a6e066a779863a106186bf57f5cd5f57925c38595a8b4c0`.
  The boundary spot check held frame 53,999 for 406.72 ms while tick p99 was
  17.18 ms; repeated-seek tick p99 was 17.19 ms, readiness p95 748.00 ms.
- Final shutdown-hardened binary:
  `2c7a11bf6b0bd99f5eec09734b281a5da942b13942ba1dca142e3ccf0ebad1cf`.
  `build-close-fix.log` records its successful build, and `ctest-close-fix.log`
  records a fresh 111/111 passing run including the repeated-shutdown regression.
  Both targeted GPU spot checks passed in `shutdown-hardened-cross-clips`:
  repeated-seek tick p99 17.19 ms and readiness p95 713.98 ms; boundary tick p99
  17.20 ms, with a remaining 407.66 ms hold of frame 53,999. This report is still
  a subset (`qualified: false`), not a complete-suite qualification.

The latter build passed 111/111 CTests on a full rerun. The first run timed out in
the unchanged `nvidia_detection_repository_tests` after its line 487 readiness
assertion. That test passed in isolation and the complete rerun passed without
changing its source or timeout. A repository publication race is suspected but
not established; the initial failure is retained in `ctest-final.log`.

Worker tests use blocked preparation/join gates, owner polling, exceptions and
close draining. Coordinator tests cover repeated pending polls without duplicate
submissions, retained frame identity, commit/presentation ordering, failures,
cancellation and superseding seeks. This is not full synthetic coverage of
`MediaSessionLoader` and its GPU lifecycle. Rapid user seek supersession and opening
a new archive during a switch have not been GUI-qualified.

A final owned-window close test (`close-pending`) exposed a shutdown double join:
the transition had already joined a decoder thread before the legacy final join.
It aborted with `std::system_error` before committing the new clip. Final shutdown
now stops and joins only live decoder handles, after draining transition workers
and before GL/GLFW teardown. A production-helper regression covers worker join,
cancel drain, and final owner drain. The identical real GPU test then passed in
`close-pending-fixed`: exit 0, clean session close, no new-clip commit. It sends
WM_DELETE_WINDOW only to the window verified to belong to its own child PID.
The final binary also passed the required authenticated June frames 0–300 GUI
playback smoke (`june-app.log`).

Measurements used the existing August recording over NFS, the local RTX A6000,
an authenticated display, isolated GUI configuration and fresh application
processes. OS/server caches were uncontrolled; no global cache flush was used.
Render tick measurements include cap sleep and tracing, not GPU timer queries.
Application file-byte counters exclude video and are not NFS wire traffic.
macOS/Windows and the colleague's RTX A4000 machine have not been tested here.
