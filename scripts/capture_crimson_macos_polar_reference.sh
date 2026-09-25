#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
app="${CRIMSON_MACOS_REFERENCE_APP:-$repo_root/build/macos-arm64-release/Crimson.app/Contents/MacOS/Crimson}"
recording_root="${CRIMSON_MACOS_REFERENCE_RECORDING_ROOT:-/Volumes/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop}"
video="${CRIMSON_MACOS_REFERENCE_VIDEO:-$recording_root/cams/Cam2010093_2026-06-14T21-12-08Z_arena_1.mp4}"
zarr="${CRIMSON_MACOS_REFERENCE_ZARR:-$recording_root/zarr/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop_analysis.zarr}"
output_dir="${1:-${CRIMSON_MACOS_REFERENCE_OUTPUT_DIR:-$repo_root/docs/reference/phase5m/macos}}"
timeout="${CRIMSON_MACOS_REFERENCE_TIMEOUT:-180}"
logical_size="${CRIMSON_MACOS_REFERENCE_SIZE:-1920x1080}"
frame=1024
stem=polar_exact_f001024_1920x1080

if [[ ! -x "$app" ]]; then
    echo "Crimson macOS executable not found: $app" >&2
    exit 2
fi
if [[ ! -f "$video" ]]; then
    echo "Reference camera video not found: $video" >&2
    exit 2
fi
if [[ ! -d "$zarr" ]]; then
    echo "Reference Zarr archive not found: $zarr" >&2
    exit 2
fi
if ! command -v jq >/dev/null 2>&1; then
    echo "jq is required for reference-marker validation." >&2
    exit 2
fi

mkdir -p "$output_dir"
output_dir="$(cd "$output_dir" && pwd)"
marker="$output_dir/$stem.ui-reference.json"
image="$marker.png"
log="$output_dir/$stem.crimson.log"
checksum="$output_dir/$stem.sha256.txt"
manifest="$output_dir/capture-manifest.json"
rm -f "$marker" "$image" "$log" "$checksum" "$manifest"

"$app" \
    --video "$video" \
    --zarr "$zarr" \
    --ui-reference-state polar \
    --ui-reference-frame "$frame" \
    --ui-reference-ready-file "$marker" \
    --ui-reference-size "$logical_size" \
    --ui-reference-timeout "$timeout" >"$log" 2>&1

if ! jq -e \
    --argjson frame "$frame" \
    --arg size "$logical_size" \
    '($size | split("x") | map(tonumber)) as $dimensions |
     .format == "crimson_ui_reference_v1" and
     .platform == "macos-metal" and
     .state == "polar" and
     .write_contract == "read-only" and
     .target_frame == $frame and
     .presented_frame == $frame and
     .stable_frames >= 60 and
     .logical_content_size.width == $dimensions[0] and
     .logical_content_size.height == $dimensions[1] and
     .framebuffer_size.width == $dimensions[0] and
     .framebuffer_size.height == $dimensions[1] and
     .framebuffer_scale.x == 1 and
     .framebuffer_scale.y == 1 and
     .polar.ready and
     .polar.status == "ready" and
     .polar.availability == "ready" and
     .polar.requested_frame == $frame and
     .polar.source_frame == $frame and
     .polar.point_count == 2 and
     (.polar.points | length) == 2 and
     (.polar.semantic_signature |
      startswith("crimson.polar-scene-semantics.v1")) and
     ([.polar.text[].content] |
      contains(["front", "left", "right", "behind",
                "Chaser bearing f=1024"]))' "$marker" >/dev/null; then
    echo "Mac polar reference marker validation failed: $marker" >&2
    jq '{platform, state, target_frame, presented_frame, stable_frames,
         framebuffer_size, framebuffer_scale, polar}' "$marker" >&2 || true
    tail -n 160 "$log" >&2 || true
    exit 4
fi
if [[ ! -f "$image" ]]; then
    echo "Mac polar reference image was not written: $image" >&2
    exit 4
fi

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

{
    printf '%s  %s\n' "$(sha256_file "$image")" "$(basename "$image")"
    printf '%s  %s\n' "$(sha256_file "$marker")" "$(basename "$marker")"
} >"$checksum"

jq -n \
    --arg schema "crimson.phase5m.polar-capture-set.v1" \
    --arg captured_at "$(date "+%Y-%m-%dT%H:%M:%S%z")" \
    --arg executable "$app" \
    --arg executable_sha256 "$(sha256_file "$app")" \
    --arg video "$video" \
    --arg zarr "$zarr" \
    --arg logical_size "$logical_size" \
    --argjson frame "$frame" \
    '{schema: $schema, captured_at: $captured_at,
      runtime: {platform: "macos", executable: $executable,
                executable_sha256: $executable_sha256},
      inputs: {video: $video, zarr: $zarr},
      logical_size: $logical_size,
      capture: {state: "polar", frame: $frame,
                stem: "polar_exact_f001024_1920x1080"}}' >"$manifest"

echo "Mac polar reference captured in $output_dir"
