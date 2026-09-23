# Crimson Phase 5O.4 Full-Archive Integration Result

Date: 2026-07-26

Status: passed for the 2,048-frame integration tier; not promotion evidence

## Scope

This checkpoint exercises the paired Palette full-analysis fixtures through
Crimson's real asynchronous repository loader and canonical detection paging
path. It validates application compatibility over frames `[0, 2048)`. It does
not validate full-duration startup, long-run cache pressure, object count,
physical read amplification, deadline behavior at scale, or a production
storage-profile promotion.

## Identity

- Crimson base revision: `34ff3c38229450b4e6bbe9b16fed99e9ca966197`
  plus the uncommitted Phase 5O.4 runner and existing port work
- Crimson fixture-contract revision: `dadd9d779f0737c9643f15e3831a7c514bf99665`
- Palette fixture-producing revision:
  `4bf96646f873517bbcf921f78af151b41ce0ed78`
- Palette branch head reported at publication: `7305224e`
- macOS: `26.3.1 (25D2128)`, Apple Silicon `arm64`
- storage: `//delahantyj@prfs.hhmi.org/johnsonlab` mounted as SMB at
  `/Volumes/johnsonlab`
- TensorStore cache pool: `67,108,864` bytes per archive
- fixture run: `crimson_storage_fixture_sleepyfish_cam2010095_v1`

Manifest SHA-256 values:

| Manifest | SHA-256 |
| --- | --- |
| pair | `57f3e56c0e5adbbcdf6bd05300e02f7179576613336554e89c6acf5de0ca1662` |
| regular | `810d2090a579cad85ffc0afdc37bcb7f6c1ac70f8529ea0f74e7776c9ebde1fd` |
| hybrid | `f43cd05d8fe1a2617255c0cc8b220887728df35129210b29da9281d5206d3f26` |

## Invocation

The runner was built with:

```bash
cmake --build --preset build-macos-arm64-release \
  --target canonical_detection_full_archive_integration_gate -j 6
```

Each candidate then ran in a separate fresh process:

```bash
build/macos-arm64-release/canonical_detection_full_archive_integration_gate \
  "$ARCHIVE" \
  crimson_storage_fixture_sleepyfish_cam2010095_v1 \
  /Volumes/johnsonlab/jeremy/recordings/sleepyfish_2026_05_05_17_45_30_cam2010095/cams/Cam2010095_sleepyfish_2026_05_05_17_45_30_cam2010095.mp4 \
  "$LABEL" \
  "$EVIDENCE_JSON"
```

`$ARCHIVE` was respectively:

```text
/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/canonical_detection_storage/full_analysis/sleepyfish_cam2010095_integration_2048_v1/regular.zarr
/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/canonical_detection_storage/full_analysis/sleepyfish_cam2010095_integration_2048_v1/hybrid.zarr
```

## Result

| Measurement | Regular | Hybrid |
| --- | ---: | ---: |
| required-product loading | 4,323.4 ms | 5,369.4 ms |
| all first presentations | 1,812.5 ms | 1,659.6 ms |
| canonical open | 264.5 ms | 565.9 ms |
| retained offsets read | 124.3 ms | 157.2 ms |
| full 2,048-frame detection traversal | 162.5 ms | 178.5 ms |
| seek-burst settlement | 0.294 ms | 0.206 ms |
| peak concurrent detection fields | 3 | 6 |
| cancelled stale requests | 14 | 14 |
| stale publications | 0 | 0 |

Both candidates passed. Both produced:

- logical FNV-1a digest `e7665c85d8913ff5`;
- 2,048 resolved detections and 2,048 shared overlay boxes;
- one root metadata read and nine consolidated declarations;
- four exact typed handle opens and zero fallback opens;
- one offsets read retaining 16,392 bytes;
- ready frame-zero keypoints, masks, subject shape, eye geometry, and crop
  geometry;
- ready motion, eye-angle, and tail-kinematics windows; and
- zero scheduler failures or work exceptions.

The fixture's persisted absolute video path uses the cluster `/groups` prefix.
Automatic discovery therefore cannot resolve it from the external benchmark
root on macOS. The contract-specified explicit `/Volumes/johnsonlab` video path
was present and used by the gate. A future portable fixture locator may remove
that override, but it is not required by this storage compatibility checkpoint.

## Interpretation

The pair proves that Crimson accepts both codecs/layout declarations and can
initialize, present, page, adapt, and cancel all required fixture products
without logical divergence. It does not establish a meaningful layout timing
comparison: at 2,048 rows, each canonical array occupies one inner chunk in
both archives. OS, SMB, and server caches were uncontrolled, and only one
retained process per condition is reported.

## Metal Playback Smoke

Both archives also passed a real Metal/GLFW playback smoke over frames
`[0, 300]` with the explicit detection run, live geometry crop, and required
analysis timelines enabled.

| Measurement | Regular | Hybrid |
| --- | ---: | ---: |
| presented terminal frame | 300 | 300 |
| decoded frames | 306 | 306 |
| skipped source frames | 0 | 0 |
| late presentations | 0 | 0 |
| maximum lag | 0 frames | 0 frames |
| canonical maximum resolve | 111.9 ms | 89.4 ms |
| video startup | 672.9 ms | 649.6 ms |
| elapsed playback smoke | 10.174 s | 10.220 s |
| peak process RSS | 771.6 MiB | 778.8 MiB |

Both smokes reached required-product Ready before playback. Canonical
detections, masks, keypoints, shape, eye geometry, geometry crop, motion,
eye-angle, and tail-kinematics presentations were active; scheduler failures,
failed canonical pages, and late frame presentations remained zero.

The next gate is a full-duration five-repetition Stage 1 pair with physical
file-range bytes, peak RSS, first-overlay timing, and deadline measurements.
