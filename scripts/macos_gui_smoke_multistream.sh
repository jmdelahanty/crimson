#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
app="${CRIMSON_MACOS_APP:-$repo_root/build/macos-arm64-release/Crimson.app/Contents/MacOS/Crimson}"
recording_root="/Volumes/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop"
default_video="$recording_root/cams/Cam2010093_2026-06-14T21-12-08Z_arena_1.mp4"
default_zarr="$recording_root/zarr/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop_analysis.zarr"
video="${1:-${CRIMSON_MACOS_MULTISTREAM_SMOKE_VIDEO:-$default_video}}"
zarr="${2:-${CRIMSON_MACOS_MULTISTREAM_SMOKE_ZARR:-$default_zarr}}"
smoke_range="${3:-${CRIMSON_MACOS_MULTISTREAM_SMOKE_RANGE:-1024:7024}}"
source="${4:-${CRIMSON_MACOS_MULTISTREAM_SMOKE_SOURCE:-acquisition}}"
crop_run="${5:-${CRIMSON_MACOS_MULTISTREAM_SMOKE_CROP_RUN:-}}"
timestamp="$(date +%Y%m%d_%H%M%S)"
log_path="${CRIMSON_MACOS_MULTISTREAM_SMOKE_LOG:-/tmp/crimson_macos_multistream_${source}_${timestamp}.log}"

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

arguments=(
    --video "$video"
    --zarr "$zarr"
    --crop-source "$source"
    --multistream-smoke "$smoke_range"
)
if [[ -n "$crop_run" ]]; then
    arguments+=(--crop-run "$crop_run")
fi

set +e
"$app" "${arguments[@]}" >"$log_path" 2>&1
status=$?
set -e

if [[ "$status" -ne 0 ]]; then
    echo "macOS multistream smoke failed with status $status" >&2
    echo "log: $log_path" >&2
    tail -n 220 "$log_path" >&2 || true
    exit "$status"
fi
for record in AppleVideoSmoke AppleStimulusSmoke AppleCropSmoke AppleMultistreamSmoke; do
    if ! grep -Fq "[$record] PASS" "$log_path"; then
        echo "macOS multistream smoke did not emit [$record] PASS" >&2
        tail -n 220 "$log_path" >&2 || true
        exit 1
    fi
done

tail -n 8 "$log_path"
echo "macOS multistream smoke passed"
echo "log: $log_path"
