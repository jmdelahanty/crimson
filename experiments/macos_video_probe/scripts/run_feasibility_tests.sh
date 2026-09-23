#!/usr/bin/env bash
set -uo pipefail

usage() {
    cat <<'EOF'
Usage:
  scripts/run_feasibility_tests.sh PRIMARY_VIDEO [--secondary LABEL=VIDEO]... [--alignment CSV]

Examples:
  scripts/run_feasibility_tests.sh /path/to/main.mp4
  scripts/run_feasibility_tests.sh /path/to/main.mp4 \
    --secondary crop=/path/to/crop.mp4 \
    --secondary stimulus=/path/to/stimulus.mp4 \
    --alignment /path/to/alignment.csv
EOF
}

if [[ $# -lt 1 ]]; then
    usage >&2
    exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
probe_dir=$(cd -- "$script_dir/.." && pwd)
build_dir="$probe_dir/build"
timestamp=$(date +%Y%m%d-%H%M%S)
results_dir="$probe_dir/results/feasibility-$timestamp"
log="$results_dir/log.txt"
mkdir -p "$results_dir"

{
    printf 'Started: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'Architecture: %s\n' "$(uname -m)"
    sw_vers
    printf '\nCommand:'
    printf ' %q' "$0" "$@"
    printf '\n\n'

    status=0
    cmake -S "$probe_dir" -B "$build_dir" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release || status=$?
    if [[ "$status" -eq 0 ]]; then
        cmake --build "$build_dir" \
            --target crimson_avfoundation_feasibility --parallel || status=$?
    fi
    if [[ "$status" -eq 0 ]]; then
        "$build_dir/crimson_avfoundation_feasibility" "$@" || status=$?
    fi
    printf '\nFinished: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'Exit status: %d\n' "$status"
    printf 'Log: %s\n' "$log"
    exit "$status"
} 2>&1 | tee "$log"

exit "${PIPESTATUS[0]}"
