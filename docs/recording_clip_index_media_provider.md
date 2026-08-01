# Recording Clip Index Media Provider

Status: shared compatibility contract and macOS playback adapter implemented

## Purpose

Long recordings may reference a sequence of independently decodable media
clips instead of one monolithic video. Crimson keeps analysis, playback,
seeking, and presentation on the parent recording frame axis while a platform
decoder operates on one clip-local frame axis at a time.

The shared `RecordingClipIndex` contract owns:

- strict parsing of `recording_clip_index.json`;
- recording and camera identity;
- contiguous half-open parent frame ranges;
- parent-frame to clip-local-frame mapping;
- recording-relative clip path resolution; and
- validation that every declared clip file is present.

Platform playback adapters own decoder creation and clip switching. They must
publish the parent frame as `frame_number`, preserve the decoder-local frame as
`local_frame_number`, and normalize PTS onto the parent recording timeline.

## Current Compatibility Boundary

Palette's current index is sufficient for read-only playback. Crimson requires
the current `materialized_stream_copy` mode, successful validation checks,
keyframe-aligned clip starts, contiguous coverage from frame zero through the
declared source frame count, and matching per-clip identities.

The current document is unversioned. Crimson therefore treats it as a strict
compatibility adapter rather than a production-authoritative storage contract.
Future Palette hardening should add:

- a schema ID and version;
- canonical serialization and a document digest;
- an explicit zero-based, half-open parent frame-domain declaration;
- source width, height, pixel format, and rational frame rate; and
- immutable per-clip content or clip-manifest digests.

These additions are not required to use existing Palette recordings. The
decoder currently verifies each opened clip's frame count, dimensions, and
nominal frame rate against the validated index and the first clip.

## Discovery And Launch

Crimson accepts an explicit index:

```bash
Crimson --recording-clip-index /path/to/recording_clip_index.json
```

`--recording-clip-index` and `--video` are mutually exclusive. A Zarr archive
can be supplied normally with `--zarr`.

The macOS File menu also exposes **Open Recording Clip Index...** for direct
interactive use.

When a user selects a Zarr archive, Crimson first resolves an affiliated full
video. If none is available, it uses the archive's `recording_id` to discover
and validate `recording_clip_index.json`. Benchmark archive directory names do
not establish identity; the archive and validated index must agree.

## Validation

Headless tests cover empty and multi-clip mappings, every clip boundary,
out-of-range frames, gaps, overlaps, duplicate clip IDs, missing media,
non-keyframe starts, failed publication checks, traversal paths, and recording
identity mismatch. The Apple provider test uses two real encoded clips and
checks both sequential boundary crossing and random seeking.

The mounted Sleepyfish smoke crossed the first boundary from parent frame
53,990 through 54,010. It presented frame 54,010 with zero PTS error while the
decoder switched from clip 0 to clip 1.

## Platform Adoption

The index parser, validated descriptors, frame mapping, archive discovery, and
session source field are backend-neutral C++.

The macOS adapter switches AVFoundation/VideoToolbox providers. Linux and
Windows can consume the same mapping contract in their FFmpeg/NVIDIA decoder
adapter; they should not duplicate JSON parsing or parent/local-frame policy.
