#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
    printf 'Usage: %s VIDEO [--duration SECONDS] [--metrics FILE]\n' "$0" >&2
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
    extra_args+=(--metrics "$probe_dir/results/avfoundation-$timestamp.csv")
fi

cmake -S "$probe_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --target crimson_avfoundation_metal_probe --parallel

exec "$build_dir/crimson_avfoundation_metal_probe" "$video" "${extra_args[@]}"
