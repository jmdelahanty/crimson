# Crop Geometry v2 Read Benchmark

Date: 2026-07-29

Status: **integration/read-harness checkpoint passed; not physical-profile
promotion evidence**

## Scope

This checkpoint exercises Palette's selector-ineligible coordinate-catalog crop
v2 canary through Crimson's real TensorStore file-kvstore path on the mounted
macOS share. It validates the exact 13-array geometry-only contract, retained
CSR offsets, the five-field presentation working set, concurrent range reads,
cancellation, and physical I/O/RSS telemetry. It does not benchmark crop
pixels and it does not promote a storage profile.

Fixture:

```text
/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/coordinate_catalog/integration/20260128_coordinate_catalog_crimson_20260728_v1/crops.zarr
```

Run:

```text
crop_geometry_coordinate_catalog_crimson_20260728_v2
```

Immutable identity:

- Crimson implementation: `e972cef941ada06f532213991bdc9d1a389f642b`
- Crimson worktree at benchmark build: clean
- Palette handoff SHA-256:
  `21ccf119dfe7910e6c2cce7b027a9318e7b3c8cec4702363fbc3d4460775c4d3`
- Crop manifest payload digest:
  `a4f42a823b1ca81cc69936fe0a374a59b75f42a8781a159a6a56f02e25a463f6`
- Structured result SHA-256:
  `dedbfa6bcd75062286f4583d53ca9cc4a692f176451a9c911f0b2c1829529615`

## Result

The fixture contains 23,287 camera frames and 22,926 crop rows. Crimson:

- validated all 13 exact paths, ranks, shapes, and dtypes without probing;
- proved direct/consolidated equivalence for every declaration;
- opened no `roi_images` or `roi_images_delta` path;
- read `frame_row_offsets` exactly once and retained 186,304 bytes;
- validated all offsets against the complete `frame_indices` array;
- issued the five UI fields concurrently for each requested range;
- discarded all 32 deliberately superseded reads with zero stale publication;
- traversed the recording in 333 70-frame windows; and
- decoded all 13 logical arrays without rereading the offsets.

| Measurement | Result |
| --- | ---: |
| Process start through first geometry page | 1,250.88 ms |
| Direct/consolidated metadata validation | 981.68 ms |
| One-time offset read | 75.09 ms |
| First five-field geometry page | 188.99 ms |
| Random-frame first-pass page p95 | 0.334 ms |
| Random-frame repeat page p95 | 0.129 ms |
| 70-frame sequential-window page p95 | 0.105 ms |
| Complete sequential traversal | 27.31 ms |
| Cancellation p95 | 0.133 ms |
| Bytes read after cancellation | 0 |
| Complete 13-array decode | 765.01 ms |
| Peak RSS | 20,414,464 bytes |

The process-first cache condition is explicitly uncontrolled: macOS, SMB, and
server caches were not flushed. The readiness boundary also deliberately
includes 13 direct `zarr.json` reads required by this diagnostic to compare
them with consolidated metadata. Production consumption should use the already
validated consolidated declarations and therefore need not repeat those direct
reads.

The first page populated the small fixture's UI chunks. The subsequent random
and sequential passes consequently recorded TensorStore cache hits and no file
reads. This proves cache reuse and offset lifetime behavior for the integration
fixture; it is not evidence about a full-duration storage profile or sustained
cache pressure.

The real canary has 361 empty frames but no multi-row frame. The benchmark's
CTest self-test separately exercises counts `[2, 0, 1, 3]`, including repeated
offsets and a three-row frame, and rejects nonmonotonic offsets.

## Reproduction

```bash
cmake --build --preset build-macos-arm64-release \
  --target crop_geometry_v2_read_benchmark

build/macos-arm64-release/crop_geometry_v2_read_benchmark \
  --store /Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/coordinate_catalog/integration/20260128_coordinate_catalog_crimson_20260728_v1/crops.zarr \
  --run crop_geometry_coordinate_catalog_crimson_20260728_v2 \
  --cache-bytes 67108864 \
  --random-seeks 128 \
  --cancellation-seeks 32 \
  --output docs/diagnostics/crop_geometry_v2_read_benchmark_2026-07-29/result.json
```

The same target also compiled and passed `--self-test` in the isolated Linux
NVIDIA checkout with CUDA 12.4 and the maintained TensorStore build. The
benchmark contract and implementation are independent of Metal, CUDA, OpenGL,
and ImGui.

## Next Gate

Palette can publish its persistent benchmark-only geometry candidate and give
Crimson its publication receipt. Crimson should rerun this exact workload
without changing the access sequence, then bind the result to that candidate's
manifest digest and receipt. Only a representative/full-duration candidate can
provide physical-profile promotion evidence.
