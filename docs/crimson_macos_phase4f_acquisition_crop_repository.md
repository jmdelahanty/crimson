# Crimson Phase 4F-C Acquisition Crop Repository

Date: 2026-07-12

Phase 4F-C implements the portable metadata boundary for Orange acquisition
crop streams. It discovers and validates the imported stream contract, maps a
Crimson camera frame to its exact video frame and CSV row, and converts the
result into the Phase 4F-B crop-source contract. It does not decode video,
create GPU surfaces, or change Crop Preview, `redgui`, or Metal presentation.

## Ownership Boundary

`AcquisitionCropRepository` is C++17-only. Its public values contain stream
identity, declared and resolved file paths, dimensions, rate, format, frame
counts, producer provenance, blank state, and full-frame geometry. They contain
no TensorStore, CUDA, OpenGL, Metal, AVFoundation, or ImGui types.

The repository owns metadata identity, not pixels:

- an acquisition-video backend decodes `resolved_video_path` and reports the
  exact decoded video-frame identity;
- a geometry-only backend supplies the exact full-frame surface; and
- a persisted-Zarr backend continues to own its materialized crop pixels.

The acquisition inventory does not declare full-camera dimensions. The active
full-frame backend must therefore supply them when requesting geometry. Crimson
does not infer them from the encoded crop width or from a recording-specific
constant.

For version 1, camera frame `F` maps by identity to video frame `F`, metadata
row `F`, and one-based `recording_frame_id == F + 1`. `local_frame_id` and
`camera_frame_id` are retained as producer provenance and are not treated as
contiguous array indices. A blank row is still mapped to its encoded black
video frame, but it has no usable live-crop geometry.

## Archive Paths

`ArchiveContext` is now implemented independently of the stimulus repository
and records both the absolute Zarr root and the recording root. Stored paths
resolve as follows:

- relative paths are always recording-root-relative, never process-CWD-relative;
- existing absolute paths remain unchanged;
- a missing foreign absolute path containing the recording directory name is
  rebased to the local recording root; and
- an unrelated missing absolute path remains unchanged and fails normal file
  validation.

Relative paths containing `..` are rejected before resolution. This supports
the same imported archive on Linux `/groups/...` and macOS `/Volumes/...`
without trusting stale import-time `exists` fields.

## TensorStore Adapter

`OpenAcquisitionCropRepository` reads both Zarr v3 `zarr.json` attributes and
Zarr v2 `.zattrs` from:

```text
analysis/acquisition_video_streams
analysis/acquisition_video_streams/streams/crop
```

The root's duplicated crop descriptor must exactly match the child descriptor.
The version 1 validator rejects unsupported schemas, incomplete inventories,
unknown stream/frame/coordinate policies, invalid dimensions or rates,
disagreeing duplicated file paths, missing required files, and inconsistent
contract, CSV, keyframe, summary, or optional status counts.

The CSV reader handles RFC-style quoted fields, escaped quotes, CRLF, quoted
newlines, a UTF-8 BOM, reordered headers, and extra named columns. Values are
parsed strictly. Validation requires contiguous one-based recording IDs,
inverse blank/detection flags, finite confidence in `[0, 1]`, zero geometry on
blank rows, positive contained geometry on detected rows, and an exact row
count. Keyframe identities must be sorted, unique, in range, and start at zero;
the format does not require every frame to be a keyframe.

## Automated Coverage

`acquisition_crop_repository_tests` creates Zarr v2 and v3 fixtures and covers:

- relative and rebased foreign absolute paths;
- reordered and quoted CSV fields;
- source capabilities, exact frame mapping, scaled transforms, and blank rows;
- optional missing status files; and
- missing inventory/video/columns, path traversal, root-child disagreement,
  invalid identities/blank geometry, and count disagreement.

The test is labeled `crop;headless;portable;repository;tensorstore;zarr` and is
part of the maintained macOS build preset.

macOS validation:

```text
cmake --build --preset build-macos-arm64-release -j 8
ctest --preset test-macos-arm64-headless --output-on-failure
12/12 passed
```

The same source patch was applied to the isolated maintained NVIDIA worktree.
With CUDA 12.4, OpenCV 4.10, TensorRT 10.0.1.6, the NVIDIA FFmpeg stack, and a
freshly configured TensorStore graph, both repository executables and the full
`redgui` target linked. All five portable CTests passed, including the new
TensorStore/Zarr fixture. The existing OpenCV/FFmpeg version-family link
warnings remain unchanged.

## Production Archives

`acquisition_crop_repository_probe` opened every imported row from both network
archives. `ffprobe` independently confirmed each MP4's dimensions, frame rate,
and container frame count.

| Recording | Crop video | Contract/MP4/CSV frames | Detected | Blank | Invalid geometry |
| --- | ---: | ---: | ---: | ---: | ---: |
| GoodCopBadCop | 256x256 at 100 fps | 140035 | 138498 | 1537 | 0 |
| RedScare | 384x384 at 100 fps | 139908 | 139903 | 5 | 0 |

Both keyframe inventories begin at zero and contain the declared number of
in-range identities. First, middle, and last requests mapped exactly; the
request at `frame_count` reported `OutOfRange`.

Both probes passed from the macOS `/Volumes/johnsonlab` mount and again from
the NVIDIA host's native `/groups/johnson/johnsonlab` mount. This exercises
both foreign-absolute rebasing and original absolute-path preservation against
the same imported archives.

## Phase 4F-D Follow-up

Phase 4F-D added the Apple acquisition-crop playback session around the
existing bounded AVFoundation video buffer. Its implementation and validation
record is in `crimson_macos_phase4f_apple_acquisition_crop_playback.md`.
Presentation remains a later step and must consume the session's shared
selection result rather than add backend-specific frame policy.
