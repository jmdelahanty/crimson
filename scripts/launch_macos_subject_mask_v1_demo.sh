#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
app="${CRIMSON_MACOS_APP:-$repo_root/build/macos-arm64-release/Crimson.app/Contents/MacOS/Crimson}"
fixture_root="${CRIMSON_SUBJECT_MASK_V1_FIXTURE_ROOT:-/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/subject_mask_storage/integration/20260128_cropv2_subject_mask_cache_pipeline_20260731_v2}"
nrs_root="${CRIMSON_NRS_MOUNT:-/Volumes/johnson}"
archive="$fixture_root/refined.zarr"
handoff="$fixture_root/handoff_manifest.json"
run="refined_subject_masks_cache_canary_v1"
handoff_digest="dbc958d2be32f530b1deffafb99e84d2150d8948171d05a2e4992fa89d07c6eb"
manifest_payload_digest="56cdc38699dff17c2331e161f73be50da372bedf49f3a707159d78ce81093d03"
source_video="$nrs_root/palette_staging/flat_roi_cache/keypoint_v2_cropv2_20260729_v2/source/recording/cams/Cam2010093_2026-01-28T19-22-28Z_arena_1.mp4"
video="${CRIMSON_SUBJECT_MASK_V1_VIDEO:-$source_video}"

if [[ ! -x "$app" ]]; then
    echo "Crimson macOS app executable not found: $app" >&2
    echo "Build it with: cmake --build build/macos-arm64-release --target Crimson -j6" >&2
    exit 2
fi
if [[ ! -r "$archive/zarr.json" || ! -r "$handoff" ]]; then
    echo "Subject-mask v1 fixture is not readable: $fixture_root" >&2
    exit 2
fi
if [[ ! -r "$source_video" ]]; then
    echo "Matching NRS video is not readable: $source_video" >&2
    echo "Mount NRS at $nrs_root or set CRIMSON_NRS_MOUNT." >&2
    exit 2
fi
if ! command -v jq >/dev/null 2>&1; then
    echo "jq is required to validate the immutable handoff." >&2
    exit 2
fi

actual_handoff_digest="$(jq -er '.payload_digest' "$handoff")"
if [[ "$actual_handoff_digest" != "$handoff_digest" ]]; then
    echo "Subject-mask handoff digest mismatch." >&2
    exit 1
fi
actual_manifest_digest="$(jq -er '.attributes.run_manifest.payload_digest' \
    "$archive/refined_subject_masks_runs/$run/zarr.json")"
if [[ "$actual_manifest_digest" != "$manifest_payload_digest" ]]; then
    echo "Subject-mask run-manifest payload digest mismatch." >&2
    exit 1
fi

if [[ -z "${CRIMSON_SUBJECT_MASK_V1_VIDEO:-}" ]] && \
   command -v ffprobe >/dev/null 2>&1 && \
   [[ "$(ffprobe -v error -select_streams v:0 \
       -show_entries stream=codec_tag_string -of default=nw=1:nk=1 \
       "$source_video")" == "hev1" ]]; then
    if ! command -v ffmpeg >/dev/null 2>&1; then
        echo "ffmpeg is required to remux the fixture's hev1 tag for AVFoundation." >&2
        exit 2
    fi
    compatibility_video="${CRIMSON_SUBJECT_MASK_V1_COMPAT_VIDEO:-/private/tmp/crimson_subject_mask_v1_demo_hvc1.mp4}"
    if [[ ! -r "$compatibility_video" ]] || \
       [[ "$(ffprobe -v error -select_streams v:0 \
           -show_entries stream=codec_tag_string -of default=nw=1:nk=1 \
           "$compatibility_video" 2>/dev/null || true)" != "hvc1" ]]; then
        temporary_video="${compatibility_video%.mp4}.partial.mp4"
        echo "Creating a frame-zero-preserving 25 s hvc1 demo remux..."
        ffmpeg -y -v error -i "$source_video" -t 25 -map 0:v:0 \
            -c:v copy -tag:v hvc1 -movflags +faststart "$temporary_video"
        mv "$temporary_video" "$compatibility_video"
    fi
    video="$compatibility_video"
fi

echo "Launching selector-ineligible subject-mask v1 demo"
echo "archive: $archive"
echo "run: $run"
echo "video: $video"

exec "$app" \
    --video "$video" \
    --zarr "$archive" \
    --benchmark-subject-mask-v1 "$run" "$manifest_payload_digest" \
    --show-subject-masks \
    --start-paused 1000 \
    --no-subject-shapes \
    --no-eye-geometry \
    --no-motion-timeline \
    --no-swim-bout-timeline \
    --no-eye-angle-timeline \
    --no-tail-kinematics-timeline \
    --no-stimulus-context-timeline
