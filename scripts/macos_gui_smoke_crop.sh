#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
app="${CRIMSON_MACOS_APP:-$repo_root/build/macos-arm64-release/Crimson.app/Contents/MacOS/Crimson}"
recording_root="/Volumes/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop"
default_video="$recording_root/cams/Cam2010093_2026-06-14T21-12-08Z_arena_1.mp4"
default_zarr="$recording_root/zarr/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop_analysis.zarr"
video="${1:-${CRIMSON_MACOS_CROP_SMOKE_VIDEO:-$default_video}}"
zarr="${2:-${CRIMSON_MACOS_CROP_SMOKE_ZARR:-$default_zarr}}"
smoke_range="${3:-${CRIMSON_MACOS_CROP_SMOKE_RANGE:-1024:1324}}"
source="${4:-${CRIMSON_MACOS_CROP_SMOKE_SOURCE:-acquisition}}"
timestamp="$(date +%Y%m%d_%H%M%S)"
log_path="${CRIMSON_MACOS_CROP_SMOKE_LOG:-/tmp/crimson_macos_crop_smoke_${source}_${timestamp}.log}"

if [[ "$source" != "acquisition" && "$source" != "geometry" ]]; then
    echo "crop source must be acquisition or geometry: $source" >&2
    exit 2
fi
if [[ ! -x "$app" ]]; then
    echo "Crimson macOS app executable not found: $app" >&2
    echo "Build it with: cmake --build --preset build-macos-arm64-release" >&2
    exit 2
fi
if [[ ! -f "$video" ]]; then
    echo "Main camera video not found: $video" >&2
    exit 2
fi
if [[ ! -d "$zarr" ]]; then
    echo "Analysis Zarr not found: $zarr" >&2
    exit 2
fi

set +e
"$app" --video "$video" --zarr "$zarr" --crop-source "$source" \
    --crop-smoke "$smoke_range" >"$log_path" 2>&1
status=$?
set -e

if [[ "$status" -ne 0 ]]; then
    echo "macOS crop smoke failed with status $status" >&2
    echo "log: $log_path" >&2
    tail -n 180 "$log_path" >&2 || true
    exit "$status"
fi
if ! grep -Fq "[AppleVideoSmoke] PASS" "$log_path"; then
    echo "macOS crop smoke did not emit the camera PASS record" >&2
    tail -n 180 "$log_path" >&2 || true
    exit 1
fi
if ! grep -Fq "[AppleCropSmoke] PASS" "$log_path"; then
    echo "macOS crop smoke did not emit the crop PASS record" >&2
    tail -n 180 "$log_path" >&2 || true
    exit 1
fi

tail -n 6 "$log_path"
echo "macOS crop smoke passed"
echo "log: $log_path"
