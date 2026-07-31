#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
app="${CRIMSON_KEYPOINT_QUALITY_APP:-$repo_root/build/macos-arm64-release/Crimson.app/Contents/MacOS/Crimson}"
video="${CRIMSON_KEYPOINT_QUALITY_VIDEO:-/Volumes/johnsonlab/jeremy/recordings/sleepyfish_2026_05_05_17_45_30_cam2010095/cams/Cam2010095_sleepyfish_2026_05_05_17_45_30_cam2010095.mp4}"
analysis="${CRIMSON_KEYPOINT_QUALITY_ANALYSIS:-/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/crimson_storage_candidates/sleepyfish_cam2010095_full_v8_canonical_v3_20260730_v1/canonical_detection.zarr}"
fixture_root="${CRIMSON_KEYPOINT_QUALITY_FIXTURE_ROOT:-/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/crimson_storage_candidates/sleepyfish_cam2010095_full_v8_20260730/keypoints}"
start_frame="${CRIMSON_KEYPOINT_QUALITY_START_FRAME:-1000}"
check_only=false

usage() {
    cat <<'EOF'
Usage: scripts/launch_macos_keypoint_quality_timeline.sh [START_FRAME]
       scripts/launch_macos_keypoint_quality_timeline.sh --check

Launches Crimson paused against the full-duration keypoint-v2 fixture.
START_FRAME defaults to 1000. --check validates the app and mounted inputs
without opening Crimson.

Environment overrides:
  CRIMSON_KEYPOINT_QUALITY_APP
  CRIMSON_KEYPOINT_QUALITY_VIDEO
  CRIMSON_KEYPOINT_QUALITY_ANALYSIS
  CRIMSON_KEYPOINT_QUALITY_FIXTURE_ROOT
  CRIMSON_KEYPOINT_QUALITY_START_FRAME
EOF
}

case "${1:-}" in
    "")
        ;;
    --check)
        check_only=true
        shift
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        start_frame="$1"
        shift
        ;;
esac

if [[ "$#" -ne 0 ]]; then
    usage >&2
    exit 2
fi
if [[ ! "$start_frame" =~ ^[0-9]+$ ]]; then
    echo "START_FRAME must be an integer from 0 through 1187999." >&2
    exit 2
fi
start_frame="$((10#$start_frame))"
if ((start_frame >= 1188000)); then
    echo "START_FRAME must be an integer from 0 through 1187999." >&2
    exit 2
fi
if [[ ! -x "$app" ]]; then
    echo "Crimson executable not found: $app" >&2
    echo "Build it with: cmake --build build/macos-arm64-release --target Crimson -j6" >&2
    exit 2
fi

required_paths=(
    "$video"
    "$analysis"
    "$fixture_root/raw_keypoints.zarr"
    "$fixture_root/keypoint_quality.zarr"
    "$fixture_root/refined_keypoints.zarr"
    "$fixture_root/body_frame.zarr"
)
for path in "${required_paths[@]}"; do
    if [[ ! -e "$path" ]]; then
        echo "Required mounted input not found: $path" >&2
        echo "Confirm /Volumes/johnsonlab is mounted, then retry." >&2
        exit 2
    fi
done

echo "Crimson keypoint-quality fixture is ready."
echo "Start frame: $start_frame"
if [[ "$check_only" == true ]]; then
    exit 0
fi

exec "$app" \
    --video "$video" \
    --zarr "$analysis" \
    --start-paused "$start_frame" \
    --benchmark-keypoint-v2-raw \
        "$fixture_root/raw_keypoints.zarr" \
        raw_keypoints_sleepyfish_cam2010095_full_v8_20260730 \
        12c2697e071be42cbfa574625b66186a97a63ebc72eba5a3e290d6efb730ab42 \
    --benchmark-keypoint-v2-quality \
        "$fixture_root/keypoint_quality.zarr" \
        keypoint_quality_sleepyfish_cam2010095_full_v8_20260730 \
        a6b887d6a241dd021bbf410fefa5b091d9ece1b655a69f05e62f70fc0bed85b9 \
    --benchmark-keypoint-v2-refined \
        "$fixture_root/refined_keypoints.zarr" \
        refined_keypoints_sleepyfish_cam2010095_full_v8_20260730 \
        f6348f815eea9c3ae574e0fe142e7906185aa4d95e8207d041b8d116ecc63fae \
    --benchmark-keypoint-v2-body-frame \
        "$fixture_root/body_frame.zarr" \
        body_frame_sleepyfish_cam2010095_full_v8_20260730 \
        c409fbabc10b0625051c43853b469abfca5723a16d93ab5fcf42e40b9b106848
