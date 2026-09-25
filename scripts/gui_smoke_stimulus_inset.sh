#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
redgui="${CRIMSON_REDGUI:-"$repo_root/release/redgui"}"
zarr_path="${1:-${CRIMSON_STIMULUS_SMOKE_ZARR:-/nvme1/recordings/2026-01-28T19-22-28Z_arena_1_DefaultScreen/zarr/2026-01-28T19-22-28Z_arena_1_DefaultScreen_analysis.zarr/}}"
timeout_s="${CRIMSON_STIMULUS_SMOKE_TIMEOUT:-10}"
timestamp="$(date +%Y%m%d_%H%M%S)"
log_path="${CRIMSON_STIMULUS_SMOKE_LOG:-/tmp/crimson_stimulus_inset_smoke_${timestamp}.log}"

if [[ ! -x "$redgui" ]]; then
    echo "redgui executable not found: $redgui" >&2
    exit 2
fi

display="${CRIMSON_STIMULUS_SMOKE_DISPLAY:-${DISPLAY:-:1}}"
xauthority="${CRIMSON_STIMULUS_SMOKE_XAUTHORITY:-${XAUTHORITY:-/run/user/$(id -u)/.mutter-Xwaylandauth.9MIFN3}}"

if ! env DISPLAY="$display" XAUTHORITY="$xauthority" xdpyinfo >/dev/null 2>&1; then
    if [[ -z "${CRIMSON_STIMULUS_SMOKE_DISPLAY:-}" ]]; then
        display=":1"
        xauthority="${CRIMSON_STIMULUS_SMOKE_XAUTHORITY:-/run/user/$(id -u)/.mutter-Xwaylandauth.9MIFN3}"
    fi
fi

set +e
timeout "$timeout_s" \
    env DISPLAY="$display" \
        XAUTHORITY="$xauthority" \
        CRIMSON_STIMULUS_DEBUG_LOGS=1 \
        "$redgui" \
        --zarr "$zarr_path" \
        --swap-interval 0 \
        --frame-cap-fps 120 \
        --no-mask-perf-log \
        >"$log_path" 2>&1
status=$?
set -e

if [[ "$status" -ne 0 && "$status" -ne 124 ]]; then
    echo "redgui exited with unexpected status $status" >&2
    echo "log: $log_path" >&2
    tail -n 80 "$log_path" >&2 || true
    exit "$status"
fi

required_patterns=(
    "Successfully loaded zarr file"
    "[StimulusSteps] Loaded"
    "[Stimulus] Auto-loaded stimulus video"
    "[StimulusPresentation] update"
)

for pattern in "${required_patterns[@]}"; do
    if ! grep -Fq "$pattern" "$log_path"; then
        echo "missing expected log pattern: $pattern" >&2
        echo "log: $log_path" >&2
        tail -n 120 "$log_path" >&2 || true
        exit 1
    fi
done

echo "stimulus inset smoke passed"
echo "log: $log_path"
