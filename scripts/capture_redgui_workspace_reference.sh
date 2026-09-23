#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  capture_redgui_workspace_reference.sh REDGUI ZARR OUTPUT_DIR STATE [WIDTH HEIGHT]

ZARR may be '-' to capture the empty workspace. The script launches redgui in
a fresh working directory, captures the undecorated X11 client, and writes a
PNG, window metadata, the generated imgui.ini (when present), and the run log.

Environment:
  DISPLAY, XAUTHORITY                 Authenticated X11 display.
  CRIMSON_REFERENCE_SETTLE_SECONDS   Delay after the client appears (default 15).
  CRIMSON_REFERENCE_LOAD_TIMEOUT     Archive-load timeout in seconds (default 180).
  CRIMSON_REFERENCE_IMGUI_INI        Optional ini profile copied before launch.
  CRIMSON_REFERENCE_SOURCE_REVISION  Source revision recorded in metadata.
  CRIMSON_REFERENCE_CAPTURE_CLASS    deterministic or observational.
  CRIMSON_REFERENCE_UI_STATE         Optional redgui UI reference preset.
  CRIMSON_REFERENCE_FRAME            Exact frame required with UI_STATE.
  CRIMSON_REFERENCE_UI_TIMEOUT       Ready-marker timeout in seconds (default 60).
  CRIMSON_REFERENCE_MASK_PERF_LOG    Preserve per-frame mask metrics when 1.
  CRIMSON_REFERENCE_KEEP_RUN_DIR     Keep temporary files after failure when 1.
EOF
}

if [[ $# -lt 4 || $# -gt 6 ]]; then
    usage >&2
    exit 2
fi

redgui="$1"
zarr_path="$2"
output_dir="$3"
state_name="$4"
width="${5:-1920}"
height="${6:-1080}"
settle_seconds="${CRIMSON_REFERENCE_SETTLE_SECONDS:-15}"
load_timeout="${CRIMSON_REFERENCE_LOAD_TIMEOUT:-180}"
capture_class="${CRIMSON_REFERENCE_CAPTURE_CLASS:-deterministic}"
ui_reference_state="${CRIMSON_REFERENCE_UI_STATE:-}"
ui_reference_frame="${CRIMSON_REFERENCE_FRAME:-}"
ui_reference_timeout="${CRIMSON_REFERENCE_UI_TIMEOUT:-60}"
mask_perf_log="${CRIMSON_REFERENCE_MASK_PERF_LOG:-0}"
keep_run_dir="${CRIMSON_REFERENCE_KEEP_RUN_DIR:-0}"
display="${DISPLAY:-}"
xauthority="${XAUTHORITY:-}"

if [[ ! -x "$redgui" ]]; then
    echo "redgui executable not found: $redgui" >&2
    exit 2
fi
if [[ "$zarr_path" != "-" && ! -d "$zarr_path" ]]; then
    echo "Zarr archive not found: $zarr_path" >&2
    exit 2
fi
if [[ -z "$display" || -z "$xauthority" ]]; then
    echo "DISPLAY and XAUTHORITY must identify an authenticated X11 session." >&2
    exit 2
fi
if [[ "$keep_run_dir" != "0" && "$keep_run_dir" != "1" ]]; then
    echo "CRIMSON_REFERENCE_KEEP_RUN_DIR must be 0 or 1." >&2
    exit 2
fi
if [[ "$mask_perf_log" != "0" && "$mask_perf_log" != "1" ]]; then
    echo "CRIMSON_REFERENCE_MASK_PERF_LOG must be 0 or 1." >&2
    exit 2
fi
if [[ -n "$ui_reference_state" || -n "$ui_reference_frame" ]]; then
    if [[ -z "$ui_reference_state" ||
          ! "$ui_reference_frame" =~ ^[0-9]+$ ]]; then
        echo "CRIMSON_REFERENCE_UI_STATE and a non-negative CRIMSON_REFERENCE_FRAME must be set together." >&2
        exit 2
    fi
    if [[ ! "$ui_reference_timeout" =~ ^[0-9]+([.][0-9]+)?$ ]] ||
        ! awk -v value="$ui_reference_timeout" 'BEGIN { exit !(value > 0) }'; then
        echo "CRIMSON_REFERENCE_UI_TIMEOUT must be a positive number." >&2
        exit 2
    fi
fi

for command in jq sha256sum xdotool xdpyinfo xprop xwd convert; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "required capture command not found: $command" >&2
        exit 2
    fi
done

if ! env DISPLAY="$display" XAUTHORITY="$xauthority" \
    xdpyinfo >/dev/null 2>&1; then
    echo "X11 authentication failed for DISPLAY=$display." >&2
    exit 3
fi

mkdir -p "$output_dir"
output_dir="$(cd "$output_dir" && pwd)"
run_dir="$(mktemp -d "${TMPDIR:-/tmp}/crimson-phase5l-reference.XXXXXX")"
log_path="$run_dir/redgui.log"
ready_path="$run_dir/ui-reference-ready.json"
pid=""
capture_succeeded=0

cleanup() {
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    if [[ "$keep_run_dir" == "1" && "$capture_succeeded" != "1" ]]; then
        echo "preserved failed capture run directory: $run_dir" >&2
    else
        rm -rf "$run_dir"
    fi
}
trap cleanup EXIT

if [[ -n "${CRIMSON_REFERENCE_IMGUI_INI:-}" ]]; then
    cp "$CRIMSON_REFERENCE_IMGUI_INI" "$run_dir/imgui.ini"
fi

args=(--swap-interval 0 --frame-cap-fps 60)
if [[ "$mask_perf_log" == "1" ]]; then
    args+=(
        --mask-perf-log "$run_dir/mask-perf.jsonl"
        --mask-perf-sample-every 1
    )
else
    args+=(--no-mask-perf-log)
fi
if [[ "$zarr_path" != "-" ]]; then
    args=(--zarr "$zarr_path" "${args[@]}")
fi
if [[ -n "$ui_reference_state" ]]; then
    args+=(
        --ui-reference-state "$ui_reference_state"
        --ui-reference-frame "$ui_reference_frame"
        --ui-reference-ready-file "$ready_path"
        --ui-reference-timeout "$ui_reference_timeout"
    )
fi

(
    cd "$run_dir"
    exec env DISPLAY="$display" XAUTHORITY="$xauthority" \
        "$redgui" "${args[@]}" >"$log_path" 2>&1
) &
pid=$!

find_client_window() {
    local candidate candidate_pid candidate_class
    while read -r candidate; do
        [[ -n "$candidate" ]] || continue
        candidate_pid="$(
            xprop -id "$candidate" _NET_WM_PID 2>/dev/null |
                awk -F' = ' 'NF == 2 {print $2}'
        )"
        candidate_class="$(xprop -id "$candidate" WM_CLASS 2>/dev/null || true)"
        if [[ "$candidate_pid" == "$pid" && "$candidate_class" == *'"Red", "Red"'* ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done < <(
        xprop -root _NET_CLIENT_LIST 2>/dev/null |
            sed -e 's/^.*# //' -e 's/,//g' |
            tr ' ' '\n'
    )
    return 1
}

window_id=""
for _ in $(seq 1 120); do
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "redgui exited before its window appeared." >&2
        tail -n 120 "$log_path" >&2 || true
        exit 4
    fi
    window_id="$(find_client_window || true)"
    [[ -n "$window_id" ]] && break
    sleep 0.25
done
if [[ -z "$window_id" ]]; then
    echo "timed out waiting for the redgui X11 client." >&2
    exit 4
fi

env DISPLAY="$display" XAUTHORITY="$xauthority" \
    xdotool windowsize "$window_id" "$width" "$height"

if [[ "$zarr_path" != "-" ]]; then
    load_deadline=$((SECONDS + load_timeout))
    until grep -Fq "Successfully loaded zarr file" "$log_path"; do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "redgui exited before the archive finished loading." >&2
            tail -n 120 "$log_path" >&2 || true
            exit 4
        fi
        if ((SECONDS >= load_deadline)); then
            echo "timed out waiting for the archive to load." >&2
            tail -n 120 "$log_path" >&2 || true
            exit 4
        fi
        sleep 0.25
    done
fi

ui_reference_json="null"
if [[ -n "$ui_reference_state" ]]; then
    reference_deadline=$((SECONDS + ${ui_reference_timeout%.*} + 10))
    until [[ -f "$ready_path" ]]; do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "redgui exited before the UI reference became ready." >&2
            tail -n 160 "$log_path" >&2 || true
            exit 4
        fi
        if ((SECONDS >= reference_deadline)); then
            echo "timed out waiting for the UI reference ready marker." >&2
            tail -n 160 "$log_path" >&2 || true
            exit 4
        fi
        sleep 0.1
    done
    if ! jq -e \
        --arg state "$ui_reference_state" \
        --argjson frame "$ui_reference_frame" \
        '.format == "crimson_ui_reference_v1" and
         .state == $state and
         .write_contract == "read-only" and
         .target_frame == $frame and
         .presented_frame == $frame and
         .current_frame == $frame and
         .slider_frame == $frame and
         .stable_frames >= 60 and
         .buffers.camera_capacity > 0 and
         .buffers.camera_valid == .buffers.camera_capacity and
         ($state != "stimulus-debug" or
          (.buffers.stimulus_capacity > 0 and
           .buffers.stimulus_valid <= .buffers.stimulus_capacity and
           .stimulus.loaded and
           .stimulus.debug_windows and
           .stimulus.target_frame == .stimulus.presented_frame)) and
         ($state != "crop-preview" or
          (.crop.ready and
           .crop.source_frame == $frame and
           .crop.width > 0 and
           .crop.height > 0)) and
         (($state != "overlays" and
           ($state != "analysis-eye" or .analysis.canonical == true)) or
          (.overlays.optional_overlay_status == "optional overlays ready" and
           .overlays.visible_roi_count > 0 and
           .overlays.component_fill_count > 0 and
           .overlays.contours_drawn > 0 and
           .overlays.axes_drawn > 0 and
           ((.overlays.gaze_rays_drawn > 0) or
            (.overlays.visual_cones_drawn > 0)) and
           .overlays.angle_labels_drawn > 0)) and
         ($state != "polar" or
          (.polar.ready and
           .polar.status == "ready" and
           .polar.availability == "ready" and
           .polar.requested_frame == $frame and
           .polar.source_frame == $frame and
           .polar.point_count == 2 and
           (.polar.points | length) == 2 and
           (.polar.semantic_signature |
            startswith("crimson.polar-scene-semantics.v1")) and
           ([.polar.text[].content] |
            contains(["front", "left", "right", "behind",
                      "Chaser bearing f=1024"])))) and
         ($state != "stimulus-overlay" or
          (.stimulus_camera_overlay.ready and
           .stimulus_camera_overlay.status == "ready" and
           .stimulus_camera_overlay.availability == "ready" and
           .stimulus_camera_overlay.requested_frame == $frame and
           .stimulus_camera_overlay.source_frame == $frame and
           .stimulus_camera_overlay.event_source_frame == $frame and
           .stimulus_camera_overlay.primitive_count > 0 and
           .stimulus_camera_overlay.text_count > 0 and
           (.stimulus_camera_overlay.semantic_signature |
            startswith("crimson.stimulus-camera-overlay-semantics.v1")))) and
         ($state != "analysis-eye" or .analysis.canonical != true or
          (.canonical_timelines.frame == $frame and
           .canonical_timelines.eye.state == "ready" and
           .canonical_timelines.eye.finite_points > 0 and
           .canonical_timelines.motion.state == "ready" and
           .canonical_timelines.motion.finite_points > 0 and
           (.canonical_timelines.bouts.state == "ready" or
            .canonical_timelines.bouts.state == "empty") and
           ([.canonical_timelines.eye, .canonical_timelines.motion,
             .canonical_timelines.bouts] | all(
                .source != "" and .first_frame <= $frame and
                .last_frame >= $frame)))) and
         ($state != "analysis-eye" or
          (.analysis.state_applied and
           .analysis.show_eye and
           (.analysis.show_tail_angle | not) and
           (.analysis.show_tail_deflection | not) and
           (.analysis.show_tail_curvature | not) and
           (.analysis.show_stimulus_context | not))) and
         ($state != "analysis-tail-stimulus" or
          (.analysis.state_applied and
           (.analysis.show_eye | not) and
           .analysis.show_tail_angle and
           .analysis.show_tail_deflection and
           .analysis.show_tail_curvature and
           .analysis.show_stimulus_context)) and
         .rendered_image.surface == "opengl_front_buffer" and
         .rendered_image.width == .framebuffer_size.width and
         .rendered_image.height == .framebuffer_size.height' \
        "$ready_path" >/dev/null; then
        echo "UI reference ready marker failed validation." >&2
        cat "$ready_path" >&2
        exit 4
    fi
    rendered_image_path="$(jq -r '.rendered_image.path' "$ready_path")"
    if [[ ! -f "$rendered_image_path" ]]; then
        echo "UI reference rendered image is missing: $rendered_image_path" >&2
        exit 4
    fi
    ui_reference_json="$(cat "$ready_path")"
fi

sleep "$settle_seconds"

prefix="$output_dir/$state_name"
env DISPLAY="$display" XAUTHORITY="$xauthority" \
    xdotool getwindowgeometry --shell "$window_id" >"$prefix.geometry.env"
env DISPLAY="$display" XAUTHORITY="$xauthority" \
    xprop -id "$window_id" >"$prefix.xprop.txt"
env DISPLAY="$display" XAUTHORITY="$xauthority" \
    xwininfo -id "$window_id" >"$prefix.xwininfo.txt"
env DISPLAY="$display" XAUTHORITY="$xauthority" \
    xwd -silent -id "$window_id" -out "$run_dir/capture.xwd"
capture_surface="x11_client"
x11_png_sha256=""
if [[ -n "$ui_reference_state" ]]; then
    convert "$run_dir/capture.xwd" "$prefix.x11.png"
    cp "$rendered_image_path" "$prefix.png"
    capture_surface="application_gl_front_buffer"
    x11_png_sha256="$(sha256sum "$prefix.x11.png" | awk '{print $1}')"
    (
        cd "$output_dir"
        sha256sum "$state_name.png" "$state_name.x11.png"
    ) >"$prefix.sha256.txt"
else
    convert "$run_dir/capture.xwd" "$prefix.png"
    (
        cd "$output_dir"
        sha256sum "$state_name.png"
    ) >"$prefix.sha256.txt"
fi

exe_sha256="$(sha256sum "$redgui" | awk '{print $1}')"
png_sha256="$(sha256sum "$prefix.png" | awk '{print $1}')"
source_revision="${CRIMSON_REFERENCE_SOURCE_REVISION:-unknown}"
geometry="$(cat "$prefix.geometry.env")"
zarr_mtime=""
if [[ "$zarr_path" != "-" ]]; then
    zarr_mtime="$(stat -c '%y' "$zarr_path" 2>/dev/null || stat -f '%Sm' "$zarr_path")"
fi

jq -n \
    --arg schema "crimson.phase5l.reference.v1" \
    --arg captured_at "$(date --iso-8601=seconds 2>/dev/null || date -Iseconds)" \
    --arg state "$state_name" \
    --arg capture_class "$capture_class" \
    --arg source_revision "$source_revision" \
    --arg executable "$redgui" \
    --arg executable_sha256 "$exe_sha256" \
    --arg archive "$zarr_path" \
    --arg archive_mtime "$zarr_mtime" \
    --arg display "$display" \
    --arg xauthority "$xauthority" \
    --arg window_id "$window_id" \
    --arg geometry "$geometry" \
    --arg capture_surface "$capture_surface" \
    --arg png_sha256 "$png_sha256" \
    --arg x11_png_sha256 "$x11_png_sha256" \
    --argjson ui_reference "$ui_reference_json" \
    --argjson width "$width" \
    --argjson height "$height" \
    '{
        schema: $schema,
        captured_at: $captured_at,
        state: $state,
        capture_class: $capture_class,
        source_revision: $source_revision,
        executable: $executable,
        executable_sha256: $executable_sha256,
        archive: $archive,
        archive_mtime: $archive_mtime,
        requested_client_size: {width: $width, height: $height},
        x11: {
            display: $display,
            xauthority_path: $xauthority,
            client_window_id: $window_id,
            geometry: $geometry,
            png_sha256: (if $x11_png_sha256 == "" then null else $x11_png_sha256 end)
        },
        capture_surface: $capture_surface,
        ui_reference: $ui_reference,
        png_sha256: $png_sha256
    }' >"$prefix.json"

cp "$log_path" "$prefix.redgui.log"
if [[ -f "$run_dir/imgui.ini" ]]; then
    cp "$run_dir/imgui.ini" "$prefix.imgui.ini"
fi
if [[ -f "$ready_path" ]]; then
    cp "$ready_path" "$prefix.ui-reference.json"
fi
if [[ -f "$run_dir/mask-perf.jsonl" ]]; then
    cp "$run_dir/mask-perf.jsonl" "$prefix.mask-perf.jsonl"
fi

kill -TERM "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
pid=""
capture_succeeded=1

echo "captured: $prefix.png"
echo "metadata: $prefix.json"
