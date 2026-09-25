# Subject-Mask Production-Location Gate

Date: 2026-08-12

Crimson commit: `e570e46006f06f50687f86d9df108fcefb255c8c`

## Verdict

The exact selector-ineligible production-path bundle is compatible with
Crimson, but it does not pass the frozen performance gate on this mounted Mac
path. Production activation is not recommended from this result.

- Compatibility and correctness: PASS.
- Five-repetition performance gate: FAIL.
- Clip-backed Metal visual smoke: PASS.
- Production activation authorized: no.

The validator accepted the production import's strict version combination:
bundle v3, raw/refined core v5, quality v2, and sampled-contour cache v2. This
is separate from the newer receipt-composed bundle-v4 path.

## Correctness

The exact requested bundle and all four member manifests passed direct versus
consolidated metadata comparison, schema, dtype, storage-plan, codec, digest,
coordinate, and cross-binding validation. The probe validated 51 array
declarations. A wrong bundle ID and wrong digest both failed closed.

All 1,169,010 refined rows joined exactly to crop geometry across 1,188,000
camera frames. Instance-key mismatches and placement mismatches were both zero.
Every fresh process read and retained `frame_row_offsets` exactly once. Normal
sampled-contour presentation performed no dense-mask, quality, derived-metric,
or `source_point_count` payload reads. No stale visible frame was published.

## Performance

The frozen gate requires warm random-frame p95 at or below 150 ms and zero
post-warmup deadline misses in every trial.

Sampled contours had 197.62 ms median and 233.75 ms maximum warm-random p95.
They had zero deadline misses, 501 MB maximum RSS, and a median of 1,777 file
reads / 206.9 MB transferred per process.

Dense masks had 225.06 ms median and 230.17 ms maximum warm-random p95. The
fifth repetition missed three of nine measured pages in both forward and
reverse traversal, producing a maximum deadline-miss ratio of 0.333. Dense
maximum RSS was 571 MB.

The earlier benchmark-namespace qualification passed with 70.36 ms sampled and
80.27 ms dense median warm-random p95. The production-location run decoded the
same declared metadata volume and issued comparable read counts, but observed
higher transferred bytes and service latency. This result does not establish
whether the difference comes from mounted-filesystem cache state, server load,
or another path-level effect; it does establish that the frozen production
activation gate did not pass.

## GUI Smoke

The app opened the production archive explicitly through the recording's
22-clip `recording_clip_index.json`, using the exact refined-mask and
sampled-contour member IDs and digests. At frame 1000, one observation exposed
all four sampled contours, the full-camera overlay aligned with the fish, and
the ROI inset reported `Live geometry exact`.

At clip boundary frame 54,000, requested and presented frames matched, all six
video buffer slots were valid, the next clip rendered, and the overlay and ROI
inset remained aligned. A specifically selected spatial crop-boundary ROI was
not available in the frozen workload, so the visual claim is limited to the
normal frame and clip-boundary smoke.

## Evidence

- `aggregate.json`: raw trials, summaries, environment, probes, and gates.
- `aggregate.sha256`: aggregate digest.
- `result.json`: concise cross-check and final verdict.
- `result.sha256`: result digest.
- `trials/`: ten fresh-process trial documents.

The benchmark executable and all trial documents are bound to the full Crimson
commit above and report a clean tracked worktree. No archive, registry, or
selector was modified.
