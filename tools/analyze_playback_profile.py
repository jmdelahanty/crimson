#!/usr/bin/env python3
"""
Analyze Crimson playback profiler CSV output and estimate likely bottlenecks.

Usage:
    python tools/analyze_playback_profile.py /path/to/crimson-playback-profile.csv
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from statistics import mean, median
from typing import Any


NUMERIC_COLUMNS = {
    "elapsed_s": float,
    "wall_epoch_ms": float,
    "play_video": int,
    "set_playback_speed": float,
    "inst_speed": float,
    "video_fps": float,
    "requested_camera_frame": int,
    "displayed_camera_frame": int,
    "current_frame_num": int,
    "min_decoded_camera_frame": int,
    "camera_decode_gap_frames": int,
    "camera_decode_convert_ms": float,
    "camera_decode_wait_ms": float,
    "camera_decode_write_ms": float,
    "camera_decode_pipeline_ms": float,
    "visible_camera_count": int,
    "playback_preview_active": int,
    "camera_upload_count": int,
    "camera_upload_ms": float,
    "camera_texture_resize_ms": float,
    "camera_preview_resize_ms": float,
    "camera_pbo_copy_ms": float,
    "camera_texture_upload_ms": float,
    "camera_plot_image_ui_ms": float,
    "camera_overlay_ui_ms": float,
    "camera_scene_ui_ms": float,
    "stimulus_window_ui_ms": float,
    "stimulus_timeline_ui_ms": float,
    "movement_timeline_ui_ms": float,
    "gl_draw_ms": float,
    "swap_ms": float,
    "frame_loop_ms": float,
    "ui_build_ms": float,
    "imgui_render_ms": float,
    "imgui_draw_cmd_count": int,
    "imgui_draw_list_count": int,
    "imgui_total_vtx_count": int,
    "imgui_total_idx_count": int,
    "stimulus_loaded": int,
    "stimulus_target_frame": int,
    "stimulus_latest_decoded": int,
    "stimulus_last_displayed": int,
    "stimulus_buffered_frames": int,
    "stimulus_progress_gap_frames": int,
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze Crimson playback profiler CSV output."
    )
    parser.add_argument("csv_path", type=Path, help="Path to crimson perf CSV")
    parser.add_argument(
        "--meta",
        type=Path,
        default=None,
        help="Optional metadata sidecar. Defaults to adjacent .meta.json",
    )
    parser.add_argument(
        "--json-output",
        type=Path,
        default=None,
        help="Optional JSON summary output path",
    )
    parser.add_argument(
        "--top-stalls",
        type=int,
        default=8,
        help="Number of worst frame-loop stalls to print",
    )
    return parser.parse_args()


def parse_numeric(value: str, caster: type) -> float:
    value = value.strip()
    if value == "":
        return math.nan
    try:
        return caster(value)
    except ValueError:
        return math.nan


def load_rows(csv_path: Path) -> list[dict[str, Any]]:
    with csv_path.open("r", newline="") as handle:
        reader = csv.DictReader(handle)
        rows: list[dict[str, Any]] = []
        for row in reader:
            parsed: dict[str, Any] = {}
            for key, value in row.items():
                if key is None:
                    continue
                if key in NUMERIC_COLUMNS:
                    parsed[key] = parse_numeric(value or "", NUMERIC_COLUMNS[key])
                else:
                    parsed[key] = value
            rows.append(parsed)
    return rows


def load_metadata(meta_path: Path | None) -> dict[str, Any] | None:
    if meta_path is None or not meta_path.exists():
        return None
    with meta_path.open("r") as handle:
        return json.load(handle)


def finite(values: list[Any]) -> list[float]:
    return [
        float(value)
        for value in values
        if isinstance(value, (int, float)) and math.isfinite(float(value))
    ]


def percentile(values: list[float], pct: float) -> float | None:
    vals = sorted(finite(values))
    if not vals:
        return None
    if len(vals) == 1:
        return vals[0]
    index = (len(vals) - 1) * pct
    lower = math.floor(index)
    upper = math.ceil(index)
    if lower == upper:
        return vals[int(index)]
    frac = index - lower
    return vals[lower] * (1.0 - frac) + vals[upper] * frac


def describe(values: list[Any]) -> dict[str, float] | None:
    vals = finite(values)
    if not vals:
        return None
    return {
        "mean": mean(vals),
        "median": median(vals),
        "p90": percentile(vals, 0.90) or math.nan,
        "p95": percentile(vals, 0.95) or math.nan,
        "max": max(vals),
    }


def safe_ratio(numerator: float, denominator: float) -> float:
    if not math.isfinite(numerator) or not math.isfinite(denominator) or denominator == 0.0:
        return math.nan
    return numerator / denominator


def clamp01(value: float) -> float:
    if not math.isfinite(value):
        return 0.0
    return max(0.0, min(value, 1.0))


def format_stat(value: float | None, unit: str = "") -> str:
    if value is None or not math.isfinite(value):
        return "n/a"
    suffix = f" {unit}" if unit else ""
    return f"{value:.3f}{suffix}"


def score_decode_bound(rows: list[dict[str, Any]], budget_ms: float | None) -> float:
    positive_gap = [
        row
        for row in rows
        if isinstance(row.get("camera_decode_gap_frames"), (int, float))
        and float(row["camera_decode_gap_frames"]) > 0
    ]
    lag_fraction = safe_ratio(float(len(positive_gap)), float(len(rows)))
    gap_p90 = percentile([row.get("camera_decode_gap_frames", math.nan) for row in rows], 0.90)
    loop_p95 = percentile([row.get("frame_loop_ms", math.nan) for row in rows], 0.95)
    budget_ratio = safe_ratio(loop_p95 or math.nan, budget_ms or math.nan)
    score = 0.0
    if math.isfinite(lag_fraction):
        score += lag_fraction * 0.7
    if gap_p90 is not None and math.isfinite(gap_p90):
        score += min(gap_p90 / 60.0, 1.0) * 0.3
    if math.isfinite(budget_ratio) and budget_ratio > 1.0:
        score += min((budget_ratio - 1.0) / 2.0, 0.3)
    return clamp01(score)


def score_upload_bound(rows: list[dict[str, Any]], budget_ms: float | None) -> float:
    upload_p90 = percentile([row.get("camera_upload_ms", math.nan) for row in rows], 0.90)
    draw_p90 = percentile([row.get("gl_draw_ms", math.nan) for row in rows], 0.90)
    if upload_p90 is None or not math.isfinite(upload_p90):
        return 0.0
    dominance = 0.0
    if draw_p90 is not None and math.isfinite(draw_p90) and upload_p90 > draw_p90:
        dominance = min((upload_p90 - draw_p90) / max(upload_p90, 1.0), 1.0)
    budget_ratio = safe_ratio(upload_p90, budget_ms or math.nan)
    score = min(upload_p90 / 12.0, 1.0) * 0.5
    if math.isfinite(budget_ratio):
        score += min(budget_ratio, 1.0) * 0.4
    score += dominance * 0.2
    return clamp01(score)


def score_draw_bound(rows: list[dict[str, Any]], budget_ms: float | None) -> float:
    draw_p90 = percentile([row.get("gl_draw_ms", math.nan) for row in rows], 0.90)
    swap_p90 = percentile([row.get("swap_ms", math.nan) for row in rows], 0.90)
    upload_p90 = percentile([row.get("camera_upload_ms", math.nan) for row in rows], 0.90)
    if draw_p90 is None or not math.isfinite(draw_p90):
        return 0.0
    dominance = 0.0
    peers = [value for value in [swap_p90, upload_p90] if value is not None and math.isfinite(value)]
    if peers and draw_p90 > max(peers):
        dominance = min((draw_p90 - max(peers)) / max(draw_p90, 1.0), 1.0)
    budget_ratio = safe_ratio(draw_p90, budget_ms or math.nan)
    score = min(draw_p90 / 16.0, 1.0) * 0.5
    if math.isfinite(budget_ratio):
        score += min(budget_ratio, 1.0) * 0.4
    score += dominance * 0.2
    return clamp01(score)


def score_ui_build_bound(rows: list[dict[str, Any]], budget_ms: float | None) -> float:
    ui_p90 = percentile([row.get("ui_build_ms", math.nan) for row in rows], 0.90)
    if ui_p90 is None or not math.isfinite(ui_p90):
        return 0.0
    budget_ratio = safe_ratio(ui_p90, budget_ms or math.nan)
    score = min(ui_p90 / 16.0, 1.0) * 0.5
    if math.isfinite(budget_ratio):
        score += min(budget_ratio, 1.0) * 0.5
    return clamp01(score)


def score_camera_scene_ui_bound(
    rows: list[dict[str, Any]], budget_ms: float | None
) -> float:
    scene_ui_p90 = percentile(
        [row.get("camera_scene_ui_ms", math.nan) for row in rows], 0.90
    )
    if scene_ui_p90 is None or not math.isfinite(scene_ui_p90):
        return 0.0
    budget_ratio = safe_ratio(scene_ui_p90, budget_ms or math.nan)
    score = min(scene_ui_p90 / 16.0, 1.0) * 0.5
    if math.isfinite(budget_ratio):
        score += min(budget_ratio, 1.0) * 0.5
    return clamp01(score)


def score_named_panel_bound(
    rows: list[dict[str, Any]], budget_ms: float | None, column_name: str
) -> float:
    panel_p90 = percentile([row.get(column_name, math.nan) for row in rows], 0.90)
    if panel_p90 is None or not math.isfinite(panel_p90):
        return 0.0
    budget_ratio = safe_ratio(panel_p90, budget_ms or math.nan)
    score = min(panel_p90 / 16.0, 1.0) * 0.5
    if math.isfinite(budget_ratio):
        score += min(budget_ratio, 1.0) * 0.5
    return clamp01(score)


def score_swap_bound(rows: list[dict[str, Any]], budget_ms: float | None) -> float:
    swap_p90 = percentile([row.get("swap_ms", math.nan) for row in rows], 0.90)
    if swap_p90 is None or not math.isfinite(swap_p90):
        return 0.0
    budget_ratio = safe_ratio(swap_p90, budget_ms or math.nan)
    score = min(swap_p90 / 8.0, 1.0) * 0.4
    if math.isfinite(budget_ratio):
        score += min(budget_ratio, 1.0) * 0.6
    return clamp01(score)


def score_stimulus_bound(rows: list[dict[str, Any]]) -> float:
    positive_gap = [
        row
        for row in rows
        if isinstance(row.get("stimulus_progress_gap_frames"), (int, float))
        and float(row["stimulus_progress_gap_frames"]) > 0
    ]
    lag_fraction = safe_ratio(float(len(positive_gap)), float(len(rows)))
    gap_p90 = percentile(
        [row.get("stimulus_progress_gap_frames", math.nan) for row in rows], 0.90
    )
    score = 0.0
    if math.isfinite(lag_fraction):
        score += lag_fraction * 0.7
    if gap_p90 is not None and math.isfinite(gap_p90):
        score += min(gap_p90 / 30.0, 1.0) * 0.3
    return clamp01(score)


def build_interpretation(
    ranked_bottlenecks: list[dict[str, float]],
    budget_ms: float | None,
    camera_scene_ui_stats: dict[str, float] | None,
    stimulus_window_ui_stats: dict[str, float] | None,
    stimulus_timeline_ui_stats: dict[str, float] | None,
    movement_timeline_ui_stats: dict[str, float] | None,
    ui_build_stats: dict[str, float] | None,
    draw_stats: dict[str, float] | None,
    upload_stats: dict[str, float] | None,
    swap_stats: dict[str, float] | None,
    camera_gap_stats: dict[str, float] | None,
    stimulus_gap_stats: dict[str, float] | None,
) -> list[str]:
    notes: list[str] = []
    top_names = {item["name"] for item in ranked_bottlenecks[:2]}
    if "movement_timeline_ui_bound" in top_names and movement_timeline_ui_stats:
        movement_ui_p90 = movement_timeline_ui_stats.get("p90", math.nan)
        if math.isfinite(movement_ui_p90):
            notes.append(
                f"Speed & Distance Timeline UI is near the frame budget ({movement_ui_p90:.2f} ms p90), so that panel is a primary playback limiter."
            )
    if "stimulus_timeline_ui_bound" in top_names and stimulus_timeline_ui_stats:
        stimulus_timeline_p90 = stimulus_timeline_ui_stats.get("p90", math.nan)
        if math.isfinite(stimulus_timeline_p90):
            notes.append(
                f"Stimulus Event Timeline UI is near the frame budget ({stimulus_timeline_p90:.2f} ms p90), so that panel is a primary playback limiter."
            )
    if "stimulus_window_ui_bound" in top_names and stimulus_window_ui_stats:
        stimulus_window_p90 = stimulus_window_ui_stats.get("p90", math.nan)
        if math.isfinite(stimulus_window_p90):
            notes.append(
                f"Stimulus video window UI is near the frame budget ({stimulus_window_p90:.2f} ms p90), so that panel is a primary playback limiter."
            )
    if "camera_scene_ui_bound" in top_names and camera_scene_ui_stats:
        scene_ui_p90 = camera_scene_ui_stats.get("p90", math.nan)
        if math.isfinite(scene_ui_p90):
            notes.append(
                f"Camera scene UI construction is near the frame budget ({scene_ui_p90:.2f} ms p90), so ImPlot image/overlay building is a primary limiter."
            )
    if "ui_build_bound" in top_names and ui_build_stats:
        ui_p90 = ui_build_stats.get("p90", math.nan)
        if math.isfinite(ui_p90):
            notes.append(
                f"CPU UI build time is near the frame budget ({ui_p90:.2f} ms p90), so ImGui/ImPlot scene construction is a primary limiter."
            )
    if "gl_draw_bound" in top_names and draw_stats:
        draw_p90 = draw_stats.get("p90", math.nan)
        if math.isfinite(draw_p90):
            notes.append(
                f"GL draw time is near the frame budget ({draw_p90:.2f} ms p90), so rendering cost is a primary limiter."
            )
    if "camera_decode_bound" in top_names and camera_gap_stats:
        gap_p90 = camera_gap_stats.get("p90", math.nan)
        if math.isfinite(gap_p90):
            notes.append(
                f"Camera decode lag is persistent (camera decode gap p90 {gap_p90:.1f} frames), so playback is also decode-limited."
            )
    if upload_stats:
        upload_p90 = upload_stats.get("p90", math.nan)
        if math.isfinite(upload_p90) and upload_p90 < 1.0:
            notes.append(
                f"Camera upload cost is negligible ({upload_p90:.3f} ms p90), so PCIe upload is not the main bottleneck in this capture."
            )
    if swap_stats:
        swap_p90 = swap_stats.get("p90", math.nan)
        if math.isfinite(swap_p90) and budget_ms is not None and math.isfinite(budget_ms):
            if swap_p90 < budget_ms * 0.15:
                notes.append(
                    f"Swap/VSync wait is small ({swap_p90:.3f} ms p90), so the app is missing frame budget before swap."
                )
    if stimulus_gap_stats:
        stim_p90 = stimulus_gap_stats.get("p90", math.nan)
        if math.isfinite(stim_p90) and stim_p90 <= 0.0:
            notes.append(
                "Stimulus stays caught up or ahead in this sample, so the current slowdown is not primarily stimulus-limited."
            )
    return notes


def build_summary(
    rows: list[dict[str, Any]],
    metadata: dict[str, Any] | None,
    top_stalls: int,
) -> dict[str, Any]:
    playing_rows = [
        row
        for row in rows
        if isinstance(row.get("play_video"), (int, float)) and int(row["play_video"]) == 1
    ]
    active_rows = playing_rows or rows

    requested_speed = percentile(
        [row.get("set_playback_speed", math.nan) for row in active_rows], 0.50
    )
    video_fps = percentile([row.get("video_fps", math.nan) for row in active_rows], 0.50)
    target_fps = (
        video_fps * requested_speed
        if video_fps is not None
        and requested_speed is not None
        and math.isfinite(video_fps)
        and math.isfinite(requested_speed)
        else math.nan
    )
    budget_ms = 1000.0 / target_fps if math.isfinite(target_fps) and target_fps > 0.0 else math.nan

    speed_stats = describe([row.get("inst_speed", math.nan) for row in active_rows])
    upload_stats = describe([row.get("camera_upload_ms", math.nan) for row in active_rows])
    camera_texture_resize_stats = describe(
        [row.get("camera_texture_resize_ms", math.nan) for row in active_rows]
    )
    camera_preview_resize_stats = describe(
        [row.get("camera_preview_resize_ms", math.nan) for row in active_rows]
    )
    camera_pbo_copy_stats = describe(
        [row.get("camera_pbo_copy_ms", math.nan) for row in active_rows]
    )
    camera_texture_upload_stats = describe(
        [row.get("camera_texture_upload_ms", math.nan) for row in active_rows]
    )
    camera_plot_image_ui_stats = describe(
        [row.get("camera_plot_image_ui_ms", math.nan) for row in active_rows]
    )
    camera_overlay_ui_stats = describe(
        [row.get("camera_overlay_ui_ms", math.nan) for row in active_rows]
    )
    camera_scene_ui_stats = describe(
        [row.get("camera_scene_ui_ms", math.nan) for row in active_rows]
    )
    stimulus_window_ui_stats = describe(
        [row.get("stimulus_window_ui_ms", math.nan) for row in active_rows]
    )
    stimulus_timeline_ui_stats = describe(
        [row.get("stimulus_timeline_ui_ms", math.nan) for row in active_rows]
    )
    movement_timeline_ui_stats = describe(
        [row.get("movement_timeline_ui_ms", math.nan) for row in active_rows]
    )
    ui_build_stats = describe([row.get("ui_build_ms", math.nan) for row in active_rows])
    imgui_render_stats = describe(
        [row.get("imgui_render_ms", math.nan) for row in active_rows]
    )
    imgui_draw_cmd_count_stats = describe(
        [row.get("imgui_draw_cmd_count", math.nan) for row in active_rows]
    )
    imgui_draw_list_count_stats = describe(
        [row.get("imgui_draw_list_count", math.nan) for row in active_rows]
    )
    imgui_total_vtx_count_stats = describe(
        [row.get("imgui_total_vtx_count", math.nan) for row in active_rows]
    )
    imgui_total_idx_count_stats = describe(
        [row.get("imgui_total_idx_count", math.nan) for row in active_rows]
    )
    draw_stats = describe([row.get("gl_draw_ms", math.nan) for row in active_rows])
    swap_stats = describe([row.get("swap_ms", math.nan) for row in active_rows])
    frame_loop_stats = describe([row.get("frame_loop_ms", math.nan) for row in active_rows])
    camera_gap_stats = describe(
        [row.get("camera_decode_gap_frames", math.nan) for row in active_rows]
    )
    camera_decode_convert_stats = describe(
        [row.get("camera_decode_convert_ms", math.nan) for row in active_rows]
    )
    camera_decode_wait_stats = describe(
        [row.get("camera_decode_wait_ms", math.nan) for row in active_rows]
    )
    camera_decode_write_stats = describe(
        [row.get("camera_decode_write_ms", math.nan) for row in active_rows]
    )
    camera_decode_pipeline_stats = describe(
        [row.get("camera_decode_pipeline_ms", math.nan) for row in active_rows]
    )
    stimulus_gap_stats = describe(
        [row.get("stimulus_progress_gap_frames", math.nan) for row in active_rows]
    )

    bottleneck_scores = {
        "camera_decode_bound": score_decode_bound(active_rows, budget_ms),
        "camera_upload_bound": score_upload_bound(active_rows, budget_ms),
        "camera_scene_ui_bound": score_camera_scene_ui_bound(active_rows, budget_ms),
        "stimulus_window_ui_bound": score_named_panel_bound(
            active_rows, budget_ms, "stimulus_window_ui_ms"
        ),
        "stimulus_timeline_ui_bound": score_named_panel_bound(
            active_rows, budget_ms, "stimulus_timeline_ui_ms"
        ),
        "movement_timeline_ui_bound": score_named_panel_bound(
            active_rows, budget_ms, "movement_timeline_ui_ms"
        ),
        "ui_build_bound": score_ui_build_bound(active_rows, budget_ms),
        "gl_draw_bound": score_draw_bound(active_rows, budget_ms),
        "swap_or_vsync_bound": score_swap_bound(active_rows, budget_ms),
    }
    stimulus_loaded = any(
        isinstance(row.get("stimulus_loaded"), (int, float)) and int(row["stimulus_loaded"]) == 1
        for row in active_rows
    )
    if stimulus_loaded:
        bottleneck_scores["stimulus_sync_bound"] = score_stimulus_bound(active_rows)

    ranked_bottlenecks = [
        {"name": name, "score": score}
        for name, score in sorted(
            bottleneck_scores.items(), key=lambda item: item[1], reverse=True
        )
        if score > 0.05
    ]

    interpretation = build_interpretation(
        ranked_bottlenecks,
        budget_ms if math.isfinite(budget_ms) else None,
        camera_scene_ui_stats,
        stimulus_window_ui_stats,
        stimulus_timeline_ui_stats,
        movement_timeline_ui_stats,
        ui_build_stats,
        draw_stats,
        upload_stats,
        swap_stats,
        camera_gap_stats,
        stimulus_gap_stats,
    )

    worst_stalls = sorted(
        [
            {
                "elapsed_s": row.get("elapsed_s"),
                "frame_loop_ms": row.get("frame_loop_ms"),
                "camera_upload_ms": row.get("camera_upload_ms"),
                "camera_texture_resize_ms": row.get("camera_texture_resize_ms"),
                "camera_preview_resize_ms": row.get("camera_preview_resize_ms"),
                "camera_pbo_copy_ms": row.get("camera_pbo_copy_ms"),
                "camera_texture_upload_ms": row.get("camera_texture_upload_ms"),
                "camera_plot_image_ui_ms": row.get("camera_plot_image_ui_ms"),
                "camera_overlay_ui_ms": row.get("camera_overlay_ui_ms"),
                "camera_scene_ui_ms": row.get("camera_scene_ui_ms"),
                "stimulus_window_ui_ms": row.get("stimulus_window_ui_ms"),
                "stimulus_timeline_ui_ms": row.get("stimulus_timeline_ui_ms"),
                "movement_timeline_ui_ms": row.get("movement_timeline_ui_ms"),
                "ui_build_ms": row.get("ui_build_ms"),
                "imgui_render_ms": row.get("imgui_render_ms"),
                "gl_draw_ms": row.get("gl_draw_ms"),
                "swap_ms": row.get("swap_ms"),
                "imgui_draw_cmd_count": row.get("imgui_draw_cmd_count"),
                "imgui_draw_list_count": row.get("imgui_draw_list_count"),
                "imgui_total_vtx_count": row.get("imgui_total_vtx_count"),
                "imgui_total_idx_count": row.get("imgui_total_idx_count"),
                "camera_decode_gap_frames": row.get("camera_decode_gap_frames"),
                "camera_decode_convert_ms": row.get("camera_decode_convert_ms"),
                "camera_decode_wait_ms": row.get("camera_decode_wait_ms"),
                "camera_decode_write_ms": row.get("camera_decode_write_ms"),
                "camera_decode_pipeline_ms": row.get("camera_decode_pipeline_ms"),
                "stimulus_progress_gap_frames": row.get("stimulus_progress_gap_frames"),
                "displayed_camera_frame": row.get("displayed_camera_frame"),
                "stimulus_target_frame": row.get("stimulus_target_frame"),
            }
            for row in active_rows
            if isinstance(row.get("frame_loop_ms"), (int, float))
            and math.isfinite(float(row["frame_loop_ms"]))
        ],
        key=lambda row: float(row["frame_loop_ms"]),
        reverse=True,
    )[:top_stalls]

    summary = {
        "sample_count": len(rows),
        "playing_sample_count": len(playing_rows),
        "duration_s": (
            float(rows[-1]["elapsed_s"]) - float(rows[0]["elapsed_s"])
            if rows
            and isinstance(rows[0].get("elapsed_s"), (int, float))
            and isinstance(rows[-1].get("elapsed_s"), (int, float))
            else math.nan
        ),
        "requested_playback_speed": requested_speed,
        "video_fps": video_fps,
        "target_fps": target_fps,
        "frame_budget_ms": budget_ms,
        "speed": speed_stats,
        "camera_upload_ms": upload_stats,
        "camera_texture_resize_ms": camera_texture_resize_stats,
        "camera_preview_resize_ms": camera_preview_resize_stats,
        "camera_pbo_copy_ms": camera_pbo_copy_stats,
        "camera_texture_upload_ms": camera_texture_upload_stats,
        "camera_plot_image_ui_ms": camera_plot_image_ui_stats,
        "camera_overlay_ui_ms": camera_overlay_ui_stats,
        "camera_scene_ui_ms": camera_scene_ui_stats,
        "stimulus_window_ui_ms": stimulus_window_ui_stats,
        "stimulus_timeline_ui_ms": stimulus_timeline_ui_stats,
        "movement_timeline_ui_ms": movement_timeline_ui_stats,
        "ui_build_ms": ui_build_stats,
        "imgui_render_ms": imgui_render_stats,
        "imgui_draw_cmd_count": imgui_draw_cmd_count_stats,
        "imgui_draw_list_count": imgui_draw_list_count_stats,
        "imgui_total_vtx_count": imgui_total_vtx_count_stats,
        "imgui_total_idx_count": imgui_total_idx_count_stats,
        "gl_draw_ms": draw_stats,
        "swap_ms": swap_stats,
        "frame_loop_ms": frame_loop_stats,
        "camera_decode_gap_frames": camera_gap_stats,
        "camera_decode_convert_ms": camera_decode_convert_stats,
        "camera_decode_wait_ms": camera_decode_wait_stats,
        "camera_decode_write_ms": camera_decode_write_stats,
        "camera_decode_pipeline_ms": camera_decode_pipeline_stats,
        "stimulus_progress_gap_frames": stimulus_gap_stats,
        "ranked_bottlenecks": ranked_bottlenecks,
        "interpretation": interpretation,
        "worst_stalls": worst_stalls,
        "metadata": metadata or {},
    }
    return summary


def print_summary(summary: dict[str, Any]) -> None:
    print("Playback Profile Analysis")
    print(f"  samples: {summary['sample_count']}")
    print(f"  playing_samples: {summary['playing_sample_count']}")
    print(f"  duration_s: {format_stat(summary.get('duration_s'))}")
    print(f"  requested_speed: {format_stat(summary.get('requested_playback_speed'))}")
    print(f"  video_fps: {format_stat(summary.get('video_fps'))}")
    print(f"  target_fps: {format_stat(summary.get('target_fps'))}")
    print(f"  frame_budget_ms: {format_stat(summary.get('frame_budget_ms'), 'ms')}")

    for key in [
        "speed",
        "camera_upload_ms",
        "camera_texture_resize_ms",
        "camera_preview_resize_ms",
        "camera_pbo_copy_ms",
        "camera_texture_upload_ms",
        "camera_plot_image_ui_ms",
        "camera_overlay_ui_ms",
        "camera_scene_ui_ms",
        "stimulus_window_ui_ms",
        "stimulus_timeline_ui_ms",
        "movement_timeline_ui_ms",
        "ui_build_ms",
        "imgui_render_ms",
        "imgui_draw_cmd_count",
        "imgui_draw_list_count",
        "imgui_total_vtx_count",
        "imgui_total_idx_count",
        "gl_draw_ms",
        "swap_ms",
        "frame_loop_ms",
        "camera_decode_gap_frames",
        "camera_decode_convert_ms",
        "camera_decode_wait_ms",
        "camera_decode_write_ms",
        "camera_decode_pipeline_ms",
        "stimulus_progress_gap_frames",
    ]:
        stats = summary.get(key)
        if not stats:
            continue
        print(
            f"  {key}: "
            f"mean={format_stat(stats.get('mean'))}, "
            f"median={format_stat(stats.get('median'))}, "
            f"p90={format_stat(stats.get('p90'))}, "
            f"max={format_stat(stats.get('max'))}"
        )

    ranked = summary.get("ranked_bottlenecks", [])
    print("Likely Bottlenecks")
    if not ranked:
        print("  none: no strong signal from the current heuristics")
    else:
        for item in ranked[:4]:
            print(f"  {item['name']}: score={item['score']:.3f}")

    interpretation = summary.get("interpretation", [])
    if interpretation:
        print("Interpretation")
        for note in interpretation:
            print(f"  {note}")

    stalls = summary.get("worst_stalls", [])
    if stalls:
        print("Worst Frame-Loop Stalls")
        for stall in stalls:
            print(
                "  "
                f"t={format_stat(stall.get('elapsed_s'), 's')}, "
                f"loop={format_stat(stall.get('frame_loop_ms'), 'ms')}, "
                f"upload={format_stat(stall.get('camera_upload_ms'), 'ms')}, "
                f"tex_resize={format_stat(stall.get('camera_texture_resize_ms'), 'ms')}, "
                f"preview_resize={format_stat(stall.get('camera_preview_resize_ms'), 'ms')}, "
                f"pbo_copy={format_stat(stall.get('camera_pbo_copy_ms'), 'ms')}, "
                f"tex_upload={format_stat(stall.get('camera_texture_upload_ms'), 'ms')}, "
                f"plot_image={format_stat(stall.get('camera_plot_image_ui_ms'), 'ms')}, "
                f"overlay_ui={format_stat(stall.get('camera_overlay_ui_ms'), 'ms')}, "
                f"scene_ui={format_stat(stall.get('camera_scene_ui_ms'), 'ms')}, "
                f"stim_window={format_stat(stall.get('stimulus_window_ui_ms'), 'ms')}, "
                f"stim_timeline={format_stat(stall.get('stimulus_timeline_ui_ms'), 'ms')}, "
                f"movement_ui={format_stat(stall.get('movement_timeline_ui_ms'), 'ms')}, "
                f"ui={format_stat(stall.get('ui_build_ms'), 'ms')}, "
                f"imgui_render={format_stat(stall.get('imgui_render_ms'), 'ms')}, "
                f"draw={format_stat(stall.get('gl_draw_ms'), 'ms')}, "
                f"swap={format_stat(stall.get('swap_ms'), 'ms')}, "
                f"draw_cmds={format_stat(stall.get('imgui_draw_cmd_count'))}, "
                f"draw_lists={format_stat(stall.get('imgui_draw_list_count'))}, "
                f"vtx={format_stat(stall.get('imgui_total_vtx_count'))}, "
                f"idx={format_stat(stall.get('imgui_total_idx_count'))}, "
                f"cam_gap={format_stat(stall.get('camera_decode_gap_frames'))}, "
                f"decode_convert={format_stat(stall.get('camera_decode_convert_ms'), 'ms')}, "
                f"decode_wait={format_stat(stall.get('camera_decode_wait_ms'), 'ms')}, "
                f"decode_write={format_stat(stall.get('camera_decode_write_ms'), 'ms')}, "
                f"decode_total={format_stat(stall.get('camera_decode_pipeline_ms'), 'ms')}, "
                f"stim_gap={format_stat(stall.get('stimulus_progress_gap_frames'))}"
            )


def sanitize_json(value: Any) -> Any:
    if isinstance(value, dict):
        return {key: sanitize_json(val) for key, val in value.items()}
    if isinstance(value, list):
        return [sanitize_json(val) for val in value]
    if isinstance(value, float) and not math.isfinite(value):
        return None
    return value


def main() -> int:
    args = parse_args()
    csv_path = args.csv_path.expanduser().resolve()
    if not csv_path.exists():
        raise SystemExit(f"CSV file not found: {csv_path}")

    meta_path = (
        args.meta.expanduser().resolve()
        if args.meta is not None
        else csv_path.with_suffix(".meta.json")
    )

    rows = load_rows(csv_path)
    metadata = load_metadata(meta_path)
    summary = build_summary(rows, metadata, args.top_stalls)
    print_summary(summary)

    if args.json_output is not None:
        output_path = args.json_output.expanduser().resolve()
        output_path.write_text(json.dumps(sanitize_json(summary), indent=2) + "\n")
        print(f"Wrote JSON summary: {output_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
