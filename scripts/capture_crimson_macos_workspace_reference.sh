#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
app="${CRIMSON_MACOS_REFERENCE_APP:-$repo_root/build/macos-arm64-release/Crimson.app/Contents/MacOS/Crimson}"
recording_root="${CRIMSON_MACOS_REFERENCE_RECORDING_ROOT:-/Volumes/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop}"
video="${CRIMSON_MACOS_REFERENCE_VIDEO:-$recording_root/cams/Cam2010093_2026-06-14T21-12-08Z_arena_1.mp4}"
zarr="${CRIMSON_MACOS_REFERENCE_ZARR:-$recording_root/zarr/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop_analysis.zarr}"
output_dir="${1:-${CRIMSON_MACOS_REFERENCE_OUTPUT_DIR:-$repo_root/docs/reference/phase5l/macos}}"
timeout="${CRIMSON_MACOS_REFERENCE_TIMEOUT:-180}"
logical_size="${CRIMSON_MACOS_REFERENCE_SIZE:-1920x1080}"

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

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

capture_state() {
    local state="$1"
    local frame="$2"
    local stem="$3"
    local marker="$output_dir/$stem.ui-reference.json"
    local image="$marker.png"
    local log="$output_dir/$stem.crimson.log"
    local checksum="$output_dir/$stem.sha256.txt"
    local args=(
        --ui-reference-state "$state"
        --ui-reference-frame "$frame"
        --ui-reference-ready-file "$marker"
        --ui-reference-size "$logical_size"
        --ui-reference-timeout "$timeout"
    )

    if [[ "$state" != "empty" ]]; then
        args=(--video "$video" --zarr "$zarr" "${args[@]}")
    fi

    rm -f "$marker" "$image" "$log" "$checksum"
    echo "Capturing $state at frame $frame..."
    "$app" "${args[@]}" >"$log" 2>&1

    if ! jq -e \
        --arg state "$state" \
        --argjson frame "$frame" \
        --arg size "$logical_size" \
        '($size | split("x") | map(tonumber)) as $dimensions |
         .format == "crimson_ui_reference_v1" and
         .platform == "macos-metal" and
         .state == $state and
         .write_contract == "read-only" and
         .target_frame == $frame and
         .stable_frames >= 60 and
         .logical_content_size.width == $dimensions[0] and
         .logical_content_size.height == $dimensions[1] and
         .framebuffer_size.width == $dimensions[0] and
         .framebuffer_size.height == $dimensions[1] and
         .framebuffer_scale.x == 1 and
         .framebuffer_scale.y == 1 and
         .semantic_snapshot.schema == "crimson.imgui_semantic_snapshot.v1" and
         (.semantic_snapshot.windows | length) > 0 and
         (($state == "empty" and .presented_frame == -1) or
          ($state != "empty" and .presented_frame == $frame)) and
         ($state != "overlays" or
          (.overlays.ready and .overlays.counts.keypoints > 0 and
           .overlays.counts.headings > 0 and
           .overlays.counts.subject_mask_primitives > 0 and
           .overlays.counts.subject_mask_rasters > 0 and
           .overlays.counts.subject_mask_text > 0)) and
         ($state != "crop-preview" or
          (.crop.ready and .crop.camera_frame == $frame and
           .crop.source == "live-geometry")) and
         ($state != "analysis-eye" or
          (.analysis.ready and .analysis.eye_representation == "gaze" and
           .overlays.ready)) and
         ($state != "stimulus-debug" or
          (.stimulus.ready and .stimulus.camera_frame == $frame and
           .stimulus.target_frame == .stimulus.presented_frame))' \
        "$marker" >/dev/null; then
        echo "Reference marker validation failed: $marker" >&2
        jq '{platform, state, target_frame, presented_frame, stable_frames,
             logical_content_size, framebuffer_size, overlays, crop,
             stimulus, analysis}' "$marker" >&2 || true
        tail -n 160 "$log" >&2 || true
        exit 4
    fi
    if [[ ! -f "$image" ]]; then
        echo "Reference image was not written: $image" >&2
        exit 4
    fi

    {
        printf '%s  %s\n' "$(sha256_file "$image")" "$(basename "$image")"
        printf '%s  %s\n' "$(sha256_file "$marker")" "$(basename "$marker")"
    } >"$checksum"
    echo "Captured $image"
}

capture_state empty 0 initial_empty_1920x1080
capture_state workspace 56 workspace_exact_f000056_1920x1080
capture_state overlays 56 overlays_exact_f000056_1920x1080
capture_state crop-preview 56 crop_preview_exact_f000056_1920x1080
capture_state analysis-eye 56 analysis_eye_exact_f000056_1920x1080
capture_state stimulus-debug 1024 stimulus_debug_exact_f001024_1920x1080

jq -n \
    --arg schema "crimson.phase5l.macos-capture-set.v1" \
    --arg captured_at "$(date "+%Y-%m-%dT%H:%M:%S%z")" \
    --arg app "$app" \
    --arg app_sha256 "$(sha256_file "$app")" \
    --arg video "$video" \
    --arg zarr "$zarr" \
    --arg logical_size "$logical_size" \
    --arg windows_runtime "deferred_by_user" \
    '{schema: $schema, captured_at: $captured_at,
      runtime: {platform: "macos", executable: $app,
                executable_sha256: $app_sha256},
      inputs: {video: $video, zarr: $zarr},
      logical_size: $logical_size,
      windows_runtime: $windows_runtime,
      captures: [
        {state: "empty", frame: 0, stem: "initial_empty_1920x1080"},
        {state: "workspace", frame: 56, stem: "workspace_exact_f000056_1920x1080"},
        {state: "overlays", frame: 56, stem: "overlays_exact_f000056_1920x1080"},
        {state: "crop-preview", frame: 56, stem: "crop_preview_exact_f000056_1920x1080"},
        {state: "analysis-eye", frame: 56, stem: "analysis_eye_exact_f000056_1920x1080"},
        {state: "stimulus-debug", frame: 1024, stem: "stimulus_debug_exact_f001024_1920x1080"}
      ]}' >"$output_dir/capture-manifest.json"

echo "macOS reference set captured in $output_dir"
