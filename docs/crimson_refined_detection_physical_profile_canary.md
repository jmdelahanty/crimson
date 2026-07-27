# Crimson Refined-Detection Physical-Profile Canary

Date: 2026-07-27

Contract version: 1

Status: frozen; mounted-macOS execution pending

## Purpose

This gate compares Palette's logically identical full-duration regular and
access-aware refined-detection v1 snapshots through Crimson's production
backend-neutral refined repository. It selects a physical storage profile; it
does not change refined selection, enable a writer, or mutate Palette state.

The canary manifest is:

```text
/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/
refined_detection_storage/profile_canary/
sleepyfish_accept_all_regular_vs_access_aware_20260727_v1/
canary_manifest.json
```

Its file SHA-256 is
`8d9215aa29bf4b0787e50114e9fb429f959194ee3a7f1bdea6fd1d04ae1424b6`
and its canonical payload digest is
`2c00649c378c7a33f5621c4cd91ad787db46dcd0a512d6391ece92b383cd5609`.

## Fixed Inputs

Both candidates contain 1,188,000 camera frames, 1,187,087 refined rows,
1,188,001 offset boundaries, exact decoded values, and the complete 28-array
refined-v1 declaration tree. The regular candidate uses 1 MiB unsharded
chunks. The access-aware candidate uses 128 KiB windowed/indexed inner chunks,
1 MiB eager offset chunks, and 8 MiB outer shards.

Every process uses Crimson's production 64 MiB TensorStore cache, 70-frame
pages, 32 presentation-cache pages, four scheduler workers, and the refined
adapter's complete ten-field concurrent presentation read. Source-audit arrays
remain unopened.

## Process Matrix

Five fresh processes run per layout. Even repetitions run regular first; odd
repetitions run access-aware first. Candidates in a repetition use identical
frame selections and traversal regions. OS, SMB, and server caches are
uncontrolled and are not described as cold.

The deterministic workload vectors are:

```text
current frames: 271085, 85499, 397712, 1003450, 939795, 903492, 351953, 1141796
seek burst:     560111, 1066017, 905397, 100063, 996466, 909512, 639969, 378251
forward starts: 300020, 399980, 500010, 700000, 800030
reverse starts: 600040, 899990, 1050000, 1000020, 1099980
```

Traversal starts are globally aligned to Crimson's 70-frame page boundaries.
Each repetition uses the starts at its matching list index.

Each process:

1. validates the canary payload digest and selector-safe publication receipt;
2. opens the exact selector-ineligible refined run through benchmark opt-in;
3. validates 28 consolidated declarations, 11 exact handles, three direct
   groups, zero audit handles, and one retained offset read;
4. publishes frame zero and records detection-only readiness;
5. traverses disjoint paired 3,500-frame forward and reverse regions outside
   the first-frame chunk, in 70-frame pages at a 700 FPS, 100 ms/page deadline;
6. settles the eight frozen current-frame probes from the Phase 5O.4 workload;
7. submits the frozen eight-frame cancellation burst and permits only the last
   generation to publish; and
8. closes the buffer and scheduler while recording physical file metrics and
   total process peak RSS.

The one-page asynchronous demand lead remains enabled. No time-based
speculative read-ahead or UI-column residency is enabled.

## Frozen Gates

Correctness requires exact paired traversal digests, one offset read, 11 exact
handles, zero source-audit handles, zero stale publications, zero failed reads
or scheduler work, and at least two concurrently active refined fields.

| Evidence | Limit |
| --- | ---: |
| every process detection readiness | at most 180 s |
| access-aware median readiness regression | at most 10% and 5 s |
| current-frame cross-process p95 | at most 1 s |
| access-aware median current-frame-p95 regression | at most 10% and 250 ms |
| post-warmup deadline misses | exactly zero |
| seek settlement cross-process p95 | at most 250 ms |
| detection-process peak RSS | at most 768 MiB |
| access-aware median peak-RSS regression | at most 64 MiB |
| access-aware median total file bytes | at most 1.05 times regular |
| access-aware median traversal file bytes | at most 0.80 times regular |
| shutdown settlement | at most 2 s |

The 0.80 traversal ratio encodes Palette's required material improvement of at
least 20%. TensorStore file bytes are driver-level file-kvstore range bytes,
not SMB wire bytes.

Passing recommends the access-aware physical profile to Palette for a separate
versioned promotion decision. Crimson never sets `profile_promoted=true` and
does not modify a writer, registry, selector, or production archive.
