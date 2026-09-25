# Crimson Phase 4C Production Zarr Parity

Date: 2026-07-12

Phase 4C validates the Phase 4B stimulus repository against a representative
production analysis Zarr. It also exposes the stimulus media path needed by a
future second decoded stream. This checkpoint does not add stimulus video
decoding or composite presentation.

## Production Archive

The same recording was opened through each host's mounted path:

- macOS: `/Volumes/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop/zarr/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop_analysis.zarr`
- NVIDIA: `/groups/johnson/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop/zarr/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop_analysis.zarr`

The `latest` and `latest_complete` stimulus pointers both select
`stimulus_external_ipc_20260616_01`. That run contains:

- `frame_alignment/camera_to_metadata_index`: 139,024 `int64` values;
- `frame_alignment/camera_interpolation_mask`: 139,024 values;
- `video_metadata/frame_metadata/stimulus_frame_num`: 165,647 `uint64`
  values;
- `camera_frame_offset`: 1,024; and
- Zstd-compressed Zarr v3 chunks.

The run has no corrected mapping arrays. Its production path therefore tests
the legacy metadata mapping branch. Corrected direct lookup, corrected metadata
fallback, and precedence remain covered by the checked-in synthetic Zarr
fixture.

## Media Path Resolution

`StimulusRepository` now exposes both `sourceVideoPath()` and
`resolvedSourceVideoPath()`. `ArchiveContext::resolveStoredPath()` preserves a
stored path when it exists. Otherwise, it identifies the recording directory
component and rebuilds the suffix under the recording root containing the open
analysis archive.

For this recording, the stored Linux path:

```text
/groups/johnson/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop/raw/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop.mp4
```

resolves on macOS to:

```text
/Volumes/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop/raw/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop.mp4
```

The local file exists. On the NVIDIA host the stored path already exists and
is returned unchanged. The fixture test exercises the same cross-mount rewrite
without depending on the network share.

## Diagnostic Probe

`stimulus_repository_probe` opens only `ArchiveContext` and the repository. It
prints run identity, stored and resolved media paths, mapping extent, corrected
availability, and selected frame resolutions. With no explicit frames it
chooses values around the first mapping and the repository boundary.

```bash
build/macos-arm64-release/stimulus_repository_probe \
  /path/to/recording_analysis.zarr 1023 1024 1025 69512 139023 139024
```

Representative results were:

```text
camera_frame_offset=1024
camera_frame_count=139024
has_corrected_mapping=false
frame=1023 status=missing source=none interpolated=false
frame=1024 status=mapped source=legacy_metadata metadata=0 stimulus=0 interpolated=false
frame=1025 status=mapped source=legacy_metadata metadata=2 stimulus=2 interpolated=false
frame=69512 status=mapped source=legacy_metadata metadata=82200 stimulus=82180 interpolated=false
frame=139023 status=mapped source=legacy_metadata metadata=165646 stimulus=165578 interpolated=false
frame=139024 status=out_of_range source=none interpolated=false
```

The narrow macOS TensorStore aggregate links the Zarr and Zarr3 drivers plus
the bytes and Zstd codecs used by the fixture and production archive. It
excludes unrelated compression, cloud, and gRPC drivers. The shared adapter is
an object library because the maintained NVIDIA prebuilt TensorStore response
file precedes ordinary static archives on the link line.

## Exhaustive Legacy Parity

The NVIDIA `palette_clipped_loader_probe` accepts
`--stimulus-repository-parity`. After the legacy `ZarrDetectionLoader` opens the
archive, the probe opens the same run through the new repository and compares:

- mapping and corrected-mapping availability;
- camera frame offset and stored source-video path;
- corrected-preferred and legacy-only stimulus lookup for every frame;
- corrected-preferred and legacy-only metadata lookup for every frame; and
- first mapped camera and first stimulus identities.

The comparison covers the larger of the legacy recording extent and repository
mapping extent, plus one boundary frame. The production result was:

```text
[StimulusRepositoryParity] PASS run=stimulus_external_ipc_20260616_01 compared_frames=140036 mapping_frames=139024 corrected=false
```

This is an exact identity comparison, not a timestamp or visual approximation.

## Validation Record

Apple Silicon macOS:

- the full native build passed;
- the production repository probe opened and decoded the Zstd-backed arrays;
- the stored Linux stimulus path resolved to the mounted macOS path; and
- CTest passed 7/7.

Maintained NVIDIA host:

- the clean CUDA 12.4, TensorRT 10.0.1.6, OpenCV 4.10, NVIDIA FFmpeg, and
  prebuilt TensorStore configuration built `redgui`, both probes, and the
  repository adapter;
- the standalone production probe produced the same mapping identities;
- exhaustive legacy parity passed all 140,036 compared positions; and
- CTest passed its registered portable test.

The NVIDIA link retained its known OpenCV/FFmpeg version-family warnings. No GUI
smoke was required because this checkpoint validates storage and frame identity
without rendering.

## Phase Boundary

The storage contract now supplies the exact stimulus frame identity and a
host-usable source video path. Phase 4D subsequently added a camera-driven
macOS stimulus decode session and headless identity tests. See
[the Phase 4D aligned stimulus decode record](crimson_macos_phase4d_aligned_stimulus_decode.md).
