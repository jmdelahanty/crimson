# Acquisition Handoff: macOS-Compatible Stimulus MP4 Encoding

## Purpose

This document tells the acquisition agent how to produce `GoodCopBadCop`,
`RedScare`, and similar rendered stimulus videos that Crimson can use on both
Linux and macOS without a later lossy repair pass.

The detailed encoder settings remain defined in
[`citrus_h264_video_encoding_contract.md`](citrus_h264_video_encoding_contract.md).
This handoff records the macOS failure that made those settings a hard
compatibility requirement and defines the acquisition-side acceptance gate.

## What failed

The original stimulus MP4s were ordinary H.264 files that could be opened and
played linearly in QuickTime and decoded by FFmpeg. That did **not** make them
suitable for Crimson.

Crimson also needs to:

- seek to an arbitrary mapped stimulus frame;
- start decoding in the middle of a recording;
- retrieve an exact frame while paused;
- step forward one source frame at a time; and
- keep a 120 fps stimulus stream aligned to a 100 fps camera stream using the
  analysis Zarr mapping.

On Apple Silicon, AVFoundation could discover the original video track and
report its duration, dimensions, and frame rate, but midstream
`AVAssetReader` requests returned `Cannot Decode`. QuickTime playback still
looked smooth because it could begin at the start and decode continuously.
Linear playback therefore did not exercise the random-access behavior Crimson
requires.

Inspection and the repository repair checker identified invalid or missing MP4
sync-sample metadata and unsuitable keyframe layout in the affected rendered
stimulus files. Without trustworthy sync samples, a decoder cannot reliably
select a preceding independently decodable frame and preroll to an arbitrary
target.

## Evidence from the repair

On 2026-07-12, all stimulus MP4s under the shared recording checkout matching
`GoodCopBadCop` or `RedScare` were checked and repaired:

- 40 `GoodCopBadCop` videos;
- 28 `RedScare` videos;
- 68 videos total;
- all 68 originals retained beside the replacement as `.mp4.bak`;
- all 68 repaired files passed the repository GOP/sync-sample check;
- all repaired streams retained their frame counts and resolutions;
- all repaired streams begin at media time `0.000`; and
- no incomplete `.recoding` files remained.

The controlled re-encoded stimulus also passed the macOS feasibility tests for
exact random access, consecutive paused-frame access, multistream common-time
access, and sampled Zarr-mapped camera-to-stimulus alignment. The original
version failed AVFoundation random access.

This establishes that the video content, resolution, and 120 fps rate are
supported on Apple Silicon. The incompatibility was in the encoded random-
access structure and MP4 metadata, not a fundamental Mac hardware limit.

## Acquisition output requirements

The acquisition agent should correct the original writer. It should **not**
plan to create a broken canonical MP4 and repair it afterward.

Each rendered stimulus video must satisfy all of the following:

| Property | Required value or behavior |
| --- | --- |
| Container | MP4 |
| Codec | H.264/AVC |
| Pixel format | `yuv420p` |
| Frame rate | Preserve the rendered source rate; currently 120 fps |
| Timeline start | First video sample at PTS `0.000` |
| GOP length | 30 frames at 120 fps, or 0.25 seconds if the rate changes |
| Random-access frames | True IDR frames at every GOP boundary |
| B-frames | Disabled for the current low-latency IPPP design |
| Packet flags | Every IDR packet marked with `AV_PKT_FLAG_KEY` |
| MP4 sync table | Valid `stss` entries matching the real IDR packets |
| MP4 metadata placement | `moov` atom at the start via `+faststart` |
| Frame preservation | No dropped, duplicated, or reordered logical frames |

For NVENC, use the equivalent of:

```text
gopLength = 30
idrPeriod = 30
frameIntervalP = 1
```

For the FFmpeg CLI, the known-good reference is:

```bash
ffmpeg -i INPUT \
  -c:v h264_nvenc -preset p1 -tune ll -rc vbr \
  -g 30 -keyint_min 30 -forced-idr 1 \
  -pix_fmt yuv420p -movflags +faststart -an \
  OUTPUT.mp4
```

The production writer should preserve its intended bitrate policy. The
important compatibility properties are the IDR cadence, correct keyframe
packet flags, valid MP4 sync table, zero-based timestamps, and fast-start
metadata.

If acquisition uses NVENC directly and passes encoded packets to libavformat,
it must detect H.264 IDR access units and set `AV_PKT_FLAG_KEY` before calling
the muxer. Having an IDR NAL unit in the bitstream is insufficient if the MP4
muxer is never told that the packet is a sync sample.

## Required publishing workflow

Acquisition should publish videos transactionally:

1. Encode to a temporary filename on the destination filesystem.
2. Close and flush the encoder and MP4 muxer successfully.
3. Run the automated checks below against the completed temporary file.
4. Compare its expected frame count, dimensions, and frame rate with the
   acquisition record.
5. Only after every check passes, atomically rename it to the final `.mp4`
   path.
6. On failure, retain diagnostic output, reject the recording artifact, and do
   not publish the temporary file as the canonical stimulus video.

This prevents a truncated or structurally invalid file from appearing valid
merely because it has the expected name.

## Automated acceptance checks

At minimum, the acquisition agent should run these checks for every output.

### Stream and timeline

```bash
ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name,width,height,pix_fmt,r_frame_rate,avg_frame_rate,start_time,nb_frames \
  -of default=noprint_wrappers=1 \
  OUTPUT.mp4
```

Reject the file unless:

- `codec_name=h264`;
- dimensions, rate, and frame count match the acquisition record;
- `pix_fmt=yuv420p`; and
- `start_time=0.000000`.

### Sync-sample table

Use a real MP4 parser in production. The following is only a quick smoke check:

```bash
python3 -c "from pathlib import Path; p=Path('OUTPUT.mp4'); assert b'stss' in p.read_bytes(), 'missing stss'"
```

Reject a file with no `stss` table. Also verify that the table entries refer to
actual IDR access units; atom presence alone is not sufficient.

### Keyframe cadence

```bash
ffprobe -v error -select_streams v:0 \
  -read_intervals 0%+5 \
  -show_entries packet=pts_time,flags \
  -of csv=p=0 \
  OUTPUT.mp4
```

The first packet must be marked as a keyframe, subsequent keyframes must occur
every 30 frames for a 120 fps stream, and the packet flags must agree with the
IDR access units.

### Fast-start metadata

```bash
python3 -c "from pathlib import Path; d=Path('OUTPUT.mp4').read_bytes()[:4096]; assert b'moov' in d, 'moov is not at file start'"
```

### Repository compatibility checker

Before deployment, run the Crimson checker over representative acquisition
output:

```bash
python3 scripts/reencode_h264_gop.py /path/to/OUTPUT.mp4 --gop 30
```

This is a dry run unless `--apply` is supplied. Acceptance requires:

```text
Needs re-encode: 0
Already OK:      1
```

Do not use `--apply` as the acquisition acceptance mechanism. If freshly
written output needs repair, the writer is still incorrect.

## macOS compatibility canary

FFprobe checks are necessary but cannot fully prove AVFoundation behavior.
Before releasing an acquisition-writer change, copy a representative output to
an Apple Silicon Mac and run the Crimson AVFoundation feasibility suite. The
canary must demonstrate:

- exact random access near the beginning, middle, and end;
- at least 20 consecutive one-frame steps from a midstream start;
- no `Cannot Decode` results;
- exact access for the main, crop, and real stimulus streams; and
- successful sampled Zarr-mapped alignment using the real analysis mapping.

Opening the file in QuickTime and watching it play is useful as a visual smoke
test, but it is **not** an acceptance test for Crimson compatibility.

## Existing recordings and data provenance

Re-encoding decodes and compresses the image content again, so it is a lossy
transformation even when the frame count and geometry are preserved. Existing
recordings should therefore follow this policy:

- retain the original as `.mp4.bak` or in immutable archival storage;
- treat the repaired MP4 as a playback-compatible derivative;
- record the encoder command/version and validation result in provenance; and
- never silently present the repaired derivative as the only canonical source.

The repository repair command for legacy H.264 rendered recordings is:

```bash
python3 scripts/reencode_h264_gop.py /path/to/recordings --gop 30 --apply
```

It encodes to a temporary file, validates sync metadata, frame count, and
resolution, renames the original to `.mp4.bak`, and then installs the validated
replacement under the original name.

## Definition of done for the acquisition agent

The acquisition-side fix is complete only when:

1. newly captured stimulus MP4s pass every automated acceptance check without
   re-encoding;
2. the repository checker reports `Needs re-encode: 0`;
3. the Apple Silicon AVFoundation canary passes exact seek and stepping tests;
4. a real three-stream Zarr-alignment canary passes;
5. failed or interrupted writes cannot appear at the final path; and
6. the writer configuration and validation evidence are documented in the
   acquisition release notes.
