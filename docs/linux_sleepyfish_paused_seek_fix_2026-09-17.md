# August Sleepyfish paused-seek fix — 2026-09-17

## Result and cause

Accurate paused seeking now preserves the requested frame identity on the
Linux/NVIDIA August rolling-clip route. This follows the integration committed
as `fe1fe7bc88d2d287750f837402d2a07bcbfbffba` on
`codex/linux-priority-20260916`.

The decoder correctly scanned forward from the preceding keyframe, but retained
the pre-scan frame number for settlement and sequential frame numbering. With an
external clip map, that stale counter mislabeled the decoded image and subsequent
images. The controller then accepted the wrong completion and moved the cursor.

| Reproduction | Before | After |
| --- | --- | --- |
| Paused parent 54010, clip-local 10 | PTS-local 10 labeled local 0 / parent 54000 | PTS-local 10 labeled local 10 / parent 54010 |
| Ordinary keyboard seek to 100 after buffered steps to 90 | Accurate scan reaches 100, but settles at pre-roll 60 | Settles and presents 100 |

The defective decoder/controller source was already present at base `06ad872`;
those files were unchanged by the August integration. Independent FFmpeg checks
found consistent metadata/keyframes and identical target-frame hashes from
sequential and timestamp-seek decoding. No recording data or video encoding was
changed by this fix.

## Implementation

- Post-scan `decode_frame_cursor` is now the sole source for both settlement and
  the sequential fallback counter. There is no separate pre-scan settled value.
- Portable helpers used by the decoder cover cursor advancement, preserving a
  queued target, final bookkeeping, and local-to-parent publication.
- Exact camera settlement checks every camera in the current seek generation
  before updating display/slider/stimulus state. A mismatch returns `Failed`.
  Intentionally approximate seeks and stimulus settlement retain their behavior.
- A rejected ring is quarantined: paused resident selection, buffered resume,
  and camera uploads in both paused and playing modes cannot consume it. The
  existing front texture may remain visible but is not newly claimed as the
  requested frame. Recovery requires a fresh backend seek; quarantine persists
  during submission and clears only after accepted camera settlement, avoiding
  a race with asynchronous ring clearing.

`decoder_seek_bookkeeping_tests` covers scans 0→10 and 60→100, the target remaining
queued, parent mapping to 54010, subsequent frame 101, target zero, map fallback,
exact under/overshoot rejection, approximate acceptance, and quarantine gating.

## Verification

Host: RTX A6000, CUDA 12.4, GNU 13.3, OpenCV 4.10.0, TensorRT 10.0.1.6.
Native incremental build passed 42/42 steps; the expanded regression target was
then rebuilt. Complete serial CTest: **95/95 passed**, 8.76 seconds. Standalone
helper compilation with `-Wall -Wextra -Werror` and `git diff --check` also passed.

Authenticated GPU paused captures passed at:

- Cam2010093: 0, 10, 30, 40, 53900, 54000, 54010, 54030, 54040, 2592040.
- Cam2010094, Cam2010095, Cam2010096: 54010 each.

All 13 captures had identical requested, presented, bounding-box-query, and
canonical-timeline frames, stable for 60 rendered frames. Across 130 logged
post-seek frame samples, decoded PTS-local identity matched the published local
label with zero mismatches. Cam2010093 frame 54010 was visually inspected with its
canonical box and ready eye-angle, speed, bout, and detector-response panels.

Ordinary keyboard navigation, without UI-reference mode, passed resident steps
10–90, nonresident exact seek 100, steps 110/120, reverse steps 110/100/90 (including
a new backend seek to 90), and single-step 91. The same script against the old
binary failed at requested 100 / settled 60. UI-reference mode intentionally
freezes rendering after its ready marker and is not used for keyboard testing.
An additional ordinary-window run resumed playback after frame 91, advanced
beyond frame 150, and paused successfully.

Live August boundary smokes passed at 53900:54010 and 2592020:2592040, including
matching video/query frames and one canonical box. The required June
GoodCopBadCop playback control passed 0:300. The May cam2010095 materialized-clip
control passed 53990:54010 using its explicit `--recording-clip-index`, with
matching video/query frame 54010 and one legacy box.

Validated GUI SHA-256:
`a1f6ef3335b8cbe8ad0fa291f1cd26db3ad620a40640ddba973e4b97edbe607a`.

## Evidence and remaining limits

Local logs, captures, and bounded reproduction scripts:
`/tmp/crimson-paused-seek-fix-20260917-heOsVc/`.
Original diagnostic evidence:
`/tmp/crimson-paused-seek-20260917-lw4gcz/`.
These temporary artifacts are not included in a repository clone.

- Rejected-ring recovery is policy-tested and source-reviewed, not fault-injected
  through live NVDEC. The fixed decoder no longer produces the historical
  mismatch. Initial demux `Seek()` failure keeps its prior acknowledgment
  semantics and was not redesigned here.
- Runtime GPU surface bytes were not compared with CPU frame hashes. PTS/labels,
  presentation/query identity, and visible overlays were checked; this does not
  rule out every independent decoder-reuse visual artifact.
- This is bounded seek/playback validation, not full-recording corruption,
  endurance, final-frame, or colleague-workstation portability qualification.
- Existing mixed FFmpeg ABI warnings and workstation-specific prebuilt
  TensorStore reuse remain unchanged.
- A May control invocation without its explicit clip index selected the
  long-video fallback, so the clipped-only smoke rejected startup and triggered
  the already-observed early-return thread-cleanup abort. This is not a seek-fix
  result; the failed log is retained as `may-boundary.log`. The corrected control
  passed and is retained as `may-explicit-boundary.log`.
