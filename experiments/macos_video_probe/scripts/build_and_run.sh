#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  scripts/build_and_run.sh VIDEO [probe options]

Examples:
  scripts/build_and_run.sh /path/to/video.mp4
  scripts/build_and_run.sh /path/to/video.mp4 --duration 600 --buffer-frames 12

Probe options:
  --mode MODE          complete (default) or realtime
  --duration SECONDS    Exit after this much active playback time (default: run until Q)
  --buffer-frames N     Maximum retained native decode surfaces (default: 12)
  --metrics FILE        CSV output path (default: timestamped file under results/)
EOF
}

if [[ $# -lt 1 ]]; then
    usage >&2
    exit 2
fi

video=$1
shift
if [[ ! -f "$video" ]]; then
    printf 'Video does not exist: %s\n' "$video" >&2
    exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
probe_dir=$(cd -- "$script_dir/.." && pwd)
build_dir="$probe_dir/build"
mkdir -p "$probe_dir/results"

have_metrics=false
for argument in "$@"; do
    if [[ "$argument" == "--metrics" ]]; then
        have_metrics=true
        break
    fi
done

extra_args=("$@")
if [[ "$have_metrics" == false ]]; then
    timestamp=$(date +%Y%m%d-%H%M%S)
    extra_args+=(--metrics "$probe_dir/results/metrics-$timestamp.csv")
fi

cmake -S "$probe_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$(brew --prefix ffmpeg)"
cmake --build "$build_dir" --parallel

exec "$build_dir/crimson_macos_video_probe" "$video" "${extra_args[@]}"
