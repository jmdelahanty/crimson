# Batman Keypoint-v2 Crimson Gate

Date: 2026-08-05

Status: headless mounted-store and source-matched Metal GUI gates passed.

## Verdict

PASS for the selector-ineligible Batman keypoint-v2 consumer gate. This does
not activate a selector or authorize expansion to other Batman recordings.

The first open exposed an intentional Palette contract revision:
`palette.refined_keypoint.source_bindings` advanced from version 1 to version
2 to carry ordered skeleton semantics. Crimson adopted that version as an
exact, digest-validated contract while retaining version 1 as an explicit
compatibility path. It does not probe dtypes, aliases, or selectors.

## Provenance

- Crimson evidence revision: `c485b80596dc30dd1b3d311943c3761ff6ac3921`
- Skeleton-v2 consumer revision: `ea0b5bea8606e4a4f9ef8199d15c4ad2a907d1e2`
- Keypoint-only GUI gate revision:
  `c8aaf9d161d5f9c6e637dcdade5aa683ef1aa34e`
- Clean GUI capture revision:
  `5cb57bd2ab8c119daa4c66b766ed6283e06b73a2`
- Palette implementation revision:
  `9598f402e27c18b5ff2dfc390811cc0472a5eaec`
- Selected refined manifest digest:
  `acd2d14f7bbed7a468ea0fd6262633a38f16dbf3d6ea48f915c81834c476412f`
- Frozen workload canonical digest:
  `c419afa719b4d2b5c43e3fe1a0a89fe6e095c1f27024ebc14e1dcc0f63372ce2`
- Experiment file SHA-256:
  `6138f0cc90381a1474ddb69af499831b87621e26ae4b8dd894fc444395b95e14`

The run used the mounted SMB archive directly. OS, SMB client, and server
caches were not controlled, so the first process is reported separately from
the warmed distribution.

## Correctness

All five fresh processes passed.

- 139,295 camera frames, 126,214 observation rows, and five ordered keypoints.
- 61 exact typed handles and 61 consolidated array declarations were opened.
- 65 direct declarations agreed with consolidated metadata in every process.
- Fallback dtype opens and fallback metadata reads were zero.
- Raw, selected-refined, and body-frame `frame_row_offsets` were each read
  exactly once; the quality offset and all quality payload arrays remained
  unread during ordinary presentation.
- Deep full-row identity validation passed for frame indexes, crop row IDs,
  acquisition frames, instance keys, row signatures, source success, and
  body-frame bindings.
- Ordered labels and skeleton edges matched the raw, refined, and body-frame
  authorities. Heading remained sourced from the body-frame run.
- Rapid seeks produced zero stale visible frames, reads produced zero errors,
  and forward/reverse traversal produced zero post-warmup deadline misses.
- No selector, registry, archive, or Palette artifact was modified.

## Metal Visual Gate

The source-matched GUI gate used the camera video, analysis archive, explicit
crop run, and all four explicit keypoint-v2 runs at frame 1,000. Crimson held
the exact requested/presented frame for 60 stable renders before publishing
the reference marker.

- Platform: `macos-metal` on Apple Silicon arm64.
- Crop source: exact live geometry for camera frame 1,000.
- Keypoint-layer primitives: 11.
- Body-frame heading primitives: 1.
- Subject-mask and subject-shape primitives: 0, as expected for this
  keypoint-only fixture.

Visual inspection places the eye, swim-bladder, snout, tail, skeleton, and
heading marks on the fish within the 348x348 live crop. No two-pixel model
padding displacement is visible. This is a presentation smoke, while the
manifest digest and exact-schema checks remain the machine-verifiable
coordinate/provenance authority.

## Performance

Median across five fresh processes:

| Metric | Median | Worst observed |
| --- | ---: | ---: |
| Repository open | 344.61 ms | 3,810.90 ms |
| First presentation readiness | 353.29 ms | 3,984.05 ms |
| Process-first random-frame p95 | 0.157 ms | 0.285 ms |
| Warm random-frame p95 | 0.137 ms | 0.164 ms |
| Forward 70-frame page p95 | 4.36 ms | 4.61 ms |
| Reverse 70-frame page p95 | 3.65 ms | 4.04 ms |
| Final rapid-seek readiness | 0.377 ms | 0.624 ms |
| Peak RSS | 104,579,072 bytes | 104,595,456 bytes |

Each process recorded 109 physical file reads and 32,231,237 transferred
bytes through Crimson's TensorStore file metrics. The 64 MiB cache reported
zero evictions. Warm traversal transferred zero additional bytes because its
working set was already resident in the process cache.

## Evidence

- `aggregate.json` SHA-256:
  `0a2d0f929336b54fbbfd30431f461bc3c2efbbbdccf5c534cdf664629ae60273`
- `summary.svg` SHA-256:
  `0107635e0239eb12e1054b15dc2a69b4010d7e042541f0ae8ebee50c6bed9331`
- `gui_reference.json` SHA-256:
  `09a8a427eb7c6449c0f6694caeb6599d8a852f3c00732763fb0b25f442a57fed`
- `gui_reference.png` SHA-256:
  `595ee232f82597f7b7b789815f1d38ec3977e11f2937203d126adb9648cafa4f`

The aggregate embeds every per-process command, structured result, environment
record, raw metric distribution, and input evidence declaration.
