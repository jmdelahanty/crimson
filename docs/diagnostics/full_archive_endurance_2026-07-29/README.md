# Full-Archive Endurance Checkpoint

Date: 2026-07-29

Status: mounted macOS checkpoint passed. This is a bounded-memory and
scheduling diagnostic, not a storage-profile or cross-platform release gate.

## Scope

The primary run used the full 1,188,000-frame Sleepyfish access-aware archive,
the explicit canonical run `crimson_storage_fixture_sleepyfish_cam2010095_v1`,
the production 64 MiB TensorStore cache, and resident detection UI columns. The
archive remained on the mounted Johnson Lab volume.

Crimson commit:

`ff2d8c6f3c18f1fd336b3f5f6a14b71339649967`

The benchmark reported `crimson_worktree_dirty=false`. The Mac was arm64 and
ran macOS 26.3.1 build 25D2128. The implementation was also compiled from an
exact source archive on ws1: the portable test passed and the maintained
CUDA/NVIDIA `redgui` executable linked successfully.

The headless workload ran 20 cycles. Every cycle resolved two deterministic
simultaneous nine-product probes, issued eight rapid seeks, and traversed 3,500
frames in alternating directions at the simulated 700 FPS page deadline.
Traversal spans were distributed across the complete camera-frame domain.

## Result

| Metric | Result |
| --- | ---: |
| Complete process | 210.32 s |
| Endurance phase | 139.01 s |
| Median cycle | 6.93 s |
| Traversed frames | 70,000 |
| Post-warmup pages | 980 |
| Post-warmup deadline misses | 0 |
| Stale seek publications | 0 |
| Offset reads during endurance | 0 |
| Scheduler failed completions / exceptions | 0 / 0 |
| Endurance file transfer | 263.7 MiB |
| Endurance file reads / batch reads | 5,149 / 5,069 |
| Lifetime process peak RSS | 1,905.9 MiB |
| RSS after repository release | 260.1 MiB |

The lifetime peak includes repository initialization and transient decode work;
it is not the steady-state plateau value.

## Memory Plateau

The first two cycles were excluded as warmup. Both frozen policies passed over
the remaining 18 samples:

| Metric | Process RSS | Reported retained allocations |
| --- | ---: | ---: |
| Initial-window median | 1,064.0 MiB | 498.3 MiB |
| Final-window median | 995.9 MiB | 514.8 MiB |
| Final growth | -68.2 MiB | +16.6 MiB |
| Maximum analyzed value | 1,173.0 MiB | 526.8 MiB |
| Fitted slope | -29.79 MiB/min | +8.15 MiB/min |
| Verdict | PASS | PASS |

The repository number is allocated container capacity, while RSS is currently
resident physical memory. On macOS, compression and paging can make allocated
capacity exceed current RSS briefly. The two curves answer different questions
and are intentionally evaluated separately.

## Cache Pressure

The extended run reached the cache-pressure behavior that the ten-cycle trial
had not yet reached:

- subject-mask mapping pages grew to about 16.6 MiB, recorded two evictions,
  and remained at 16.5--16.6 MiB for the last four cycles;
- sparse mask payload chunks recorded 141 cumulative evictions, while current
  payload occupancy remained between 12.4 and 38.3 MiB;
- the canonical-detection presentation cache remained 215,040 bytes and
  recorded 232 evicted pages;
- resident canonical columns served 730 range resolutions; only three earlier
  resolutions used paging; and
- the shared scheduler retained four workers with one current-frame
  reservation.

Across the complete process, current-frame queue wait averaged `0.029 ms`,
peaked at `0.325 ms`, and never crossed `100 ms`. Scheduler timing maxima in the
JSON are process totals and include repository initialization. Cycle physical,
memory, and product-cache values are endurance-specific.

## Six-Cycle Sensitivity Trial

The earlier clean six-cycle trial failed only the provisional RSS slope limit.
Its four post-warmup RSS samples moved from a 1,000.3 MiB initial median to a
1,041.1 MiB final median, within the 256 MiB endpoint gate, but one late sample
made the fitted slope 203.1 MiB/min. Reported retained allocations stayed near
495.5--501.0 MiB.

That trial also exposed that a thrown plateau verdict omitted the completed
endurance summary from failure JSON. Commit `ff2d8c6` corrected failure-evidence
publication. The executable now requires at least ten cycles; thresholds were
not loosened after observing the failure. The six-cycle JSON is retained as
nonacceptance sensitivity evidence.

## Decision

The current full-archive Mac working set is bounded over this workload. Keep
the current cache budgets and current-frame reservation. This result does not
justify enlarging caches and does not settle evolving keypoint, mask, shape, or
eye storage contracts.

Still open:

- repeat the same workload in multiple fresh processes for a release gate;
- move the workload outward to production buffers as remaining adapters adopt
  the shared scheduler;
- validate native Windows runtime behavior; and
- run literal full-recording cycles only if release risk warrants the roughly
  28-minute traversal per direction at 700 FPS.

## Evidence

- `hybrid_resident_20cycle.json`
  - SHA-256:
    `54e406246728fb718c1dd5d9aa79b961cb53ab88b602d0a95c69ba3a48542980`
- `hybrid_resident_20cycle.png`
  - SHA-256:
    `6550e0e690fb0662187b9130a0fb0cc6ae80b0ef7ad72d472d8977b1419d0966`
- `hybrid_resident_6cycle_sensitivity_failure.json`
  - SHA-256:
    `92aa1423f08a9df80ddc3a52981ed0f7cc7ae379c79445c6a3100f446c0db869`

The plot is generated by `tools/plot_analysis_endurance.py`. The portable
workload and plateau policy are in `src/endurance_workload.*`; the invocation
and gate semantics are frozen in
`docs/crimson_phase5o_endurance_acceptance.md`.
