# Linux loading responsiveness — 2026-09-23

Implemented on `codex/main-integration-20260922`, based on `c30f8e7`.
This is a locally validated checkpoint; no main merge, push, new
package, public drop, or recording changes were made.

## Changes

- The shared loading modal has a stable ImGui identity and font-scaled,
  viewport-bounded dimensions. Its phase text and progress bar no longer drive
  auto-resize feedback. The regression test reproduces the old width drift and
  checks 300 frames, changing progress/text/title, reopen, fonts and viewports.
- A portable single-flight loading runner executes CPU work off the owner
  thread while that thread polls events and draws only the loading modal.
  Failure, close, stale generation, reentrant calls and adoption are tested.
  Closing drains an in-flight read with event polling; it cannot forcibly
  interrupt the underlying storage operation.
- Linux CLI and deferred File Open / Load Zarr Archive / Load Stimulus Video
  paths use it for archive reads, media/index discovery, video/image probes,
  camera calibration, stimulus probes and initial legacy mask reads.
  Dialog selections are copied and executed after the preceding UI frame ends.
- Prepared camera/stimulus values own their CPU resources. Decoder joins finish
  before old resources are cleared. Live scene changes, GPU allocations and GL
  texture uploads stay on the owner thread. Allocation loops poll events and
  stage boundaries check for close before starting more work.
- The loading modal remains visible across session-open stages. Repainting is
  limited to approximately 60 Hz even when VSync is disabled. Logs expose
  per-worker-stage duration and maximum heartbeat gap.
- Ordinary clip switching, normal playback demand and scheduler/read-ahead
  policy are preserved. Optional refined overlay loading was already async;
  its multi-second log reports worker time, not a UI stall.

## Validation

- Full build in the pinned Ubuntu 22 / CUDA 12.4 builder: passed.
- Full CTest: **107/107 passed**, 11.04 seconds.
- Loading runner independently passed ASan, UBSan and LeakSanitizer.
- Required June GoodCopBadCop GUI playback, frames 0–300: passed, including
  GPU stimulus initialization. The 4.33-second archive read had a maximum
  heartbeat gap of 35.04 ms. Initial mask warm-up was also pumped.
- August Sleepyfish camera 2010093, frames 53990–54320, 60 Hz render cap:
  **329/329 presented frames had first-draw masks**, no coverage/identity errors.
  Two video-clock skips are reported separately. Peak decoded mask cache was
  5,905,630 bytes; this is not a total-process memory bound.
- August's archive read took 2.41 seconds (maximum heartbeat gap 33.12 ms).
  Affiliated-media discovery took 1.56 seconds with a 17.84 ms maximum gap;
  index opening took 0.48 seconds with a 17.51 ms maximum gap.
- Real close request during archive loading: exited successfully, rejected
  adoption and started no decoder afterward. The active read drained in
  4.34 seconds with continued polling (maximum heartbeat gap 34.47 ms).
- Owned, isolated GUI windows verified File Open, archive opening, invalid
  archive error/dismissal, same-archive reopening with active media, and
  replacement of an existing GPU stimulus decoder. Reload returned to ready
  at generation 3. Loading and normal-window screenshots were inspected.
- Source whitespace and CMake preset syntax checks passed. The two new portable
  tests are included in the explicit macOS build preset, but **native macOS and
  Windows were not built or run**. Linux GUI wiring is not a claim of Apple
  backend adoption.

Final logs, traces, screenshots and executable checksum are retained in the
ignored `build/loading-evidence-20260923/` directory. Tests used the freshly
built `release/redgui` with matching libraries from the existing local
`dist/Crimson-linux-integration-20260922` staging directory; that staged
executable and Ginny's public package were not replaced.

## Local visual check

From an authenticated local GPU display, run the new executable, not an older
package executable. This example creates isolated settings/window-layout state:

```bash
cd /home/delahantyj@hhmi.org/gitrepos/crimson-main-integration-20260922
crimson_repo="$PWD"
crimson_stage="$crimson_repo/dist/Crimson-linux-integration-20260922"
crimson_test_dir="$(mktemp -d /tmp/crimson-loading-user-check.XXXXXX)"
cd "$crimson_test_dir"
LD_LIBRARY_PATH="$crimson_stage/lib:$crimson_stage/lib64:$crimson_stage/lib/crimson/private:$crimson_stage/lib64/crimson/private" \
XDG_CONFIG_HOME="$crimson_test_dir/config" \
XDG_CACHE_HOME="$crimson_test_dir/cache" \
"$crimson_repo/release/redgui" \
  --zarr /misc/public/forGinny/recordings/2026_08_06_19_13_35_cam2010093/zarr/2026_08_06_19_13_35_cam2010093_analysis.zarr \
  --show-subject-masks --swap-interval 0 --frame-cap-fps 60
```

For tmux, supply a currently authenticated `DISPLAY`/`XAUTHORITY` pair as
described in `AGENTS.md`; do not assume an old Xwayland cookie is still valid.

## Limits

Individual driver/GL/CUDA calls are still synchronous. Polling between allocation
slots cannot interrupt a blocked driver call. In-memory repository conversion
and scene adoption remain owner-thread work. Ordinary seeks and non-session
reload/write paths were not converted to this loading workflow. These tests
are not a guarantee against every possible OS unresponsive-window warning,
nor storage-cold latency qualification.
