# TensorStore `keypoints_img` PRFS read benchmark (2026-07-14)

## Decision

Indexed sharding is safe for Crimson's `keypoints_img` reads with a 1,024-row
inner chunk. TensorStore range-read the shard index and requested inner chunk;
it did not read the enclosing 32K, 131K, or 262K-row shard. All four decoded
fixtures had the same SHA-256 digest as the production source.

Use **131,072 rows per outer shard with 1,024-row inner chunks** as the general
tabular default. The 262K layout was marginally fastest to publish and read on
the compute node, but the benefit over 131K was small (43 ms during full
publication and generally less than 0.2 ms at median read latency), while a
maintenance rewrite touches twice as much uncompressed tabular data (about
20 MiB rather than 10 MiB for `float64[N,5,2]`). Use 262K for immutable
snapshots where minimizing object count is more important than later rewrites.

Do not select 32K as the general default from these results. It offers smaller
maintenance rewrites, but publication was 42% slower than 131K and its read
latency was not better.

## Source and layouts

The exact production source was:

```text
/groups/johnson/johnsonlab/jeremy/recordings/sleepyfish_2026_05_05_17_45_30_cam2010095/zarr/sleepyfish_2026_05_05_17_45_30_cam2010095_analysis.zarr/refined_keypoints_runs/refined_keypoints_sleepyfish_kp_allclips_20260708_01/keypoints_img
```

It contains 1,169,010 rows with shape `[1169010,5,2]` and dtype `float64`.
The inner chunk was 1,024 rows in every layout.

| Layout | Outer shard | Full publication | Decoded digest |
|---|---:|---:|---|
| ordinary | none | 23.501 s | `ca4ed7c9…9798bc` |
| sharded | 32,768 rows | 2.350 s | `ca4ed7c9…9798bc` |
| sharded | 131,072 rows | 1.652 s | `ca4ed7c9…9798bc` |
| sharded | 262,144 rows | 1.609 s | `ca4ed7c9…9798bc` |

## Method

Each host ran three repetitions, reversing layout order on alternating
repetitions. Every workload ran in a fresh process after best-effort
`POSIX_FADV_DONTNEED`, followed by an identical warm pass in the same process.
The workloads were:

- 256 random single-row reads;
- 64 random 1,024-row reads;
- 128 shuffled inner chunks, read twice;
- a complete sequential scan in 1,024-row requests.

The harness recorded per-operation p50/p95/p99/max latency, workload wall time,
maximum RSS, `/proc/self/io` physical bytes, `rchar`, and read syscall count.
PRFS/server caches cannot be forcibly evicted, so physical counters and
"fresh" results are observational rather than a controlled storage cold start.

The workstation run used `delahantyj-ws1`. Compute measurements ran only in
LSF allocation 153099238 on `h07u29`. The workstation-built benchmark required
an Ubuntu 24.04 Apptainer runtime on the RHEL 9 compute node; the same executable
and fixtures were used on both hosts.

## Compute-node medians

These are the median fresh-process results across three repetitions.

| Layout | random row wall / p50 / p95 | random 1024 wall / p50 / p95 | full scan wall / p50 / p95 |
|---|---:|---:|---:|
| ordinary | 0.457 s / 1.772 / 2.294 ms | 0.117 s / 1.797 / 2.250 ms | 2.019 s / 1.693 / 2.211 ms |
| 32K shard | 0.439 s / 1.664 / 2.271 ms | 0.122 s / 1.854 / 2.443 ms | 1.563 s / 1.322 / 1.683 ms |
| 131K shard | 0.430 s / 1.626 / 2.130 ms | 0.115 s / 1.742 / 2.379 ms | 1.569 s / 1.339 / 1.656 ms |
| 262K shard | 0.422 s / 1.495 / 1.983 ms | 0.104 s / 1.556 / 1.955 ms | 1.517 s / 1.295 / 1.589 ms |

Warm repeated reads were similarly close. For example, random 1,024-row p50
was 1.194 ms ordinary and 1.360, 1.365, and 1.292 ms for 32K, 131K, and 262K.

## Read amplification evidence

For 256 random single-row reads, logical payload was only 20 KiB, but every
layout correctly had to decompress an inner 1,024-row chunk. The median
physical/logical ratios were 390x ordinary and 484x, 465x, and 455x for the
three sharded layouts. Those ratios correspond to roughly 8-10 MiB total, not
the 640 MiB, 2.5 GiB, or 5 GiB that enclosing-shard reads would imply.

The read syscall counts make the mechanism visible: ordinary random rows used
about 259 reads for 256 operations, while sharded layouts used about 515. The
extra read is the indexed-shard lookup; physical bytes stayed near one
compressed inner chunk plus index overhead. Increasing the outer shard from
32K to 262K did not increase physical bytes per request.

## Workstation caveat

Warm workstation reads were stable but slower than compute: random 1,024-row
p50 was 3.039 ms ordinary and 5.678, 5.869, and 5.620 ms sharded. Best-effort
cold sharded runs also encountered intermittent ~1-second PRFS request stalls,
which drove p95 near one second. The same objects showed 2-3 ms p95 on the
compute node, so this was not enclosing-shard transfer. It appears to be
workstation/PRFS request latency amplified by the extra index request.

Crimson is a long-lived reader and benefits from TensorStore and filesystem
caches, but first-use UI latency on workstation-mounted PRFS should remain a
smoke-test item after changing a production default.

## Retained evidence

The retained benchmark root is:

```text
/groups/johnson/johnsonlab/jeremy/crimson-tensorstore-keypoint-read-benchmark-20260714
```

It retains `manifest.json`, per-host `raw.jsonl`, `results.csv`, `summary.md`,
and LSF logs. The generated array payloads and staged runtime artifacts are
temporary and must be deleted after validation.
