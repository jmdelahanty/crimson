# Crimson Phase 5M.4 Cross-Platform Acceptance

Date: 2026-07-18

Status: complete. Phase 5M.4 and the full Phase 5M read-only polar extraction
gate are accepted.

Lifecycle: **archived acceptance record**. Preserve its evidence under
`docs/reference/phase5m/` when the document is moved.

## Accepted Production Source

The GoodCopBadCop archive changed after the Phase 5M.3 checkpoint, so this
acceptance does not reuse the older selected-run provenance. Both adapters
selected and compared the current complete source:

- group: `analysis/chaser_distance_runs`;
- run: `chaser_distance_v1_20260718`;
- component: `egocentric_bearing_v1_20260718`;
- shape: 140,035 camera-frame rows by two chasers;
- distance unit: `mm`; and
- dataset maximum valid distance: 76.72962188720703 mm.

The frozen descriptor and exact-frame samples are in
`docs/reference/phase5m/legacy_polar_goodcopbadcop.json`. Frames 0, 56, and
140034 are valid-empty. Frames 1024 and 7024 contain two valid points. The
legacy adapter and TensorStore adapter comparison passed all five frames with
the same run/component identity, exact frame identity, distances, bearings,
colors, color provenance, and radial maximum.

## Contract Coverage

Synthetic characterization and portable tests cover:

- unavailable, unsupported metadata, missing exact frame, valid-empty, ready,
  and failed availability states;
- supported and rejected coordinate/angle conventions;
- non-finite, negative, out-of-range, and otherwise invalid point values;
- duplicate camera-frame rows, sparse and missing frame IDs, and exact-frame
  enforcement;
- stimulus-protocol, component-summary, and fixed-palette color precedence;
  and
- global maximum, display padding, fallback, and degenerate radial scales.

The contract preserves the legacy first-row-wins duplicate policy without
collapsing a missing frame into a valid-empty frame. The shared scene signature
normalizes geometry to the inset box, making scene semantics independent of
the surrounding camera viewport while retaining ordered primitives, text,
colors, values, and anchors.

## Controlled Cross-Platform Capture

The maintained NVIDIA/OpenGL client and native Mac/Metal client were captured
at camera frame 1024 in a 1920x1080 logical framebuffer. Both rendered the same
156x213 inset and 140x140 graph with two points, seven ordered vector
primitives, and seven ordered text annotations. Their surrounding fitted camera
viewports differ, 430x298 on Linux and 338x338 on Mac, so the acceptance
contract compares normalized source geometry rather than absolute window
placement.

`tools/phase5m_polar_compare.py` passed all 20 checks in
`docs/reference/phase5m/acceptance_report.json`:

- canonical semantic signatures and production descriptors are identical;
- maximum scientific descriptor delta is 0;
- maximum normalized scene geometry/value delta is
  2.842170943040401e-14 pixels;
- minimum opaque-marker vector coverage within one pixel is 1.0 against a
  required 0.995;
- maximum vector outlier distance is one pixel against an allowed two pixels;
- maximum opaque marker channel delta is 0 against an allowed 3/255; and
- maximum raster marker-centroid delta is 0.2613 pixels against an allowed
  0.5 pixels.

Font glyph pixels are compared by shared text content, order, color, and scene
anchors because OpenGL and Metal use different font rasterizers. Translucent
background/grid pixels are compared by the canonical scene semantics because
they blend over differently fitted camera pixels. The polar inset has no
binary scientific mask, so the Phase 5 mask-IoU threshold is not applicable.
These exclusions are declared in the acceptance contract rather than applied
implicitly by the comparator.

## Builds and Production Smokes

The final macOS arm64 Release suite passed 43/43 CTest tests. Its production
frame 1024:1324 video smoke reached exact frame 1324. The polar path presented
23 scenes and 46 points, with zero failed resolves and zero runtime failures:

```text
[AppleChaserPolar] presentations=23 points=46 requests=243 cache_hits=23 ready=17 empty=0 missing=0 failed=0 discarded=10 source_points=34 published_points=34 peak_cached=9 peak_pending=9 max_resolve_ms=1435.4 runtime_failed=0 error=
[AppleVideoSmoke] PASS start=1024 end=1324 requested=1324 presented=1324 decoded=208 peak_buffer=6 repeats=184 skipped_source_frames=94 late_presentations=156 max_lag_frames=150.0 catchup_discarded_frames=51 catchup_seeks=0 pts_error_frames=+0.000 startup_ms=376.4 seek_ms=576.3 next_drawable_max_ms=11.7 command_wait_max_ms=23.6 elapsed_s=6.508 memory_mib=577.5 peak_memory_mib=585.8 thermal=nominal subject_mask_presentations=9 subject_mask_resolved=15
```

The isolated Linux/NVIDIA build compiled `redgui` and passed 32/32 CTest
tests. Its authenticated production smoke passed the same interval:

```text
[PlaybackSmoke] PASS start_frame=1024 end_frame=1324 presented_frame=1324 presented_slot=24 view_idx=0 presented_count=349 elapsed_s=2.99145
```

Generated macOS and isolated Linux compile-command databases contain neither
`-Ofast` nor `-ffast-math`. Repository OpenCV helpers explicitly disable
`ENABLE_FAST_MATH` and `CUDA_FAST_MATH`, while repository-controlled
TensorStore builds remove bundled dav1d's upstream `-ffast-math` option. This
keeps finite-value checks and scientific comparisons on normal IEEE semantics.

The direct `zarr_loader.h` include policy remains at its 28/28 allowed-file
baseline. Shared polar scene code and the Mac application do not consume a
concrete `ZarrDetectionLoader` polar type. Remaining maintained UI includes
belong to unmigrated responsibilities recorded in the migration ledger.

## Read-Only Proof

The production `analysis/chaser_distance_runs` subtree was fingerprinted
before adapter comparison, captures, and production smokes, then fingerprinted
again afterward. All 10,736 files retained identical paths, sizes,
modification times, and SHA-256 hashes. The compact evidence is recorded in
`docs/reference/phase5m/nonmutation_summary.json`.

No group creation, attribute update, write repository, provisional schema,
or unrelated loader migration was introduced. Windows runtime capture remains
deferred as previously agreed; the maintained compatibility adapter is shared
source, and the Phase 5M gate specifically requires and now has isolated
NVIDIA plus Mac runtime acceptance.

## Gate Closure

Phase 5M is complete: both adapters agree on accepted synthetic and production
data; conventions and all availability states are explicit; missing and empty
remain distinct; the scene is storage/platform neutral; maintained ImGui and
Mac Metal consume the same semantics within declared tolerances; builds,
deterministic tests, and production smokes pass; and the archive is unchanged.

The next roadmap checkpoint is Phase 5N, applying the same incremental
read-only extraction pattern to remaining stimulus event and step-direction
camera overlays.
