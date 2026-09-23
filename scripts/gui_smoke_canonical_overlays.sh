#!/usr/bin/env bash
# Read-only August overlay capture. Starts and stops only its own GUI process.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
archive="${1:?Usage: gui_smoke_canonical_overlays.sh ARCHIVE.zarr FRAME}"
frame="${2:?Frame required}"
[[ -d "$archive" && "$frame" =~ ^[0-9]+$ ]] || { echo "Invalid archive/frame" >&2; exit 2; }
binary="${CRIMSON_REDGUI:-$repo_root/release/redgui}"
display="${CRIMSON_PLAYBACK_SMOKE_DISPLAY:-${DISPLAY:-:1}}"
xauthority="${CRIMSON_PLAYBACK_SMOKE_XAUTHORITY:-${XAUTHORITY:-}}"
env DISPLAY="$display" XAUTHORITY="$xauthority" xdpyinfo >/dev/null 2>&1 ||
  { echo "No authenticated X display; set CRIMSON_PLAYBACK_SMOKE_DISPLAY/XAUTHORITY" >&2; exit 2; }
case_dir="$(mktemp -d "${TMPDIR:-/tmp}/crimson-canonical-overlay-smoke.XXXXXX")"
echo "evidence=$case_dir"
cd "$case_dir"
timeout 90 env DISPLAY="$display" XAUTHORITY="$xauthority" \
  XDG_CONFIG_HOME="$case_dir/config" XDG_CACHE_HOME="$case_dir/cache" \
  "$binary" --zarr "$archive" \
  --ui-reference-state overlays --ui-reference-frame "$frame" \
  --ui-reference-ready-file "$case_dir/ready.json" --ui-reference-timeout 75 \
  --ui-reference-size 1600x1000 --swap-interval 0 --frame-cap-fps 60 \
  --no-mask-perf-log > "$case_dir/app.log" 2>&1 &
case_pid=$!
cleanup() {
  if kill -0 "$case_pid" 2>/dev/null; then kill -TERM "$case_pid" 2>/dev/null || true; fi
  wait "$case_pid" 2>/dev/null || true
}
trap cleanup EXIT
while kill -0 "$case_pid" 2>/dev/null; do
  if [[ -f "$case_dir/ready.json" ]]; then
    jq -e --argjson frame "$frame" '
      .state == "overlays" and .target_frame == $frame and
      .presented_frame == $frame and .current_frame == $frame and
      .bbox_query_frame == $frame and .stable_frames >= 60 and
      .canonical_overlays.query_frame == $frame and .canonical_overlays.draw_frame == $frame and
      (.canonical_overlays | .keypoints.state == "ready" and .masks.state == "ready" and
        .shapes.state == "ready" and .keypoints.frame == $frame and
        .masks.frame == $frame and .shapes.frame == $frame and
        .keypoint_primitives > 0 and .shape_primitives == .shape_expected_primitives and
        (.keypoints.instance_keys|sort) == (.masks.instance_keys|sort) and
        (.keypoints.instance_keys|sort) == (.shapes.instance_keys|sort)) and
      .overlays.component_fill_count == .canonical_overlays.mask_expected_fills' "$case_dir/ready.json" >/dev/null
    test -s "$case_dir/ready.json.png"
    echo "PASS frame=$frame evidence=$case_dir"
    exit 0
  fi
  sleep 0.1
done
set +e
wait "$case_pid"
status=$?
set -e
echo "FAIL frame=$frame status=$status evidence=$case_dir" >&2
tail -n 40 "$case_dir/app.log" >&2
exit 1
