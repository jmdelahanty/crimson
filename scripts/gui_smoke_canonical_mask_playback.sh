#!/usr/bin/env bash
# Advancing-video mask coverage; owns only its child process and temporary state.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
archive="${1:?Usage: gui_smoke_canonical_mask_playback.sh ARCHIVE.zarr START:END [RENDER_FPS]}"
range="${2:?Frame range required}"
render_fps="${3:-60}"
[[ -d "$archive" && "$range" =~ ^([0-9]+):([0-9]+)$ ]] || { echo "Invalid archive/range" >&2; exit 2; }
start_frame="${BASH_REMATCH[1]}"
end_frame="${BASH_REMATCH[2]}"
[[ "$render_fps" == 30 || "$render_fps" == 60 ]] || { echo "Render FPS must be 30 or 60" >&2; exit 2; }
((end_frame > start_frame)) || { echo "End must follow start" >&2; exit 2; }
binary="${CRIMSON_REDGUI:-$repo_root/release/redgui}"
display="${CRIMSON_PLAYBACK_SMOKE_DISPLAY:-${DISPLAY:-:1}}"
xauthority="${CRIMSON_PLAYBACK_SMOKE_XAUTHORITY:-${XAUTHORITY:-}}"
env DISPLAY="$display" XAUTHORITY="$xauthority" xdpyinfo >/dev/null 2>&1 ||
  { echo "No authenticated X display" >&2; exit 2; }
case_dir="$(mktemp -d "${TMPDIR:-/tmp}/crimson-mask-playback-smoke.XXXXXX")"
echo "evidence=$case_dir"
cd "$case_dir"
timeout 90 env DISPLAY="$display" XAUTHORITY="$xauthority" \
  XDG_CONFIG_HOME="$case_dir/config" XDG_CACHE_HOME="$case_dir/cache" \
  "$binary" --zarr "$archive" --show-subject-masks \
  --playback-smoke "$range" --playback-smoke-warmup-seconds 8 \
  --playback-smoke-timeout 75 --swap-interval 0 --frame-cap-fps "$render_fps" \
  --playback-trace-log "$case_dir/playback.jsonl" --no-mask-perf-log \
  > "$case_dir/app.log" 2>&1
python3 "$repo_root/tools/check_canonical_mask_playback.py" \
  "$case_dir/playback.jsonl" "$start_frame" "$end_frame" \
  --grace-frames "${CRIMSON_MASK_PLAYBACK_GRACE_FRAMES:-0}" \
  --minimum-frames "${CRIMSON_MASK_PLAYBACK_MINIMUM_FRAMES:-60}" \
  | tee "$case_dir/coverage.json"
