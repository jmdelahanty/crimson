#!/usr/bin/env python3
"""
Plot Crimson playback profiler CSV output.

Usage:
    python tools/plot_playback_profile.py /path/to/crimson-playback-profile.csv

This script expects the CSV emitted by `redgui.exe --perf-log ...` and will
automatically read the adjacent `.meta.json` file when present.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
from pathlib import Path
from statistics import mean, median
import tempfile
from typing import Any

if "MPLCONFIGDIR" not in os.environ:
    default_mpl_config = Path.home() / ".config" / "matplotlib"
    if not os.access(default_mpl_config.parent, os.W_OK):
        os.environ["MPLCONFIGDIR"] = tempfile.mkdtemp(prefix="matplotlib-")

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


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
    "camera_display_convert_ms": float,
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
        description="Plot Crimson playback profiler CSV output."
    )
    parser.add_argument("csv_path", type=Path, help="Path to crimson perf CSV")
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="PNG output path. Defaults to <csv>.png",
    )
    parser.add_argument(
        "--meta",
        type=Path,
        default=None,
        help="Optional metadata sidecar. Defaults to adjacent .meta.json",
    )
    parser.add_argument(
        "--title",
        default=None,
        help="Optional figure title override",
    )
    parser.add_argument(
        "--dpi",
        type=int,
        default=150,
        help="Output DPI",
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


def load_rows(csv_path: Path) -> dict[str, list[Any]]:
    with csv_path.open("r", newline="") as handle:
        reader = csv.DictReader(handle)
        fieldnames = reader.fieldnames or []
        columns: dict[str, list[Any]] = {name: [] for name in fieldnames}
        for row in reader:
            for name in fieldnames:
                raw = row.get(name, "")
                if name in NUMERIC_COLUMNS:
                    columns[name].append(parse_numeric(raw, NUMERIC_COLUMNS[name]))
                else:
                    columns[name].append(raw)
    return columns


def load_metadata(meta_path: Path | None) -> dict[str, Any] | None:
    if meta_path is None or not meta_path.exists():
        return None
    with meta_path.open("r") as handle:
        return json.load(handle)


def finite_values(values: list[float]) -> list[float]:
    return [v for v in values if isinstance(v, (int, float)) and math.isfinite(v)]


def finite_pairs(x_values: list[float], y_values: list[float]) -> tuple[list[float], list[float]]:
    xs: list[float] = []
    ys: list[float] = []
    for x_val, y_val in zip(x_values, y_values):
        if isinstance(x_val, (int, float)) and isinstance(y_val, (int, float)):
            if math.isfinite(x_val) and math.isfinite(y_val):
                xs.append(float(x_val))
                ys.append(float(y_val))
    return xs, ys


def describe(values: list[float]) -> str:
    vals = finite_values(values)
    if not vals:
        return "n/a"
    return f"mean={mean(vals):.3f}, median={median(vals):.3f}, max={max(vals):.3f}"


def print_summary(columns: dict[str, list[Any]], metadata: dict[str, Any] | None) -> None:
    elapsed = finite_values(columns.get("elapsed_s", []))
    play_mask = columns.get("play_video", [])
    inst_speed = columns.get("inst_speed", [])
    playing_speeds = [
        float(speed)
        for speed, playing in zip(inst_speed, play_mask)
        if isinstance(speed, (int, float))
        and isinstance(playing, (int, float))
        and math.isfinite(speed)
        and int(playing) == 1
    ]
    print("Summary")
    print(f"  samples: {len(columns.get('elapsed_s', []))}")
    if elapsed:
        print(f"  duration_s: {elapsed[-1] - elapsed[0]:.3f}")
    print(f"  playing_speed: {describe(playing_speeds)}")
    print(
        f"  camera_upload_ms: {describe(columns.get('camera_upload_ms', []))}"
    )
    print(
        f"  camera_texture_resize_ms: {describe(columns.get('camera_texture_resize_ms', []))}"
    )
    print(
        f"  camera_preview_resize_ms: {describe(columns.get('camera_preview_resize_ms', []))}"
    )
    print(
        f"  camera_display_convert_ms: {describe(columns.get('camera_display_convert_ms', []))}"
    )
    print(
        f"  camera_pbo_copy_ms: {describe(columns.get('camera_pbo_copy_ms', []))}"
    )
    print(
        f"  camera_texture_upload_ms: {describe(columns.get('camera_texture_upload_ms', []))}"
    )
    print(
        f"  camera_plot_image_ui_ms: {describe(columns.get('camera_plot_image_ui_ms', []))}"
    )
    print(
        f"  camera_overlay_ui_ms: {describe(columns.get('camera_overlay_ui_ms', []))}"
    )
    print(
        f"  camera_scene_ui_ms: {describe(columns.get('camera_scene_ui_ms', []))}"
    )
    print(
        f"  stimulus_window_ui_ms: {describe(columns.get('stimulus_window_ui_ms', []))}"
    )
    print(
        f"  stimulus_timeline_ui_ms: {describe(columns.get('stimulus_timeline_ui_ms', []))}"
    )
    print(
        f"  movement_timeline_ui_ms: {describe(columns.get('movement_timeline_ui_ms', []))}"
    )
    print(f"  ui_build_ms: {describe(columns.get('ui_build_ms', []))}")
    print(f"  imgui_render_ms: {describe(columns.get('imgui_render_ms', []))}")
    print(
        f"  imgui_draw_cmd_count: {describe(columns.get('imgui_draw_cmd_count', []))}"
    )
    print(
        f"  imgui_draw_list_count: {describe(columns.get('imgui_draw_list_count', []))}"
    )
    print(
        f"  imgui_total_vtx_count: {describe(columns.get('imgui_total_vtx_count', []))}"
    )
    print(
        f"  imgui_total_idx_count: {describe(columns.get('imgui_total_idx_count', []))}"
    )
    print(f"  gl_draw_ms: {describe(columns.get('gl_draw_ms', []))}")
    print(f"  swap_ms: {describe(columns.get('swap_ms', []))}")
    print(f"  frame_loop_ms: {describe(columns.get('frame_loop_ms', []))}")
    print(
        "  camera_decode_gap_frames: "
        f"{describe(columns.get('camera_decode_gap_frames', []))}"
    )
    print(
        "  camera_decode_convert_ms: "
        f"{describe(columns.get('camera_decode_convert_ms', []))}"
    )
    print(
        "  camera_decode_wait_ms: "
        f"{describe(columns.get('camera_decode_wait_ms', []))}"
    )
    print(
        "  camera_decode_write_ms: "
        f"{describe(columns.get('camera_decode_write_ms', []))}"
    )
    print(
        "  camera_decode_pipeline_ms: "
        f"{describe(columns.get('camera_decode_pipeline_ms', []))}"
    )
    print(
        "  stimulus_progress_gap_frames: "
        f"{describe(columns.get('stimulus_progress_gap_frames', []))}"
    )
    if metadata:
        main_video = metadata.get("main_video", {})
        stimulus = metadata.get("stimulus", {})
        print("Metadata")
        print(
            "  main_video: "
            f"buffer={main_video.get('buffer_mode')}, "
            f"buffer_size={main_video.get('buffer_size')}, "
            f"requested_speed={main_video.get('requested_playback_speed')}"
        )
        print(
            "  stimulus: "
            f"backend={stimulus.get('decode_backend')}, "
            f"buffer={stimulus.get('buffer_mode')}, "
            f"buffer_size={stimulus.get('buffer_size')}"
        )


def maybe_plot_line(
    axis: plt.Axes,
    x_values: list[float],
    y_values: list[float],
    label: str,
    **kwargs: Any,
) -> None:
    xs, ys = finite_pairs(x_values, y_values)
    if xs and ys:
        axis.plot(xs, ys, label=label, **kwargs)


def build_title(
    csv_path: Path,
    metadata: dict[str, Any] | None,
    explicit_title: str | None,
) -> str:
    if explicit_title:
        return explicit_title
    if metadata:
        main_video = metadata.get("main_video", {})
        stimulus = metadata.get("stimulus", {})
        return (
            f"Crimson Playback Profile\n"
            f"main={main_video.get('buffer_mode', 'unknown')} "
            f"({main_video.get('buffer_size', 'n/a')}), "
            f"stim={stimulus.get('decode_backend', 'unknown')}/"
            f"{stimulus.get('buffer_mode', 'unknown')} "
            f"({stimulus.get('buffer_size', 'n/a')})"
        )
    return f"Crimson Playback Profile\n{csv_path.name}"


def plot_profile(
    columns: dict[str, list[Any]],
    metadata: dict[str, Any] | None,
    output_path: Path,
    title: str,
    dpi: int,
) -> None:
    elapsed = [float(v) for v in columns.get("elapsed_s", [])]

    figure, axes = plt.subplots(4, 1, figsize=(16, 13), sharex=True)

    maybe_plot_line(
        axes[0], elapsed, columns.get("inst_speed", []), "measured speed", color="tab:blue"
    )
    maybe_plot_line(
        axes[0],
        elapsed,
        columns.get("set_playback_speed", []),
        "requested speed",
        color="tab:orange",
        linestyle="--",
    )
    axes[0].set_ylabel("Playback speed")
    axes[0].grid(True, alpha=0.3)
    axes[0].legend(loc="upper right")

    maybe_plot_line(
        axes[1],
        elapsed,
        columns.get("requested_camera_frame", []),
        "requested camera frame",
        color="tab:blue",
    )
    maybe_plot_line(
        axes[1],
        elapsed,
        columns.get("displayed_camera_frame", []),
        "displayed camera frame",
        color="tab:green",
    )
    maybe_plot_line(
        axes[1],
        elapsed,
        columns.get("min_decoded_camera_frame", []),
        "min decoded camera frame",
        color="tab:red",
    )
    axes[1].set_ylabel("Camera frame")
    axes[1].grid(True, alpha=0.3)
    axes[1].legend(loc="upper left")
    gap_axis = axes[1].twinx()
    maybe_plot_line(
        gap_axis,
        elapsed,
        columns.get("camera_decode_gap_frames", []),
        "camera decode gap",
        color="tab:purple",
        alpha=0.45,
    )
    gap_axis.set_ylabel("Gap frames")

    maybe_plot_line(
        axes[2],
        elapsed,
        columns.get("camera_upload_ms", []),
        "camera upload ms",
        color="tab:green",
    )
    maybe_plot_line(
        axes[2],
        elapsed,
        columns.get("camera_texture_resize_ms", []),
        "camera texture resize ms",
        color="tab:gray",
    )
    maybe_plot_line(
        axes[2],
        elapsed,
        columns.get("camera_preview_resize_ms", []),
        "camera preview resize ms",
        color="tab:blue",
    )
    maybe_plot_line(
        axes[2],
        elapsed,
        columns.get("camera_display_convert_ms", []),
        "camera display convert ms",
        color="tab:cyan",
        alpha=0.8,
    )
    maybe_plot_line(
        axes[2],
        elapsed,
        columns.get("camera_pbo_copy_ms", []),
        "camera pbo copy ms",
        color="tab:olive",
        alpha=0.65,
    )
    maybe_plot_line(
        axes[2],
        elapsed,
        columns.get("camera_texture_upload_ms", []),
        "camera texture upload ms",
        color="tab:pink",
        alpha=0.65,
    )
    maybe_plot_line(
        axes[2],
        elapsed,
        columns.get("camera_plot_image_ui_ms", []),
        "camera plot image UI ms",
        color="tab:orange",
        alpha=0.8,
    )
    maybe_plot_line(
        axes[2],
        elapsed,
        columns.get("camera_overlay_ui_ms", []),
        "camera overlay UI ms",
        color="tab:red",
        alpha=0.8,
    )
    maybe_plot_line(
        axes[2],
        elapsed,
        columns.get("camera_scene_ui_ms", []),
        "camera scene UI ms",
        color="tab:brown",
    )
    maybe_plot_line(
        axes[2],
        elapsed,
        columns.get("stimulus_window_ui_ms", []),
        "stimulus window UI ms",
        color="tab:pink",
    )
    maybe_plot_line(
        axes[2],
        elapsed,
        columns.get("stimulus_timeline_ui_ms", []),
        "stimulus timeline UI ms",
        color="tab:olive",
    )
    maybe_plot_line(
        axes[2],
        elapsed,
        columns.get("movement_timeline_ui_ms", []),
        "movement timeline UI ms",
        color="tab:cyan",
    )
    maybe_plot_line(
        axes[2], elapsed, columns.get("ui_build_ms", []), "UI build ms", color="tab:purple"
    )
    maybe_plot_line(
        axes[2],
        elapsed,
        columns.get("imgui_render_ms", []),
        "ImGui render ms",
        color="tab:gray",
        linestyle="--",
    )
    maybe_plot_line(
        axes[2], elapsed, columns.get("gl_draw_ms", []), "GL draw ms", color="tab:orange"
    )
    maybe_plot_line(
        axes[2], elapsed, columns.get("swap_ms", []), "swap ms", color="tab:red"
    )
    maybe_plot_line(
        axes[2],
        elapsed,
        columns.get("frame_loop_ms", []),
        "frame loop ms",
        color="tab:blue",
    )
    maybe_plot_line(
        axes[2],
        elapsed,
        columns.get("camera_decode_convert_ms", []),
        "decode convert ms",
        color="tab:green",
        linestyle="--",
        alpha=0.7,
    )
    maybe_plot_line(
        axes[2],
        elapsed,
        columns.get("camera_decode_wait_ms", []),
        "decode wait ms",
        color="tab:brown",
        linestyle="--",
        alpha=0.7,
    )
    maybe_plot_line(
        axes[2],
        elapsed,
        columns.get("camera_decode_write_ms", []),
        "decode write ms",
        color="tab:cyan",
        linestyle="--",
        alpha=0.7,
    )
    maybe_plot_line(
        axes[2],
        elapsed,
        columns.get("camera_decode_pipeline_ms", []),
        "decode total ms",
        color="tab:gray",
        linestyle=":",
        alpha=0.7,
    )
    axes[2].set_ylabel("Milliseconds")
    axes[2].grid(True, alpha=0.3)
    axes[2].legend(loc="upper right")

    maybe_plot_line(
        axes[3],
        elapsed,
        columns.get("stimulus_target_frame", []),
        "stimulus target",
        color="tab:blue",
    )
    maybe_plot_line(
        axes[3],
        elapsed,
        columns.get("stimulus_latest_decoded", []),
        "stimulus latest decoded",
        color="tab:orange",
    )
    maybe_plot_line(
        axes[3],
        elapsed,
        columns.get("stimulus_last_displayed", []),
        "stimulus last displayed",
        color="tab:green",
    )
    axes[3].set_ylabel("Stimulus frame")
    axes[3].grid(True, alpha=0.3)
    axes[3].legend(loc="upper left")
    stim_gap_axis = axes[3].twinx()
    maybe_plot_line(
        stim_gap_axis,
        elapsed,
        columns.get("stimulus_progress_gap_frames", []),
        "stimulus progress gap",
        color="tab:red",
        alpha=0.45,
    )
    maybe_plot_line(
        stim_gap_axis,
        elapsed,
        columns.get("stimulus_buffered_frames", []),
        "stimulus buffered frames",
        color="tab:purple",
        alpha=0.45,
    )
    stim_gap_axis.set_ylabel("Gap / buffered")

    axes[3].set_xlabel("Elapsed seconds")
    figure.suptitle(title)

    if metadata:
        subtitle = (
            f"csv={metadata.get('perf_csv_path', output_path.name)}\n"
            f"main={metadata.get('main_video', {}).get('buffer_mode', 'unknown')} "
            f"({metadata.get('main_video', {}).get('buffer_size', 'n/a')}), "
            f"stim={metadata.get('stimulus', {}).get('decode_backend', 'unknown')}/"
            f"{metadata.get('stimulus', {}).get('buffer_mode', 'unknown')} "
            f"({metadata.get('stimulus', {}).get('buffer_size', 'n/a')}), "
            f"swap_interval={metadata.get('window', {}).get('swap_interval', 'n/a')}"
        )
        figure.text(0.01, 0.01, subtitle, fontsize=9, va="bottom", ha="left")

    figure.tight_layout(rect=(0.0, 0.04, 1.0, 0.96))
    figure.savefig(output_path, dpi=dpi)
    plt.close(figure)


def main() -> int:
    args = parse_args()
    csv_path = args.csv_path.expanduser().resolve()
    if not csv_path.exists():
        raise SystemExit(f"CSV file not found: {csv_path}")

    output_path = (
        args.output.expanduser().resolve()
        if args.output is not None
        else csv_path.with_suffix(".png")
    )
    meta_path = (
        args.meta.expanduser().resolve()
        if args.meta is not None
        else csv_path.with_suffix(".meta.json")
    )

    columns = load_rows(csv_path)
    metadata = load_metadata(meta_path)
    print_summary(columns, metadata)
    title = build_title(csv_path, metadata, args.title)
    plot_profile(columns, metadata, output_path, title, args.dpi)
    print(f"Wrote plot: {output_path}")
    if metadata and meta_path.exists():
        print(f"Read metadata: {meta_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
