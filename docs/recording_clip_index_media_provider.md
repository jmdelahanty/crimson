# Recording Clip Index Media Provider

Status: shared compatibility contract with macOS and NVIDIA playback adapters
implemented

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

Crimson accepts two index generations through one validated descriptor model:

- the unversioned `materialized_stream_copy` document used by the May and June
  recordings; and
- `palette.orange_external_ipc_recording_clip_index.v1`, schema version 1, for
  consolidated Orange `rolling_clips` recordings.

The materialized adapter continues to require successful validation checks,
keyframe-aligned clip starts, contiguous coverage from frame zero through the
declared source frame count, and matching per-clip identities. Its validation
path is unchanged.

The rolling adapter accepts only the consolidated, single-camera Palette
projection. It does not accept a raw multi-camera Orange index or relabel the
mode. It requires completed and drained rows, matching recording/camera/clip
identities, safe recording-relative artifact paths, an authoritative full
stream in each versioned clip manifest, and readable existing media. It reads
FPS and clip-start keyframe evidence from each declared keyframe sidecar. Clip
lengths are taken from the rows; they are not inferred from rollover ordinal or
nominal duration.

Rolling acquisition IDs are inclusive and need not have a fixed base. Crimson
uses each clip's explicit `parent_frame_index_offset` from the sibling
`recording_frame_index_manifest.json` to translate them to the zero-based
parent axis. The manifest must be the completed versioned Palette convenience
index, identify the same recording, and contain successful per-clip row-count,
endpoint, intra-clip continuity, and inter-clip continuity checks. Crimson
validates the bounded JSON manifest rather than loading the multi-million-row
Parquet/CSV table. Thus an acquisition beginning at ID 0, 1, or another value
maps correctly when the published offset and all declared ranges agree.

Absolute metadata paths embedded in the derived frame-index manifest are
rebased through that manifest's original `recording_folder` and must equal the
safe relative metadata path in the clip row. This preserves validation when a
recording directory is copied or remounted without trusting stale absolute
locations.

The legacy materialized document remains unversioned. Crimson treats it as a
strict compatibility adapter rather than a production-authoritative storage
contract. Future hardening of that schema should add:

- a schema ID and version;
- canonical serialization and a document digest;
- an explicit zero-based, half-open parent frame-domain declaration;
- source width, height, pixel format, and rational frame rate; and
- immutable per-clip content or clip-manifest digests.

These additions are not required to use existing Palette recordings. The
decoder verifies each opened clip's frame count, dimensions, and nominal frame
rate against the validated index and the first clip. Supporting rolling clips
does not re-encode or rewrite source media.

## Discovery And Launch

Both application shells accept an explicit index alongside an analysis
archive:

```bash
Crimson --zarr /path/to/analysis.zarr \
  --recording-clip-index /path/to/recording_clip_index.json
```

On macOS, `--recording-clip-index` and `--video` are mutually exclusive. The
NVIDIA shell requires `--zarr` or `--recording` so the global analysis frame
axis remains explicit.

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

The native NVIDIA smoke used the same parent range through
`--recording-clip-index`. FFmpeg/NVDEC loaded clip 1 at decoder-local frame
zero, the shared handoff settled after parent frame 54,000 presented, and the
smoke passed at parent frame 54,010 with the overlay query on frame 54,010.

The NVIDIA mapped-media adapter exposes the strict index and legacy clipped
collections through the same `ClippedFrameBinding` query. A narrow coordinator
applies the portable boundary policy, invokes the injected load/seek command,
validates the newly loaded binding, and publishes lifecycle events. The
application composition root does not parse index entries, scan legacy clip
ranges, or mirror the active handoff state.

## Platform Adoption

The index parser, validated descriptors, frame mapping, archive discovery, and
session source field are backend-neutral C++.

The macOS adapter switches AVFoundation/VideoToolbox providers. The shared
Linux/Windows adapter now consumes `RecordingClipMediaProvider` and switches
FFmpeg/NVDEC media while keeping playback on the parent frame axis. Neither
adapter duplicates JSON parsing or parent/local-frame policy. Linux has a
native compile and focused test gate; Windows still needs a native build and
GUI smoke before release qualification.
