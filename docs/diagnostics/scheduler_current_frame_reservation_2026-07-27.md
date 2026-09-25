# Current-Frame Scheduler Reservation Checkpoint

Date: 2026-07-27

Status: accepted for the native macOS application and matching full-archive
gates. This is a Crimson scheduling decision, not a Palette storage-profile
decision.

## Question

Can long, non-preemptive analysis initialization occupy all four shared workers
and delay a current-frame overlay even when the overlay's repository work is
short? If so, does reserving one worker remove that delay without breaking
bounded startup, traversal, cancellation, memory, or physical-I/O behavior?

## Workload

Both fresh-process trials used the frozen full-duration hybrid/paged fixture on
the mounted Johnson Lab volume:

- archive: `sleepyfish_cam2010095_v1/hybrid.zarr`;
- explicit run: `crimson_storage_fixture_sleepyfish_cam2010095_v1`;
- 1,188,000 camera frames and 1,187,087 detection rows;
- shared 64 MiB TensorStore cache;
- concurrent maintained-product initialization;
- eight deterministic settles and eight rapid seeks;
- 3,500-frame forward and reverse traversal in 70-frame pages at a simulated
  700 FPS; and
- bounded cancellation and shutdown.

The baseline used Crimson commit `49a6104` with four workers and no reserved
capacity. The second trial used the same base plus the portable reservation:
four workers, at most three simultaneously active non-current requests, and one
slot available for future `CurrentFrame` work. macOS, SMB, and server caches
were uncontrolled, so this pair is a scheduling checkpoint rather than a
storage-layout comparison.

## Result

| Metric | No reservation | One current-frame slot | Change |
| --- | ---: | ---: | ---: |
| current-frame maximum queue wait | 28,991.36 ms | 0.98 ms | removed |
| current-frame average queue wait | 1,832.93 ms | 0.11 ms | removed |
| first detection overlay | 30,636.35 ms | 1,642.60 ms | -28,993.75 ms |
| first-page repository service | 244.57 ms | 458.00 ms | +213.43 ms |
| all-products readiness | 61,676.40 ms | 67,036.49 ms | +5,360.10 ms |
| complete workload | 80,492.08 ms | 86,529.40 ms | +6,037.32 ms |
| peak RSS | 1,789,493,248 B | 1,391,837,184 B | diagnostic only |
| physical bytes | 577,749,185 B | 577,793,526 B | +44,341 B |
| forward/reverse deadline misses | 0 / 0 | 0 / 0 | unchanged |
| stale publications | 0 | 0 | unchanged |

The baseline first-page request arrived while four requests were active and
waited behind long initialization callbacks. Keypoint, subject-mask,
subject-shape, and eye-geometry initialization each occupied a worker for about
30-34 seconds. The detection page then required less than half a second of
repository service. This is head-of-line blocking, not a detection storage
failure.

The reserved run reached `peak_active=4` and
`peak_active_non_current=3`, proving the fourth slot was used for current-frame
work rather than reducing total concurrency to three. The reserved trial's
approximately 8.7% longer readiness is the conservative observed cost of
removing a 29-second interactive delay. Because macOS, SMB, and server caches
were uncontrolled, this single pair does not isolate the reservation as the
sole cause of that difference. Readiness remained below the frozen 180-second
limit, and the complete traversal/cancellation gate passed.

## Policy

- Reservation is opt-in at scheduler construction and backend-neutral.
- A four-worker application scheduler reserves one current-frame slot.
- Current-frame work may use any available worker; the slot is a capacity
  guarantee, not a dedicated thread.
- At least one non-current worker is always retained, so a one-worker scheduler
  clamps the reservation to zero.
- Active callbacks remain non-preemptive.
- Source isolation, speculative limits, priority ordering, cancellation, and
  immutable publication are unchanged.

A deterministic fake-repository test holds three background callbacks, proves a
fourth background request remains queued, proves current-frame demand starts in
the reserved capacity, and then proves background progress resumes. This test
does not depend on TensorStore or mounted storage.

## Evidence

- Baseline:
  `docs/diagnostics/scheduler_current_frame_reservation_2026-07-27/baseline_no_reservation.json`
  SHA-256 `a344845a1683dc3c9ebe6fe5c015d004a6f1d769126261c1effea94ecbf77fc1`
- Reserved:
  `docs/diagnostics/scheduler_current_frame_reservation_2026-07-27/reserved_current_frame.json`
  SHA-256 `3fa4ea19b3c24b3d7e9a98bd6955f45de626c5cbe9b06513b941fb944a65436f`

## Verdict

Accept one reserved current-frame slot for the four-worker application
scheduler and the equivalent full-archive gates. Do not increase worker count
or introduce callback preemption from this evidence. Continue to attribute the
remaining all-products readiness time and peak RSS separately.
