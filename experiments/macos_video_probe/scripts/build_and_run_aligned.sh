#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  scripts/build_and_run_aligned.sh \
    --main VIDEO --crop VIDEO --stimulus VIDEO --alignment CSV [--duration S]
EOF
}

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
probe_dir=$(cd -- "$script_dir/.." && pwd)
build_dir="$probe_dir/build"
mkdir -p "$probe_dir/results"

args=("$@")
have_metrics=false
for argument in "$@"; do
    [[ "$argument" == "--metrics" ]] && have_metrics=true
done
if [[ "$have_metrics" == false ]]; then
    timestamp=$(date +%Y%m%d-%H%M%S)
    args+=(--metrics "$probe_dir/results/aligned-playback-$timestamp.csv")
fi

if [[ $# -lt 8 ]]; then usage >&2; exit 2; fi
cmake -S "$probe_dir" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --target crimson_aligned_playback_probe --parallel
exec "$build_dir/crimson_aligned_playback_probe" "${args[@]}"
