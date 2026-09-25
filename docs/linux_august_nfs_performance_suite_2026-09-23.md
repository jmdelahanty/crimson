# August Linux/NFS performance regression suite

The opt-in suite is `tools/august_nfs_performance.py`, with the frozen workload
and provisional acceptance budgets in
`tools/fixtures/august_nfs_performance_v1.json`. The GUI instrumentation is
disabled unless `CRIMSON_PERFORMANCE_CASE` is explicitly supplied. Ordinary
interactive runs retain their existing behavior.

## Workloads and gates

Three fresh-process repetitions reverse case ordering on alternating runs:

- Twenty seconds of sustained 30 FPS source playback at a 60 Hz render cap.
- Ten seconds spanning the 54,000-frame clip boundary.
- Ten seconds spanning the 55,296-frame in-memory timeline-page transition,
  with overlays and shading enabled, to exercise sequential page turnover.
- The sustained-playback and clip-boundary workloads each run with plots only,
  overlays without bout shading, and
  overlays plus bout shading. Full overlays include masks, contours, keypoints
  and the normal shape options, not every optional spline diagnostic marker.
  The `plots_only` control disables those camera layers, not the existing
  detection boxes or Frame Inspect's independent keypoint-data requests.
- Twenty seconds of immediate playback with no paused prewarm; first combined
  readiness and startup-pending draws are reported separately from later misses.
- Ten deterministic paused seeks in one process, repeating five targets to
  exercise retained caches. Readiness requires two consecutive draws at the
  exact target with the motion/eye/bout windows and requested overlays ready.
  Initial opening readiness is separate from subsequent seek percentiles.

The versioned budgets include render-loop p95/p99/max, timeline UI p99, skipped
video-frame fraction, first-draw overlay lateness, timeline readiness, clip
handoff intervals without a newly presented frame, initial/seek readiness,
current-priority queue wait and peak RSS. Missing telemetry, incomplete runs,
source/frame mismatches and scheduler failures fail closed. The checker does
not silently turn the 60 Hz cap into a claim of sustained 60 Hz performance.

The continuity follow-up adds a separate visible-image hold gate. It uses the
post-draw front-texture parent-frame identity, source FPS, playback rate, and
explicit manual-seek markers. The provisional maximum is 2.5 source-frame
periods (83.33 ms at 30 FPS and 1x). Automatic clip-switch waits remain included;
deliberate pauses, manual seeks, warmup, and rate changes break measurement
epochs. Unknown front-texture identity fails closed. Invalid/absent visible
images have separate fraction and duration gates. Measurement ends at smoke
completion, not after shutdown. This is application presentation telemetry,
not a measurement of physical monitor scanout.

Earlier measurements below predate this gate and cannot qualify against it:
they lack the new continuity schema and visible-image telemetry. A valid frame
could previously freeze for hundreds of milliseconds while the old gates
passed. See the [nonblocking clip-switch report](linux_nonblocking_clip_switch_2026-09-23.md)
for that historical limitation and the
[prewarm follow-up](linux_clip_prewarm_2026-09-23.md) for the subsequent work.

A negative presented-frame sentinel during handoff is not an overlay event for
frame -1. Such draws are counted separately and remain included in frame-time
statistics. They do not establish whether the retained camera texture was
visible or blank; that needs image inspection.

The first calibration exposed seek-transition frame-time spikes. Those remain
failures under the p99 budget; no application seek fix or relaxed seek-specific
frame-time limit is part of this suite. Budgets are explicit provisional product
targets, not assertions that the existing application already satisfies them.

## Read and cache accounting

The tested `/groups/johnson` mount is NFSv4.1. Every run records `findmnt` output,
host/kernel/GPU/driver, binary SHA256, Git revision and dirty diff hash, untracked
source hashes, recording/index/selector metadata hashes, actual selected source
bindings, full arguments and raw traces. The runner requires NFS and an
authenticated display and creates a new output directory; it refuses to
overwrite prior evidence.

Three different quantities must not be conflated:

1. TensorStore file reads/batch reads/bytes are process-level application file
   access, including initialization and shutdown. They can be served from the
   kernel cache; they exclude FFmpeg video reads. A sampled post-prewarm byte
   upper bound is reported separately when available. The summary's `file_reads`
   is the sum of the `read` and `batch_read` API counters, not a count of POSIX
   calls or network operations; raw counters are retained for interpretation.
2. Dense-mask and contour logical bytes describe repository payload requests,
   not compressed wire traffic. Timeline row counts are also retained.
3. Linux mountstats server-read bytes and READ RPC deltas describe this client's
   entire mount, including video and other processes. They are not attributed
   exclusively to Crimson and are not directly gated against a previous run.

Fresh processes reset application caches, **not** OS or NFS-server caches. The
label is always `fresh_process_os_and_server_cache_uncontrolled_no_flush`.
Repeated targets are within-process cache reuse, not guaranteed all-layer hits.
The suite never drops global caches, remounts storage, alters recording data,
or terminates a pre-existing Crimson process. It starts/stops only its own
isolated GUI processes. Use a quiet workstation for comparable measurements;
remote-server load remains uncontrolled.

Render tick intervals include UI work, cap sleep, trace overhead, and work
between main render-loop iterations (including clip transitions). They are not
GPU timer-query durations. Both candidate and baseline must use the same
instrumentation. Queue/service telemetry reports averages/totals/maxima, not
uncollected per-request p95/p99 distributions. Overall timeline UI cost includes
shading; there is no separate GPU shading timer.

## Running and comparing

Build `redgui` first. Supply the authenticated display and packaged runtime
libraries as in the [local launch instructions](linux_sleepyfish_contours_shape_controls_2026-09-23.md#local-visual-check).
From the integration repository:

```bash
python3 tools/august_nfs_performance.py \
  /misc/public/forGinny/recordings/2026_08_06_19_13_35_cam2010093/zarr/2026_08_06_19_13_35_cam2010093_analysis.zarr \
  --output /tmp/crimson-august-nfs-baseline --repetitions 3
```

For a quick calibration, use `--repetitions 1 --case clip_boundary/overlays_shading`
or `--case repeated_seeks`. Subsets and fewer than three repetitions cannot
produce `qualified: true`, even when their individual cases pass.

To compare a later binary against a complete passing baseline, add:

```bash
--baseline /tmp/crimson-august-nfs-baseline/result.json
```

Comparisons require identical workload/threshold digest, recording fingerprint,
selected source bindings, cache policy and environment. Medians across matched
cases are compared with a 20% relative allowance plus metric-specific absolute
floors (whichever allowance is larger). Shading-on versus shading-off file-byte
amplification is checked separately. Absolute case gates still apply, so a
baseline comparison cannot hide poor performance shared by both revisions.
These are engineering regression tolerances, not statistical confidence bounds.

`passed` means every measured case and comparison passed; `qualified` additionally
requires the complete workload with at least three repetitions. Neither is a
native macOS/Windows or colleague-workstation release qualification. Failed
results remain useful evidence but cannot silently become passing baselines.

The deterministic Python checker tests and C++ workload-parser tests run under
CTest without NFS or a GUI. Actual GPU/NFS workload runs are manual opt-in, not
part of ordinary CTest.

## September 23 integration measurements

The Ubuntu 22 integration build passed all 110 CTests. The Python checker also
has 14 direct tests, including rejection of internally consistent overlays
attached to the wrong presented video frame. The final binary passed the
authenticated June recording GUI smoke over frames 0–300. Logs are
`ctest-final.log` and `june-app.log` under the evidence root.

Evidence root: `/tmp/crimson-august-perf-20260923.dDBOMp`.
The tested `release/redgui` SHA256 is
`e2b5ac7de424e0795fe21856712b78439a5f3875e67bef46e525bb9bccf33792`.
It was built from `dd41281` plus the uncommitted shading/performance changes;
this is not a published release.

The original eight-case matrix completed three repetitions (24 trials) in
`repeated-baseline/result.json`: **19 passed, five failed; not qualified**.
The dedicated timeline-page case was added while those trials were running,
so that report retains its original workload digest. The subsequent three
page-transition trials are separate evidence, not a combined nine-case
qualification. The default fixture now includes all nine cases.
All 27 raw traces were rechecked with the final checker, including its
presented-frame identity assertion, with unchanged outcomes.

| Workload | Passing trials | Median render tick p99 | Median application file bytes |
| --- | ---: | ---: | ---: |
| Sustained, plots control | 3/3 | 17.19 ms | 1,329,741,635 |
| Sustained, overlays | 2/3 | 17.18 ms | 1,334,280,734 |
| Sustained, overlays + shading | 3/3 | 17.20 ms | 1,334,280,734 |
| Clip boundary, plots control | 3/3 | 17.20 ms | 1,329,620,754 |
| Clip boundary, overlays | 3/3 | 17.21 ms | 1,333,400,141 |
| Clip boundary, overlays + shading | 3/3 | 17.20 ms | 1,333,400,141 |
| No paused prewarm | 2/3 | 17.19 ms | 1,333,860,236 |
| Repeated paused seeks | 0/3 | 136.94 ms | 1,381,452,027 |
| Timeline page boundary (separate run) | 2/3 | 17.12 ms | 1,335,211,034 |

The paired shading-on/off playback cases had identical median application-read
bytes and API counter sums. Timeline UI p99 medians remained below 0.6 ms. This
supports reuse of cached data for shading in these ranges; it is not proof of
zero overhead or a measurement of process-attributed NFS network traffic.

Failures retained without loosening limits:

- One sustained-overlay trial had 35/601 first-draw mask misses (5.82%, limit
  1%). Of these, 32 were at playback start while opening was finishing, and
  three occurred later. The other repetitions had zero misses. A later
  shading-on run cannot establish that shading improved mask readiness.
- One unprewarmed trial had 1.07% late overlays after first combined readiness,
  above the 1% limit. Initial readiness was separately measured at roughly
  7.6–7.9 seconds in these no-prewarm trials.
- All three repeated-seek trials exceeded the 50 ms render tick p99 target:
  136.94, 138.25 and 122.14 ms. Subsequent exact seek readiness remained below
  624 ms, but that does not excuse the UI render stalls.
- One page-transition trial had 24/301 late mask/contour frames (7.97%): nine
  at playback start and fifteen later. They were not at the exact page boundary
  (55,296). All three trials reported zero late timeline draws; their median
  timeline UI p99 was 0.44 ms. Do not attribute mask lateness to page turnover
  from this evidence alone. Raw supplemental evidence is in
  `timeline-page-boundary/result.json`.

These are current performance gaps detected by the new suite, not demonstrated
regressions against a previously qualified revision. No seek/prefetch fix,
budget relaxation, commit, push, or package publication accompanies this report.
