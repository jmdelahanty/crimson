# August Sleepyfish Linux overlay integration — 2026-09-21

Status: implemented and native-tested in the working tree based on
`b78b7cad`, branch `codex/linux-priority-20260916`. These changes are not
committed/pushed or present in the September 17 colleague package.

## Delivered scope

The August rolling-clip route now displays exact eye-bound keypoints, filled
subject-mask components, valid shape geometry and shape-authorized headings.
It keeps the existing canonical boxes/scores/classes and eye-angle, speed,
bout-interval and detector-response timelines. Readers are read-only: no source
archives, selectors, clips or published analysis products were changed.

Selection follows the selected v7 eye product's explicit upstream bindings,
including its complete but selector-ineligible keypoint/mask dependencies.
It does not independently pick each group's latest run. The bound raw-v2
keypoint view does not invent quality/body-frame companion products required
by the separate general keypoint-v2 API.

Raw detections and the NVIDIA bridge now retain uint64 instance keys with an
explicit validity/authority flag. Numeric zero and values above INT64_MAX are
valid keys. Raw boxes are not positionally joined to downstream observations:
their counts and upstream lineage differ. Keypoint/mask/shape associations use
the exact bound source, parent frame and validated observation keys.

Both keypoints_img and August shape positional arrays are already full-camera
continuous pixels. Shape coordinate-derivation metadata describes a transform
performed by the producer, not an instruction to translate again. Crop placement
is retained as provenance and mask placement. Unit vectors remain vectors;
heading requires body and axis validity plus finite coordinates.

Missing inference is distinct from a read failure. Camera 94 frame 2565015
has zero observations in all three products. Frame 2862000 has an observation
and keypoints but no valid mask/shape presentation. At frame 54010, that camera
has three filled components and no shape/heading primitives. None of these
cases fabricates geometry or suppresses independent valid layers.

## Storage and lifecycle

- One archive context is shared across paired readers; its existing TensorStore
  cache pool is 64 MiB.
- Repository opens and old-source retirement run on a lifecycle worker.
  UI queries read immutable cached snapshots, not synchronous payload reads.
- Keypoint, mask and shape buffers use the existing shared priority scheduler.
  Shape no longer needs a separate private scheduling path. Current-frame
  capacity is reserved; stale source/seek work is cancelled/discarded.
  Already-running filesystem/TensorStore reads are not interruptible.
- Keypoint/shape frame caches are eight frames with two-frame lookahead;
  mask cache is four frames with no speculative mask prefetch in this increment.
  Repository-level mask prefetch is also disabled for this route.
- Keypoint payload admission is at most 64 observations / 1 MiB per frame;
  shape at most 16 observations / 16 MiB; mask at most 16 observations, eight
  logical rows per read and serial component reads. Mask retained sparse-payload
  cache has a 64 MiB byte limit. Its existing strict mapping open is still eager;
  an estimated 768 MiB mapping admission checks known allocations before reading.
  This is not a hard process-RSS cap.
- GPU mask cache is limited to 64 textures and 64 MiB, with an 8 MiB single RGBA
  upload limit. Payload keys include archive/run/manifest/instance/component.
  Source changes and shutdown release textures on the GL owner thread.
- The 140 MiB consolidated metadata document still must be read/parsed for
  validation. Projection retains root authority and complete selected-run
  metadata, not unrelated product DOMs. Direct/consolidated equality and
  selected-node publication-digest checks remain enforced.
- Important correction: `masks_roi` outer shards have 2872 rows; indexed
  **inner read chunks have 8 rows × 1 channel × 384×384 uint8 = 1.125 MiB**.
  TensorStore reports 8 read rows. The 404 MiB logical shard is not the
  decompression unit.

These are bounded asynchronous loaders using the existing scheduling/cache
infrastructure and measured storage layout, not an automatically latency-tuned
policy. Strict mask mappings retain roughly 211–224 MiB across these recordings.
Opening paired products is serial: a slow mask open currently delays publishing
the otherwise independent keypoint/shape readers.

## Verification

The final native build uses the RTX A6000 workstation, CUDA 12.4, TensorRT 10.0.1.6,
GCC 13 and existing native dependencies. It does not qualify Ubuntu 22 portability.
The reviewed `release/redgui` SHA-256 is
`db043410692009c6f94fbe29876eb08d454bd809a31004fcb4227c7e760cffb4`.

- Complete native CTest: **99/99 passed**, including new selection, bound raw-v2
  keypoint, canonical session and presentation targets plus expanded detection,
  shape, mask and archive-context fixtures.
- Shared data-access, scheduler and canonical overlay lifecycle tests passed
  ten consecutive repetitions each.
- Tests cover exact parent binding, digest/schema/coordinates, uint64 identity,
  reordered keys, wrong frames/runs, duplicate keys, validity/NaNs, empty and
  failed products, bounded reads/cache, blocked storage, source replacement,
  reader exceptions, nonblocking close and independent concurrent sessions.
- All four August cameras passed integrated repository probes at 0, 53990, 54000,
  54010, a metadata-derived late boundary and 2937603. Camera 93's late probe is
  2592030; the other cameras use 2862000. Two extra camera 94 absence cases passed.
- The independent chunk-level Python oracle agrees exactly with all six
  camera 93 samples on keys, acquisition frames, validity and keypoints_img.
  It does not use Zarr/TensorStore and reads only requested indices/chunks.
  Manifest hashes are validated as canonical metadata identities, not digital
  signatures; full payload digests were not recomputed.
- Authenticated paused GPU overlay captures passed for all four cameras at 54010
  and camera 93 at 2592030, stable for 60 rendered frames. Markers require exact
  video/query/overlay frames, matching bound identities, actual keypoint drawing
  and mask/shape counts matching valid source presentation. Zero expected shape
  primitives are allowed; nonempty camera 93/95/96 cases exercise shape drawing.
  Final lifecycle/eligibility review fixes were followed by successful repeats
  of camera 93 and 94 captures at 54010.
- All four rolling-clip playback smokes passed 53990:54010.
- June GoodCopBadCop playback passed 0:300. May materialized-clip control passed
  53990:54010 with its explicit clip index. These use separate smoke windows.
- The user also visually observed overlays in the running smoke application.

Example local evidence (temporary paths, not included in a clone):

- `/tmp/crimson-overlay-reviewed-ctest.log`
- `/tmp/crimson-overlay-reviewed-repeat.log`
- `/tmp/crimson-august93-overlays-verified.json`
- `/tmp/crimson-2010094-overlays-verified.json` (and 2010095/96)
- `/tmp/crimson-august94-absence-probe.json`
- `/tmp/crimson-overlay-independent-oracle.json`
- `/tmp/crimson-canonical-overlay-smoke.On82Z6/ready.json` (camera 93, 54010)
- `/tmp/crimson-canonical-overlay-smoke.rHcFa5/ready.json` (camera 94 absent shape)
- `/tmp/crimson-canonical-overlay-smoke.6ukyNk/ready.json` (camera 95)
- `/tmp/crimson-canonical-overlay-smoke.zelVdr/ready.json` (camera 96)
- `/tmp/crimson-canonical-overlay-smoke.6AlGDj/ready.json` (camera 93 late boundary)
- `/tmp/crimson-overlay-final-june-control.log`
- `/tmp/crimson-overlay-final-may-control.log`

Observed repository-only process peaks were 704–751 MiB; camera 93 dropped from
about 1.4 GiB before metadata projection to 751 MiB afterward. Opens took 4.38–4.87 s.
Individual sampled frame requests ranged from 14 ms to 3.95 s across all cameras.
These runs had warmed/unknown OS/storage caches and some concurrent build work;
they are observations, not cold-cache latency acceptance or a 30 fps mask promise.
Logical payload counters are not physical filesystem byte measurements.

## Reproducing checks and running locally

From the active Linux checkout:

```bash
cmake --build build/linux-trt10-cuda12.4-release --parallel 4
ctest --test-dir build/linux-trt10-cuda12.4-release --output-on-failure

build/linux-trt10-cuda12.4-release/canonical_overlay_repository_probe \
  /path/to/analysis.zarr 0 53990 54000 54010

CRIMSON_PLAYBACK_SMOKE_DISPLAY=:1 \
CRIMSON_PLAYBACK_SMOKE_XAUTHORITY=/path/to/current/Xauthority \
  bash scripts/gui_smoke_canonical_overlays.sh /path/to/analysis.zarr 54010

./release/redgui --zarr \
  /misc/public/forGinny/recordings/2026_08_06_19_13_35_cam2010093/zarr/2026_08_06_19_13_35_cam2010093_analysis.zarr
```

The native command needs a current authenticated DISPLAY/XAUTHORITY, including
inside tmux. The existing packaged `dist/...20260917...` executable is unchanged;
running it will not include these new overlays. The oracle tool is
`tools/august_keypoint_source_oracle.py`; it uses standard Python and system
`libzstd` through `ctypes`, without a Python `zstandard` module dependency.

## Explicit remaining work

- User-deferred: mask contour outlines and swim-bout/core shading behind the
  speed trace. The filled masks/shape lines must not be described as contours.
- Eye-axis/gaze/angle-arc geometry, new tail traces, editing/export and arbitrary
  track selection are not part of this increment.
- Progressive per-product open readiness, cold-storage tuning, continuous
  overlay endurance, repeated GUI reopen/reverse-seek qualification, detailed
  physical-I/O/GPU-memory accounting and A4000/driver 535 qualification remain.
  Existing video playback smokes do not prove sustained mask availability.
- Independent full mask/shape payload-oracle coverage is narrower than the
  keypoint oracle; shape/mask correctness has fixture, source-authority,
  per-frame identity and bounded GUI evidence, not whole-recording validation.
- Commit/review checkpoint and a fresh sealed-builder Ubuntu 22 package are next.
  Do not reuse the native source-root release output as an Ubuntu 22 artifact.
  Do not overwrite the shared September 17 package until a new staged package
  has passed its runtime and GPU checks.
