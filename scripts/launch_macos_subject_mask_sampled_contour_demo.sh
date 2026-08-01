#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
app="${CRIMSON_MACOS_APP:-$repo_root/build/macos-arm64-release/Crimson.app/Contents/MacOS/Crimson}"
recording_root="${CRIMSON_SUBJECT_MASK_RECORDING_ROOT:-/Volumes/johnsonlab/jeremy/recordings/sleepyfish_2026_05_05_17_45_30_cam2010095}"
source_store="${CRIMSON_SUBJECT_MASK_SOURCE_STORE:-/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/subject_mask_storage/full_duration/sleepyfish_cam2010095_20260731_73f7bb5e/analysis.zarr}"
source_run="${CRIMSON_SUBJECT_MASK_SOURCE_RUN:-refined_subject_masks_sleepyfish_subject_mask_full_duration_20260731_73f7bb5e}"
source_digest="${CRIMSON_SUBJECT_MASK_SOURCE_DIGEST:-9efe2d3e5865d495e40779ecb1be3fcc55953b02bd82ca20125f953c0c2aa78c}"
cache_store="${CRIMSON_SUBJECT_MASK_CONTOUR_STORE:-/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/subject_mask_storage/sampled_contours/sleepyfish_cam2010095_sampled_contours_20260801_90829491/cache.zarr}"
cache_run="${CRIMSON_SUBJECT_MASK_CONTOUR_RUN:-subject_mask_sampled_contours_sleepyfish_20260801_90829491}"
cache_digest="${CRIMSON_SUBJECT_MASK_CONTOUR_DIGEST:-c04a3f9283da0bd9bd16497b7ecc9ca9871c816ad517b1e5d40b092bde8c6861}"
clip_index="$recording_root/recording_clip_index.json"
start_frame="${1:-1000}"

if [[ ! -x "$app" ]]; then
    echo "Crimson macOS app executable not found: $app" >&2
    exit 2
fi
if [[ ! -r "$clip_index" || ! -r "$source_store/zarr.json" || ! -r "$cache_store/zarr.json" ]]; then
    echo "The clip index, source masks, or sampled-contour cache is not readable." >&2
    exit 2
fi
if ! command -v jq >/dev/null 2>&1; then
    echo "jq is required to validate immutable manifests." >&2
    exit 2
fi

actual_source_digest="$(jq -er '.attributes.run_manifest.payload_digest' \
    "$source_store/refined_subject_masks_runs/$source_run/zarr.json")"
actual_cache_digest="$(jq -er '.attributes.run_manifest.payload_digest' \
    "$cache_store/subject_mask_cache_runs/$cache_run/zarr.json")"
if [[ "$actual_source_digest" != "$source_digest" || \
      "$actual_cache_digest" != "$cache_digest" ]]; then
    echo "Subject-mask source or sampled-contour manifest digest mismatch." >&2
    exit 1
fi

echo "Launching sampled-contour subject-mask demo at frame $start_frame"
exec "$app" \
    --recording-clip-index "$clip_index" \
    --zarr "$source_store" \
    --benchmark-subject-mask-v1 "$source_run" "$source_digest" \
    --benchmark-subject-mask-presentation-cache-v1 \
        "$cache_store" "$cache_run" "$cache_digest" \
    --show-subject-masks \
    --start-paused "$start_frame" \
    --no-subject-shapes \
    --no-eye-geometry \
    --no-motion-timeline \
    --no-swim-bout-timeline \
    --no-eye-angle-timeline \
    --no-tail-kinematics-timeline \
    --no-stimulus-context-timeline
