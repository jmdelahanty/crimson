# NVIDIA next-clip prewarming — September 23, 2026

This follows the [nonblocking switch](linux_nonblocking_clip_switch_2026-09-23.md)
and targets forward playback through August recording-index clip boundaries.
Changes remain on `codex/main-integration-20260922`; this work does not publish a
package or qualify macOS, Windows, or the colleague's RTX A4000.

## Resource and ownership design

The existing 100-frame video ring and persistent viewport remain in place. A
single speculative decoder owns an independent demuxer, seek state, stop flag,
and one staged GPU NV12 frame. At 4512 x 4512 the staged frame is 30,537,216 bytes
(29.12 MiB), compared with about 2.84 GiB for the existing ring. Adoption copies
the exact boundary frame into the normal ring and rebinds the **same decoder
worker** to that ring; it does not decode a prefix then cold-restart playback.

For safe pointer lifetime, the adopted decoder retains its staging allocation
until that worker is joined. At a later boundary the old retained stage and the
new candidate stage can coexist: about 58.24 MiB total staging for this recording.
The admission budget is per speculative stage, not a total process-memory cap.

The old decoder is retired asynchronously only after read leases prove that
every remaining old-clip parent frame through its final frame is resident.
The old ring stays usable while that worker exits. A capacity of 100 alone is
not treated as proof of a contiguous tail.

The fast path requires the same camera, dimensions, codec, pixel format, color
metadata, and source rate, with an exact parent/local boundary-frame match.
Calibration still follows the ordinary lookup, including clip-specific metadata;
matching camera serials alone do not justify reusing a calibration.

Handoff requests/settlement now observe the actual front texture after drawing
and swapping, rather than a prospective read-head slot. Ready candidates wait
until the boundary frame is due on the existing transport clock. An unchanged
parent timeline no longer calls `updateTimeline()` during clip activation: that
call rounded away fractional clock phase. The initial implementation showed the
first new frame early and held it for 84.99 ms despite later frames being ready;
the fix preserves timing instead of adding another staged frame.

Manual seeks and pause cancel speculation; session-open barriers drain workers
before replacing mappings. Restoration after early retirement queues an exact
seek to the previously visible parent frame. Shutdown and ordinary reload stop
the adopted worker's own stop flag before joining it. Failed activation retains
the recording provider for a cold retry. Cold fallback waits for speculative
cleanup to finish before starting another decoder. Rare thread-launch or
activation failures can still require a synchronous safety drain; an unbounded
responsiveness guarantee is not claimed for those exceptional paths.

The staging budget defaults to 64 MiB. Admission also requires 1 GiB of free
GPU-memory headroom beyond the staged allocation. Decoder-internal surfaces and
driver allocations are additional: this is **not** a hard cap on total decoder
memory or a guarantee that a second session can initialize. Preparation and
retirement have dedicated workers, separate from analysis-data scheduler jobs.

The initial lead policy is three seconds converted using source FPS and
playback rate, capped at 90 frames. It is conservative and bounded, not yet an
adaptive latency estimator. A short lead, incompatible stream, exhausted
budget, failed initialization, or late readiness uses the responsive paused
switch path. That fallback is expected to fail the seamless-continuity budget;
correct playback and responsiveness remain separate requirements.

Diagnostic controls:

- `CRIMSON_DISABLE_CLIP_PREWARM=1` selects the ordinary async switch path.
- `CRIMSON_PREWARM_MAX_BYTES=0` exercises staging-budget rejection.

One physical NVDEC engine does not itself imply only one decoding session.
Actual codec, device, driver, memory, and throughput constraints still apply;
the hardware-independent promise is fallback, not seamless playback everywhere.

## Validation

Evidence root: `/tmp/crimson-clip-prewarm-20260923.4Z8eBn` (local, not checked in).
The original nonblocking binary is retained there as `redgui-before-prewarm`.
Tests use the existing August recording over NFS without modifying data, flushing
global caches, or stopping pre-existing application processes. GUI configuration
and output directories are isolated; window controls verify the owned child PID.

Final binary SHA256:
`2e882eeb6218c2be696c1255adc03f67546bdd18cad29a068990a57b50857ef9`.
Built with the pinned Ubuntu 22 builder from `dd41281` plus the uncommitted
integration changes. Source stayed frozen during final qualification checks;
only documentation changed afterward. The local GPU was an RTX A6000.

- All **112/112 CTests** passed (`ctest-final.log`), as did all **18 Python
  checker tests**, `git diff --check`, and the required authenticated June
  frames 0–300 GUI playback smoke (`june-app.log`).
- The initial clock-corrected boundary spot check passed every gate, with a
  maximum visible-image hold of 34.84 ms and render tick p99 of 17.23 ms
  (`clock-fixed-boundary`).
- Three further boundary trials all passed the video-continuity gate. Their
  maximum frame holds across playback were **34.61, 34.35, and 43.86 ms**,
  compared with the earlier 407.66 ms hold of frame 53,999. No visible-image
  gaps or source-frame skips occurred in these three trials. Tick p99 was
  17.11–17.23 ms, including cap sleep and tracing; this is not a hard 60 Hz
  guarantee. Three repeated paused-seek trials also passed, with subsequent
  readiness p95 of 714–766 ms.
- **The six-trial set is not a passing full-performance qualification.** One
  boundary trial failed the unchanged overlay-lateness budget: masks/contours
  were late on initial frames 53,940–53,943 (4/301, 1.329%, against 1%). Overlay
  opening took 7,987.66 ms against the fixed eight-second warmup, followed by
  pending initial payloads. Frames around the actual 54,000 boundary were ready
  and correctly aligned. This startup-readiness failure remains recorded in
  `repeated-targeted/result.json`; neither the warmup nor the threshold was
  relaxed. The other five trials passed all their gates. This subset alone
  could not establish `qualified: true` even without that failure.
- Disabling prewarming and rejecting its staging budget both continued playback
  and closed cleanly (`fallback-disabled`, `fallback-capacity`). Holds were
  389.74 and 391.44 ms, with tick p99 of 17.14 and 17.19 ms. These intentionally
  **failed the continuity gate**, and all other existing gates passed. The
  fallback skipped one and two source frames respectively near the transition;
  it is responsive, not seamless or guaranteed frame-complete.
- Closing the owned window during preparation passed with no adoption and a
  clean exit (`close-during-prepare`). Pausing after early retirement, issuing
  20 backward seek inputs beyond the resident tail, waiting for exact frame
  53,745, and resuming through the boundary passed (`seek-after-retirement`).
  The equivalent test after adoption reached exact frame 53,801 in the old
  clip, resumed, adopted again, and closed cleanly (`seek-after-adoption`).
- An attempted very-late-entry workload (`fallback-late-entry`) actually landed
  at the preceding keyframe, 53,970, because the playback smoke uses an inaccurate
  initial seek. It successfully prewarmed; it **does not** validate a candidate
  missing its deadline. The artifact is retained without relabeling it as a
  successful forced-late fallback.

The policy tests cover bounded lead, contiguous-tail identity, exact adoption,
and staging admission. They are not a synthetic test of every GPU lifecycle
failure. Opening/canceling a different archive during speculation was code
reviewed but not GUI-qualified. Real device/session exhaustion, long NFS stalls,
and additional hardware/backends remain unqualified. OS/server caches were
uncontrolled; these are not storage-cold latency guarantees. Calibration and
GPU allocation/activation still have owner-thread work.

## Trying the integration build

Use `release/redgui` in this integration worktree, **not** an older `dist` or
Ginny package. The [local launch instructions](linux_sleepyfish_contours_shape_controls_2026-09-23.md#local-visual-check)
provide its runtime-library environment and isolated settings. For an automatic
visual crossing, add these flags to that command:

```bash
--playback-smoke 53940:54240 --playback-smoke-warmup-seconds 8
```

The window closes after that smoke finishes. For interactive testing omit those
smoke flags, seek near frame 53,940, and play across 54,000. No commit, push,
main merge, worktree removal, or package publication is part of this increment.
