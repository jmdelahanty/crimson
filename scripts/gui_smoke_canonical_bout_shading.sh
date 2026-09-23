#!/usr/bin/env bash
# Read-only canonical speed shading capture; controls only its own GUI process.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
archive="${1:?Usage: gui_smoke_canonical_bout_shading.sh ARCHIVE.zarr FRAME [on|off]}"
frame="${2:?Frame required}"
mode="${3:-on}"
case "$mode" in
  on) enabled=1 ;;
  off) enabled=0 ;;
  *) echo "Invalid shading mode: $mode" >&2; exit 2 ;;
esac
[[ -d "$archive" && "$frame" =~ ^[0-9]+$ ]] ||
  { echo "Invalid archive/frame" >&2; exit 2; }
binary="${CRIMSON_REDGUI:-$repo_root/release/redgui}"
display="${CRIMSON_PLAYBACK_SMOKE_DISPLAY:-${DISPLAY:-:1}}"
xauthority="${CRIMSON_PLAYBACK_SMOKE_XAUTHORITY:-${XAUTHORITY:-}}"
env DISPLAY="$display" XAUTHORITY="$xauthority" xdpyinfo >/dev/null 2>&1 ||
  { echo "No authenticated X display; set CRIMSON_PLAYBACK_SMOKE_DISPLAY/XAUTHORITY" >&2; exit 2; }
case_dir="$(mktemp -d "${TMPDIR:-/tmp}/crimson-canonical-bout-shading.XXXXXX")"
echo "evidence=$case_dir"
cd "$case_dir"
timeout 90 env DISPLAY="$display" XAUTHORITY="$xauthority" \
  CRIMSON_CANONICAL_BOUT_SHADING="$enabled" \
  XDG_CONFIG_HOME="$case_dir/config" XDG_CACHE_HOME="$case_dir/cache" \
  "$binary" --zarr "$archive" \
  --ui-reference-state analysis-eye --ui-reference-frame "$frame" \
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
    jq -e --argjson frame "$frame" --argjson enabled "$enabled" '
      .state == "analysis-eye" and .target_frame == $frame and
      .presented_frame == $frame and .current_frame == $frame and
      .stable_frames >= 60 and .analysis.canonical == true and
      (.canonical_timelines |
        .frame == $frame and .motion.state == "ready" and
        .bouts.state == "ready" and .bouts.interval_count > 0 and
        .speed_shading.frame == $frame and
        .speed_shading.enabled == ($enabled == 1) and
        (if $enabled == 1 then
          .speed_shading.bands_drawn > 0 and
          .speed_shading.core_bands_drawn > 0
         else .speed_shading.bands_drawn == 0 and
              .speed_shading.core_bands_drawn == 0 end))' \
      "$case_dir/ready.json" >/dev/null
    test -s "$case_dir/ready.json.png"
    echo "PASS frame=$frame shading=$mode evidence=$case_dir"
    exit 0
  fi
  sleep 0.1
done
set +e
wait "$case_pid"
status=$?
set -e
echo "FAIL frame=$frame shading=$mode status=$status evidence=$case_dir" >&2
tail -n 40 "$case_dir/app.log" >&2
exit 1
