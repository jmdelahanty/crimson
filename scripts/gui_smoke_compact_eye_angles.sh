#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
redgui="${CRIMSON_REDGUI:-"$repo_root/release/redgui"}"
zarr_path="${1:-${CRIMSON_COMPACT_EYE_ANGLE_SMOKE_ZARR:-/nvme1/recordings/2026-01-28T23-15-10Z_arena_2_Feeding/zarr/2026-01-28T23-15-10Z_arena_2_Feeding_analysis.zarr/}}"
eye_angle_run="${CRIMSON_COMPACT_EYE_ANGLE_RUN:-eye_angle_compact_dense_v2_canary_20260511_axisavail}"
timeout_s="${CRIMSON_COMPACT_EYE_ANGLE_SMOKE_TIMEOUT:-20}"
timestamp="$(date +%Y%m%d_%H%M%S)"
log_path="${CRIMSON_COMPACT_EYE_ANGLE_SMOKE_LOG:-/tmp/crimson_compact_eye_angle_smoke_${timestamp}.log}"

if [[ ! -x "$redgui" ]]; then
    echo "redgui executable not found: $redgui" >&2
    exit 2
fi

display="${CRIMSON_COMPACT_EYE_ANGLE_SMOKE_DISPLAY:-${DISPLAY:-:1}}"
xauthority="${CRIMSON_COMPACT_EYE_ANGLE_SMOKE_XAUTHORITY:-${XAUTHORITY:-/run/user/$(id -u)/.mutter-Xwaylandauth.9MIFN3}}"

if ! env DISPLAY="$display" XAUTHORITY="$xauthority" xdpyinfo >/dev/null 2>&1; then
    if [[ -z "${CRIMSON_COMPACT_EYE_ANGLE_SMOKE_DISPLAY:-}" ]]; then
        display=":1"
        xauthority="${CRIMSON_COMPACT_EYE_ANGLE_SMOKE_XAUTHORITY:-/run/user/$(id -u)/.mutter-Xwaylandauth.9MIFN3}"
    fi
fi

set +e
timeout "$timeout_s" \
    env DISPLAY="$display" \
        XAUTHORITY="$xauthority" \
        "$redgui" \
        --zarr "$zarr_path" \
        --eye-angle-run "$eye_angle_run" \
        --swap-interval 0 \
        --frame-cap-fps 120 \
        --no-mask-perf-log \
        >"$log_path" 2>&1
status=$?
set -e

if [[ "$status" -ne 0 && "$status" -ne 124 ]]; then
    echo "redgui exited with unexpected status $status" >&2
    echo "log: $log_path" >&2
    tail -n 120 "$log_path" >&2 || true
    exit "$status"
fi

required_patterns=(
    "Successfully loaded zarr file"
    "Eye angle run '$eye_angle_run' loaded"
    "layout compact_dense_v2"
    "[EyeAngleCompact] overlay fields:"
    "left_gaze_xy=roi"
    "right_gaze_xy=roi"
    "left_eye_angle_deg=roi+frame"
    "right_eye_angle_deg=roi+frame"
    "vergence_eye_angle_deg=roi+frame"
    "left_gaze_deg=roi+frame"
    "right_gaze_deg=roi+frame"
    "vergence_gaze_deg=roi+frame"
    "valid_frame=roi+frame"
)

for pattern in "${required_patterns[@]}"; do
    if ! grep -Fq "$pattern" "$log_path"; then
        echo "missing expected log pattern: $pattern" >&2
        echo "log: $log_path" >&2
        tail -n 160 "$log_path" >&2 || true
        exit 1
    fi
done

echo "compact eye-angle smoke passed"
echo "log: $log_path"
