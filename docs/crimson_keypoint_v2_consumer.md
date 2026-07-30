# Crimson Keypoint V2 Consumer

Date: 2026-07-29

Status: selector-ineligible raw/refined interoperability gate passed on macOS
and isolated Linux; production selection and long-recording physical-profile
promotion remain open

## Boundary

Crimson now has one backend-neutral, read-only consumer for Palette's exact:

- raw keypoint observations v2;
- keypoint quality v1;
- refined keypoint observations v2; and
- body frame v1.

`KeypointV2Repository` implements the existing `KeypointOverlayRepository`
presentation interface. TensorStore, Zarr metadata, and exact schema validation
remain below that interface; the scene and Metal/OpenGL presentation paths do
not depend on the storage backend.

Legacy mutable keypoint runs remain behind the existing compatibility adapter.
The v2 path never probes alternative dtypes or aliases and is available to the
application only through explicit selector-ineligible benchmark selections.

## Read Contract

The consumer:

- validates the run-manifest envelope and digest for every selected stage;
- validates direct metadata against inline consolidated metadata;
- requires exactly 15 raw, 13 quality, 23 refined, and 10 body-frame array
  declarations with their compile-time dtypes and ranks;
- rejects missing, additional, or incompatible declarations;
- reads and retains each selected `frame_row_offsets` vector exactly once;
- resolves the complete half-open row range
  `[offsets[frame], offsets[frame + 1])`, including empty and multi-row frames;
- preserves `instance_key` as observation/edit identity through scene
  primitives without treating it as longitudinal subject identity;
- checks selected/body instance keys, frames, source rows, and row signatures on
  every presented page;
- leaves quality metric/flag payloads unread during ordinary playback; and
- obtains heading only from the bound body-frame stage. Raw embedded heading is
  forbidden by the raw-v2 manifest contract.

Page columns are issued together as TensorStore futures, then collected and
validated as one immutable result. Raw presentation issues 14 required
raw/body columns per batch. Refined presentation issues 22 required
refined/body columns per batch. TensorStore owns the bounded I/O execution;
Crimson does not create one OS thread per column.

## Application Selection

The asynchronous analysis loader accepts four exact artifact selections. The
macOS shell exposes these only through benchmark arguments of the form:

```text
--benchmark-keypoint-v2-raw ARCHIVE RUN MANIFEST_DIGEST
--benchmark-keypoint-v2-quality ARCHIVE RUN MANIFEST_DIGEST
--benchmark-keypoint-v2-refined ARCHIVE RUN MANIFEST_DIGEST
--benchmark-keypoint-v2-body-frame ARCHIVE RUN MANIFEST_DIGEST
```

Raw, quality, and body frame are required. Refined is optional, but an explicit
refined selection must be complete and valid; it never silently falls back to
raw. An explicit v2 selection also makes keypoint readiness required by the
session-open transaction. Without these arguments, Crimson uses the unchanged
legacy compatibility adapter.

## Palette Handoffs

Raw/quality/body interoperability fixture:

```text
/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/keypoint_storage/
integration/20260128_cropv2_keypoint_v2_20260729_v4
```

- Palette commit: `5037e90c677b83b3e24f7833898b014502d9e443`
- handoff SHA-256:
  `cd33ac60e2f72f614a0ea5f2583d08229b9dee22d2ddb2692a56a284f4f2d8c2`

Refined/body interoperability fixture:

```text
/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/keypoint_storage/
integration/20260128_cropv2_keypoint_refined_v2_20260729_v2
```

- Palette commit: `11fc33db074ff64ba5c797d0b3e7b84f06f3c39d`
- handoff SHA-256:
  `d1c0e27303b715c95c645f077406906f691be2f9d86a74307425bb55465606b1`

Both fixtures contain 23,287 frames, 22,926 observations, three keypoints, and
361 empty frames. The refined fixture contains one correction, one rejection,
and one recovered failed pose. It contains no multi-observation frame, so the
portable deterministic `[2, 0, 1, 3]` offset test remains part of the gate.

## Mounted Result

The final macOS headless mounted run passed both repositories:

| Metric | Raw | Refined |
| --- | ---: | ---: |
| Exact consolidated declarations | 38 | 61 |
| Direct metadata reads | 41 | 65 |
| Exact handle opens | 38 | 61 |
| Fallback metadata/dtype opens | 0 | 0 |
| Retained offset bytes | 372,608 | 558,912 |
| Representative frame probes | 128 | 131 |
| Mapped / missing probes | 123 / 5 | 126 / 5 |
| Frame-probe time | 164.8 ms | 12.1 ms |
| Maximum columns per batch | 14 | 22 |
| Ordinary quality payload reads | 0 | 0 |

The raw and refined offset vectors were each read once; the quality offset was
not read until the explicit audit. The audit validated quality/source row
identity without changing ordinary playback behavior. All refined decision
keys, review/reason codes, success transitions, and edit flags survived through
the shared presentation model.

The macOS GUI smoke used the normal asynchronous loader, shared data scheduler,
keypoint buffer, scene adapter, and Metal renderer. It passed frames 0 through
30 with zero late presentations. Current-frame queue wait stayed below 0.1 ms;
the maximum service time was 311.9 ms on the mounted path. The exact January
source video was not copied with Palette's headless fixture, so a different
4512-by-4512 camera stream was used only to validate GUI/Metal mechanics. It is
not visual scientific-alignment evidence.

The complete macOS release preset passed 65 of 65 tests. The portable contract
coverage includes empty and multiple-observation frames, malformed offsets,
duplicate keys, stable identity on every scene primitive, and body-frame-only
heading.

An isolated Linux release build at the same Crimson implementation revision
also built `keypoint_v2_contract_tests`, `keypoint_v2_canary_gate`, and the full
CUDA/NVIDIA `redgui` target with CUDA architectures `80;86`. The portable
contract test and the same raw/refined mounted-store gate passed. Linux opened
the raw repository in 228.1 ms and the refined repository in 291.1 ms; the
representative frame probes took 29.9 ms and 43.0 ms, respectively. Exact-open,
offset-retention, quality-laziness, and refined-decision results matched macOS.

Those Linux timings are not a platform-performance comparison. `ws1` read
`/groups` through the on-site server network, while the Mac read
`/Volumes/johnsonlab` over Wi-Fi, VPN, and its mounted-filesystem path. Only
results collected on the same host, storage path, cache state, and network tier
may be compared as storage-performance evidence.

The Linux build also verified source ownership: keypoint-v2 contract and
TensorStore implementation files are compiled once by
`crimson_tensorstore_repositories` and excluded from the NVIDIA executable's
legacy source glob.

## Remaining Gates

This checkpoint does not:

- select a raw or refined v2 run in production;
- promote a keypoint storage profile for long recordings;
- claim a source-matched visual smoke for the headless fixture;
- implement quality inspection UI or any edit/write lifecycle; or
- replace legacy adapters for archives that do not satisfy v2 exactly.

A larger immutable fixture is needed only for long-duration cache pressure,
transfer, and physical-layout promotion. It is not needed to repeat the logical
interoperability decision established here.
