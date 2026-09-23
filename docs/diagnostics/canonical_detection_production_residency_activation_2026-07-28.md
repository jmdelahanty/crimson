# Canonical Detection Production Residency Activation

Date: 2026-07-28

Status: accepted for the native macOS application. The policy and repository
boundary are backend-neutral; Linux/Windows entry-point adoption remains a
separate checkpoint.

## Decision

Activate byte-budgeted background residency for the selected canonical or
refined detection surface after its first page is available. The production
policy admits at most 64 MiB of decoded UI columns and reads them in bounded
512 KiB chunks. Paging remains authoritative while the snapshot is built and
remains the permanent fallback after budget rejection, cancellation, or
failure.

Residency is not an analysis-readiness requirement. It is speculative work and
may yield behind initialization indefinitely without delaying current-frame
demand or playback.

## Mounted Gate

The final pre-activation run used the full-duration hybrid fixture on the
mounted Johnson Lab volume:

- archive: `sleepyfish_cam2010095_v1/hybrid.zarr`;
- run: `crimson_storage_fixture_sleepyfish_cam2010095_v1`;
- 1,188,000 camera frames and 1,187,087 detection rows;
- shared 64 MiB TensorStore cache;
- four scheduler workers with one reserved current-frame slot;
- concurrent maintained-product initialization; and
- the existing deterministic seek, 700 FPS traversal, cancellation, and
  shutdown workload.

This was a fresh process with uncontrolled macOS, SMB, and server caches. It is
an activation-safety gate, not a physical-layout comparison.

## Result

| Metric | Observed |
| --- | ---: |
| current-frame maximum queue wait | 0.149 ms |
| during-build demand publication | 15.72 ms |
| first detection overlay after archive open | 1,557.84 ms |
| required-product readiness | 104,174.05 ms |
| resident build elapsed | 103,263.33 ms |
| resident decoded/retained bytes | 28,490,088 B |
| resident publications | 1 |
| stale/failed resident chunks | 0 / 0 |
| forward/reverse deadline misses | 0 / 0 |
| post-residency traversal UI field reads | 0 |
| rapid-seek stale publications | 0 |
| peak process RSS | 1,536,049,152 B |
| bounded shutdown | 7.94 ms |

The resident source was deliberately speculative. Its longest queue wait was
87.25 seconds while required analysis initialization used the non-current
capacity. This is the intended priority policy: the snapshot completed shortly
after required-product readiness, while current-frame demand continued through
the reserved slot. The build did not turn slow background work into a loading
or playback dependency.

## Production Behavior

1. Request the initial detection page through ordinary paging.
2. Observe an actual published frame before attempting residency.
3. Calculate the exact decoded hot-set size from the selected repository.
4. Reject the candidate without allocation when it exceeds 64 MiB.
5. Build bounded chunks on the distinct speculative residency source.
6. Validate and atomically publish one immutable snapshot.
7. Resolve later UI ranges from RAM without TensorStore payload reads.

The macOS diagnostics report `activation`, state `transition`, and `summary`
events. Decisions are explicit: `building`, `resident`, `rejected_budget`,
`fallback_paged`, `cancelled`, or `disabled`. A failed or ineligible residency
attempt never marks the selected detection surface unavailable.

Session close cancels an active build, waits for its source to become idle,
records elapsed time, and discards partial columns. Headless fake-repository
coverage proves current-frame demand overtakes a blocked build and that neither
explicit cancellation nor close publishes a partial snapshot.

## Native Application Smoke

The release application then ran a mounted `--video-smoke 0:5` against the same
archive and explicit detection run. It reported the 64 MiB/512 KiB policy,
observed the first paged frame, and activated a 55-chunk, 28,490,088-byte
candidate. The intentionally short smoke ended while the builder was active:

- 24 chunks and 12,582,720 decoded source bytes completed;
- close cancelled the build in 396.4 ms;
- retained bytes and publications remained zero;
- stale/failed chunks were 1/0;
- detection current-frame queue maximum was 0.2 ms;
- all 32 resolved detection pages succeeded; and
- exact video playback reached frame 5 and passed.

This proves the native lifecycle does not expose or retain a partial snapshot.
The same run reproduced a separate legacy-keypoint problem: its initial-frame
repository callback took about 58 seconds and did not settle before the smoke's
keypoint timeout. Detection residency started afterward and did not cause that
delay; keypoint metadata and row-access optimization remain separate work.

## Validation

- `canonical_detection_repository_tests`: passed;
- complete macOS suite: 59/59 passed; and
- mounted native `--video-smoke 0:5`: passed.

## Evidence

Raw result:

`docs/diagnostics/canonical_detection_production_residency_activation_2026-07-28/mounted_hybrid_resident_reserved_r2.json`

SHA-256:

`2ca4338570b3bf09087898fead7a695fc784296ab8d780f35800a645de3e4912`

The earlier 20-process isolated and ten-process full-archive comparisons remain
the statistical strategy evidence. This run adds the scheduler-reservation
activation checkpoint; it does not replace those balanced comparisons.
