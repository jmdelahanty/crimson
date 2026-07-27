# Crimson Phase 5M.2 Polar Storage Adapters

Date: 2026-07-17

Status: complete. This checkpoint implements and compares the two read-only
storage paths behind the Phase 5M.1 portable polar contract. Shared scene and
renderer integration remain Phase 5M.3 work.

## Implemented Boundary

The native repository is declared in
`src/zarr/tensorstore_chaser_distance_polar_repository.h`. It opens an existing
archive through `ArchiveContext`, selects the characterized run and component,
and returns `ChaserDistancePolarDescriptor` and exact-camera-frame samples. It
uses `OpenMode::open` and `ReadWriteMode::read` only. Production code contains
no create, write, attribute-update, or group-creation path.

The maintained compatibility repository is declared in
`src/zarr/chaser_distance_polar_legacy_repository.h`. It adapts an already
loaded `ZarrDetectionLoader` without taking ownership; the loader must outlive
the adapter. The maintained loader now exposes the otherwise-lost selection,
color, exact-row, and radial-maximum provenance needed to construct the same
portable result. Its eager array storage remains compatibility behavior and is
not copied into the portable contract.

Both repositories preserve:

- `latest_complete`, `latest_completed`, `latest_success`, then `latest`
  selection order;
- lexicographically last complete run/component fallback;
- component frame/chaser identity arrays with the characterized run-level
  fallback;
- first-row-wins behavior for duplicate camera frame IDs;
- exact missing-frame versus valid-empty-frame identity;
- stimulus protocol, component summary, then fixed palette color precedence;
- dataset-wide maximum positive finite valid distance and 5 percent headroom;
  and
- sparse nonnegative chaser identities independent of matrix column count.

## Bounded Native Access

The TensorStore repository keeps only frame IDs, the first-row lookup map,
chaser IDs, resolved colors, and open read-only stores resident. Opening scans
distance and validity matrices in a configurable bounded number of rows to
derive the dataset-wide radial maximum. Resolving an exact frame reads one row
from bearing, distance, and validity matrices. Metrics expose index rows,
radial scan rows, exact rows, matrix operations, maximum rows per read, and
resident identity counts.

`ChaserDistancePolarBuffer` adds one worker thread, bounded lookahead, bounded
sample caching, cache-hit and availability metrics, and generation-based
discard after seeks or discontinuities. It never substitutes a neighboring
camera frame.

The repository accepts the maintained loader's characterized integer identity
representations with saturating conversion to the portable signed types. This
includes the production `uint8` chaser-index array. Bearing and distance
matrices accept `float32` or `float64`; validity remains boolean. Synthetic
fixtures use the production `uint8` chaser representation so this behavior is
covered on both platforms.

## Read-Only and Numerical Policy

The native test snapshots every file, byte, size, and modification time in its
synthetic archive before and after descriptor and frame reads. The production
GoodCopBadCop comparison fingerprints the polar and stimulus metadata before
and after. Both snapshots are unchanged.

Crimson and the repository-provided OpenCV build helpers no longer enable
fast-math. Finite and range checks in the polar contract and repositories are
therefore compiled under ordinary Release floating-point semantics. Bundled
`dav1d` source targets also have their upstream fast-math option removed.
Externally supplied prebuilt dependencies retain independent build provenance.

## Agreement Tests

`tools/chaser_distance_polar_tensorstore_repository_tests.cpp` covers:

- selected and requested run/component provenance;
- compatibility fallback and unsafe requested names;
- production coordinate, angle, integer, float, and boolean representations;
- duplicate, missing, empty, ready, invalid, and non-finite samples;
- protocol, summary, and fallback colors;
- sparse chaser identity and radial scaling;
- bounded scan and exact-row metrics; and
- absence, unsupported metadata, invalid options, and non-mutation.

`tools/chaser_distance_polar_buffer_tests.cpp` covers all six availability
states, bounds, cache hits, metrics, and discontinuity discard without a GUI.
The maintained `chaser_distance_polar_legacy_probe` retains its original
characterization assertions and additionally compares both portable
descriptors and every sample field on the same synthetic archives.

The production comparison passed for camera frames 0, 56, 1024, 7024, and
140034:

```text
chaser_distance_polar_adapter_compare: PASS
run=goodcopbadcop_chaser_distance_v1_20260617
component=track_offline_goodcopbadcop_tk_hyst4_low2_latch_s005_id_0_smoothed
rows=140035 chasers=2 max_mm=76.7296 frames=5
```

## Checkpoint Verification

The complete macOS arm64 Release build passed 42/42 CTest tests. The generated
macOS and Linux `compile_commands.json` databases contain neither `-Ofast` nor
`-ffast-math`.

The isolated Linux/NVIDIA build compiled the full `redgui` target and passed
31/31 CTest tests. The authenticated production playback smoke passed:

```text
[PlaybackSmoke] PASS start_frame=0 end_frame=300 presented_frame=300 presented_slot=0 view_idx=0 presented_count=351 elapsed_s=2.9958
```

After the production comparison and playback smoke, all 2,538 size,
modification-time, and SHA-256 records in the saved chaser-distance metadata
slice matched the pre-run fingerprint exactly. The synthetic native-repository
test independently snapshots and compares every file in its fixture.
