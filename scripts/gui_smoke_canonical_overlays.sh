#!/usr/bin/env bash
# Read-only August overlay capture. Starts and stops only its own GUI process.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
archive="${1:?Usage: gui_smoke_canonical_overlays.sh ARCHIVE.zarr FRAME [default|contour_only|fill_only|body_contour_only]}"
frame="${2:?Frame required}"
mode="${3:-${CRIMSON_CANONICAL_OVERLAY_MODE:-default}}"
case "$mode" in
  default|contour_only|fill_only|body_contour_only) ;;
  *) echo "Invalid canonical overlay mode: $mode" >&2; exit 2 ;;
esac
require_shape_debug="${CRIMSON_CANONICAL_OVERLAY_REQUIRE_SHAPE_DEBUG:-0}"
[[ "$require_shape_debug" == 0 || "$require_shape_debug" == 1 ]] ||
  { echo "CRIMSON_CANONICAL_OVERLAY_REQUIRE_SHAPE_DEBUG must be 0 or 1" >&2; exit 2; }
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
  CRIMSON_CANONICAL_OVERLAY_MODE="$mode" \
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
    jq -e --argjson frame "$frame" --arg mode "$mode" \
      --argjson require_shape_debug "$require_shape_debug" '
      .state == "overlays" and .target_frame == $frame and
      .presented_frame == $frame and .current_frame == $frame and
      .bbox_query_frame == $frame and .stable_frames >= 60 and
      .canonical_overlays.query_frame == $frame and .canonical_overlays.draw_frame == $frame and
      (.canonical_overlays | .keypoints.state == "ready" and .masks.state == "ready" and
        .shapes.state == "ready" and .keypoints.frame == $frame and
        .masks.frame == $frame and .shapes.frame == $frame and
        .keypoint_primitives > 0 and .shape_primitives == .shape_expected_primitives and
        .mask_actual_contours == .mask_expected_contours and
        (.keypoints.instance_keys|sort) == (.masks.instance_keys|sort) and
        (.keypoints.instance_keys|sort) == (.shapes.instance_keys|sort)) and
      .overlays.component_fill_count == .canonical_overlays.mask_expected_fills and
      (if $mode == "contour_only" then
       .canonical_overlays.mask_expected_fills == 0 and
         .canonical_overlays.mask_contours.state == "ready" and
         .canonical_overlays.mask_contours.frame == $frame and
         .canonical_overlays.mask_contours.error == "" and
         .canonical_overlays.mask_contour_error == "" and
         (.canonical_overlays.mask_contours.instance_keys | sort) ==
           (.canonical_overlays.masks.instance_keys | sort) and
         .canonical_overlays.mask_actual_contours > 0 and
         .canonical_overlays.mask_contour_payload_reads > 0
       elif $mode == "fill_only" then
         .canonical_overlays.mask_expected_fills > 0 and
         .canonical_overlays.mask_actual_contours == 0 and
         .canonical_overlays.mask_contour_payload_reads == 0
       elif $mode == "body_contour_only" then
         .canonical_overlays.mask_expected_fills == 0 and
         .canonical_overlays.mask_contours.state == "ready" and
         .canonical_overlays.mask_contours.frame == $frame and
         .canonical_overlays.mask_contours.error == "" and
         .canonical_overlays.mask_contour_error == "" and
         .canonical_overlays.mask_actual_contours > 0 and
         .canonical_overlays.mask_contour_payload_reads > 0 and
         (.canonical_overlays.mask_expected_contour_labels | keys) == ["subject_body"]
       else .canonical_overlays.mask_actual_contours == 0 and
            .canonical_overlays.mask_contour_payload_reads == 0 end) and
      (if $require_shape_debug == 1 then
         .canonical_overlays.shape_expected_labels.bspline_debug > 0 and
         .canonical_overlays.shape_expected_labels.control_points > 0 and
         .canonical_overlays.shape_expected_labels.tail_samples > 0 and
         .canonical_overlays.shape_expected_labels.tail_normals > 0
       else true end)' "$case_dir/ready.json" >/dev/null
    test -s "$case_dir/ready.json.png"
    echo "PASS frame=$frame mode=$mode evidence=$case_dir"
    exit 0
  fi
  sleep 0.1
done
set +e
wait "$case_pid"
status=$?
set -e
echo "FAIL frame=$frame mode=$mode status=$status evidence=$case_dir" >&2
tail -n 40 "$case_dir/app.log" >&2
exit 1
