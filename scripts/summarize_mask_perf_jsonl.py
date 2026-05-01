#!/usr/bin/env python3
"""Summarize Crimson mask/frame performance JSONL logs."""

from __future__ import annotations

import argparse
import json
import math
from collections import Counter
from pathlib import Path
from typing import Any, Iterable


DEFAULT_LOG = Path.home() / ".cache/crimson/buffer_dumps/mask_perf_latest.jsonl"


def get_nested(row: dict[str, Any], path: str) -> float | int | None:
    value = get_path(row, path)
    if isinstance(value, (int, float)) and math.isfinite(value):
        return value
    return None


def get_path(row: dict[str, Any], path: str) -> Any:
    value: Any = row
    for part in path.split("."):
        if not isinstance(value, dict) or part not in value:
            return None
        value = value[part]
    return value


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return float("nan")
    values = sorted(values)
    if len(values) == 1:
        return values[0]
    pos = (len(values) - 1) * pct / 100.0
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return values[lo]
    return values[lo] * (hi - pos) + values[hi] * (pos - lo)


def summarize(values: Iterable[float | int | None]) -> str:
    finite = [float(v) for v in values if isinstance(v, (int, float)) and math.isfinite(v)]
    if not finite:
        return "n=0"
    return (
        f"n={len(finite)} mean={sum(finite) / len(finite):.4f} "
        f"p50={percentile(finite, 50):.4f} "
        f"p90={percentile(finite, 90):.4f} "
        f"p95={percentile(finite, 95):.4f} "
        f"p99={percentile(finite, 99):.4f} "
        f"max={max(finite):.4f}"
    )


def contiguous_runs(rows: list[dict[str, Any]], gap_ms: int = 100) -> list[list[dict[str, Any]]]:
    runs: list[list[dict[str, Any]]] = []
    current: list[dict[str, Any]] = []
    for row in rows:
        if not current:
            current = [row]
            continue
        previous = current[-1]
        wall = row.get("wall_epoch_ms")
        previous_wall = previous.get("wall_epoch_ms")
        gap = (
            wall - previous_wall
            if isinstance(wall, int) and isinstance(previous_wall, int)
            else 0
        )
        if row.get("play_video") != previous.get("play_video") or gap > gap_ms:
            runs.append(current)
            current = [row]
        else:
            current.append(row)
    if current:
        runs.append(current)
    return runs


def mean_finite(values: Iterable[float | int | None]) -> float | None:
    finite = [float(v) for v in values if isinstance(v, (int, float)) and math.isfinite(v)]
    if not finite:
        return None
    return sum(finite) / len(finite)


def print_observed_rates(rows: list[dict[str, Any]]) -> None:
    runs = [run for run in contiguous_runs(rows) if len(run) >= 10]
    if not runs:
        return
    print("\nobserved contiguous rates")
    for idx, run in enumerate(runs):
        first_wall = run[0].get("wall_epoch_ms")
        last_wall = run[-1].get("wall_epoch_ms")
        if not isinstance(first_wall, int) or not isinstance(last_wall, int):
            continue
        wall_s = (last_wall - first_wall) / 1000.0
        if wall_s <= 0.0:
            continue
        sample_every = run[0].get("mask_perf_sample_every", 1)
        if not isinstance(sample_every, int) or sample_every <= 0:
            sample_every = 1
        sample_hz = (len(run) - 1) / wall_s
        approx_app_hz = sample_hz * sample_every
        mean_loop = mean_finite(get_nested(row, "frame_loop_ms") for row in run)
        loop_hz = 1000.0 / mean_loop if mean_loop and mean_loop > 0.0 else None
        current_frame_hz = None
        first_frame = run[0].get("current_frame_num")
        last_frame = run[-1].get("current_frame_num")
        if isinstance(first_frame, int) and isinstance(last_frame, int):
            current_frame_hz = (last_frame - first_frame) / wall_s
        print(
            "run={idx} play={play} samples={samples} wall_s={wall_s:.3f} "
            "sample_hz={sample_hz:.2f} approx_app_hz={app_hz:.2f} "
            "loop_hz={loop_hz} current_frame_hz={current_hz}".format(
                idx=idx,
                play=run[0].get("play_video"),
                samples=len(run),
                wall_s=wall_s,
                sample_hz=sample_hz,
                app_hz=approx_app_hz,
                loop_hz=f"{loop_hz:.2f}" if loop_hz is not None else "n/a",
                current_hz=(
                    f"{current_frame_hz:.2f}"
                    if current_frame_hz is not None
                    else "n/a"
                ),
            )
        )


def print_timing_group(title: str, rows: list[dict[str, Any]], fields: list[tuple[str, str]]) -> None:
    print(f"\n{title}")
    for label, path in fields:
        print(f"{label}: {summarize(get_nested(row, path) for row in rows)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "path",
        nargs="?",
        default=str(DEFAULT_LOG),
        help=f"JSONL log path (default: {DEFAULT_LOG})",
    )
    parser.add_argument("--top", type=int, default=10, help="slow-frame rows to show")
    args = parser.parse_args()

    path = Path(args.path).expanduser()
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as stream:
        for line_num, line in enumerate(stream, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise SystemExit(f"{path}:{line_num}: failed to parse JSON: {exc}") from exc

    print(f"path: {path}")
    print(f"samples: {len(rows)}")
    if not rows:
        return 0

    print(f"formats: {dict(Counter(row.get('format') for row in rows))}")
    print(
        "mask_perf_sample_every:",
        dict(Counter(row.get("mask_perf_sample_every", 1) for row in rows)),
    )
    print(f"play_video: {dict(Counter(row.get('play_video') for row in rows))}")
    print(f"overlay_enabled: {dict(Counter(row.get('overlay_enabled') for row in rows))}")
    print(f"modes: {dict(Counter(get_path(row, 'metrics.mode') for row in rows))}")
    print(
        "frames: "
        f"{rows[0].get('current_frame_num')} -> {rows[-1].get('current_frame_num')} "
        f"({len({row.get('current_frame_num') for row in rows})} unique)"
    )
    print(f"zarr: {rows[-1].get('loaded_zarr_path')}")
    print(f"source: {rows[-1].get('source_label')} / {rows[-1].get('run_name')}")
    frame_cap_fps = get_nested(rows[-1], "frame_perf.render.frame_cap_fps")
    if frame_cap_fps and frame_cap_fps > 0.0:
        print(
            "frame_cap_fps: "
            f"{frame_cap_fps:.3f} ({1000.0 / frame_cap_fps:.4f} ms budget)"
        )
    print_observed_rates(rows)

    mask_fields = [
        ("frame_loop_ms", "frame_loop_ms"),
        ("mask_data_load_ms", "mask_data_load_ms"),
        ("mask_total_draw_ms", "metrics.total_draw_ms"),
        ("texture_lookup_ms", "metrics.texture_lookup_ms"),
        ("texture_upload_ms", "metrics.texture_upload_ms"),
        ("fill_draw_ms", "metrics.fill_draw_ms"),
        ("contour_build_ms", "metrics.contour_build_ms"),
        ("contour_draw_ms", "metrics.contour_draw_ms"),
        ("axis_draw_ms", "metrics.axis_draw_ms"),
        ("pick_ms", "metrics.pick_ms"),
    ]
    print_timing_group("mask timings", rows, mask_fields)

    frame_fields = [
        ("camera_upload_ms", "frame_perf.camera_pipeline.upload_ms"),
        ("camera_texture_upload_ms", "frame_perf.camera_pipeline.texture_upload_ms"),
        ("camera_plot_image_ui_ms", "frame_perf.camera_pipeline.plot_image_ui_ms"),
        ("camera_overlay_ui_ms", "frame_perf.camera_pipeline.overlay_ui_ms"),
        (
            "subject_shape_overlay_ms",
            "frame_perf.camera_pipeline.subject_shape_overlay_ms",
        ),
        (
            "tail_kinematics_overlay_ms",
            "frame_perf.camera_pipeline.tail_kinematics_overlay_ms",
        ),
        ("camera_scene_ui_ms", "frame_perf.camera_pipeline.scene_ui_ms"),
        ("camera_playback_stage_total_ms", "frame_perf.camera_pipeline.playback_stage_total_ms"),
        ("camera_playback_swap_ms", "frame_perf.camera_pipeline.playback_swap_ms"),
        ("ui_build_ms", "frame_perf.ui.build_ms"),
        ("imgui_render_ms", "frame_perf.ui.imgui_render_ms"),
        ("frame_debug_ui_ms", "frame_perf.ui.frame_debug_ms"),
        ("gl_draw_ms", "frame_perf.render.gl_draw_ms"),
        ("swap_ms", "frame_perf.render.swap_ms"),
        ("frame_cap_sleep_ms", "frame_perf.render.frame_cap_sleep_ms"),
        ("decoder_convert_ms", "frame_perf.decoder.convert_ms"),
        ("decoder_wait_ms", "frame_perf.decoder.buffer_wait_ms"),
        ("decoder_pipeline_ms", "frame_perf.decoder.pipeline_ms"),
    ]
    crop_fields = [
        ("crop_total_window_ms", "frame_perf.ui.crop_preview_perf.total_window_ms"),
        ("crop_refresh_ms", "frame_perf.ui.crop_preview_perf.refresh_ms"),
        ("crop_window_setup_ms", "frame_perf.ui.crop_preview_perf.window_setup_ms"),
        ("crop_imgui_begin_ms", "frame_perf.ui.crop_preview_perf.imgui_begin_ms"),
        (
            "crop_resolve_selection_ms",
            "frame_perf.ui.crop_preview_perf.resolve_selection_ms",
        ),
        ("crop_context_build_ms", "frame_perf.ui.crop_preview_perf.context_build_ms"),
        ("crop_editor_panel_ms", "frame_perf.ui.crop_preview_perf.editor_panel_ms"),
        ("crop_post_panel_ms", "frame_perf.ui.crop_preview_perf.post_panel_ms"),
        ("crop_imgui_end_ms", "frame_perf.ui.crop_preview_perf.imgui_end_ms"),
        ("crop_finish_ms", "frame_perf.ui.crop_preview_perf.finish_ms"),
        (
            "crop_provider_texture_lookup_ms",
            "frame_perf.ui.crop_preview_perf.provider_texture_lookup_ms",
        ),
        (
            "crop_provider_image_lookup_ms",
            "frame_perf.ui.crop_preview_perf.provider_image_lookup_ms",
        ),
        (
            "crop_render_texture_ms",
            "frame_perf.ui.crop_preview_perf.render_crop_texture_ms",
        ),
        ("crop_image_upload_ms", "frame_perf.ui.crop_preview_perf.image_upload_ms"),
        (
            "crop_get_raw_detections_ms",
            "frame_perf.ui.crop_preview_perf.get_raw_detections_ms",
        ),
        (
            "crop_render_rotated_texture_ms",
            "frame_perf.ui.crop_preview_perf.render_rotated_texture_ms",
        ),
        (
            "crop_keypoint_transform_ms",
            "frame_perf.ui.crop_preview_perf.keypoint_transform_ms",
        ),
    ]
    if any(get_nested(row, "frame_perf.frame_loop_ms") is not None for row in rows):
        print_timing_group("frame timings", rows, frame_fields)
        print_timing_group("crop preview timings", rows, crop_fields)
        print(
            "crop sources:",
            dict(
                Counter(
                    get_path(row, "frame_perf.ui.crop_preview_perf.crop_source")
                    for row in rows
                )
            ),
        )
        print(
            "crop rotated_requested:",
            dict(
                Counter(
                    get_path(row, "frame_perf.ui.crop_preview_perf.rotated_requested")
                    for row in rows
                )
            ),
        )
    else:
        print("\nframe timings: unavailable in this log; rerun with crimson_mask_perf_v3")

    for play_value in (True, False):
        subset = [row for row in rows if row.get("play_video") is play_value]
        if not subset:
            continue
        print_timing_group(f"mask timings play_video={play_value}", subset, mask_fields)
        if any(get_nested(row, "frame_perf.frame_loop_ms") is not None for row in subset):
            print_timing_group(f"frame timings play_video={play_value}", subset, frame_fields)
            print_timing_group(
                f"crop preview timings play_video={play_value}",
                subset,
                crop_fields,
            )

    print(f"\ntop {args.top} slow frames")
    top_rows = sorted(rows, key=lambda row: get_nested(row, "frame_loop_ms") or -1.0, reverse=True)[
        : args.top
    ]
    for row in top_rows:
        print(
            "frame={frame} play={play} loop={loop:.3f} mask={mask:.3f} "
            "ui={ui:.3f} imgui={imgui:.3f} gl={gl:.3f} swap={swap:.3f} "
            "cap_sleep={cap_sleep:.3f} "
            "camera_upload={cam_upload:.3f} plot={plot:.3f} decoder_wait={dec_wait:.3f} "
            "shape_overlay={shape_overlay:.3f} tail_overlay={tail_overlay:.3f} "
            "crop={crop:.3f} crop_source={crop_source} rotated={rotated} "
            "mod32={mod32} crop_refresh={crop_refresh:.3f} "
            "crop_render={crop_render:.3f} crop_raw={crop_raw:.3f} "
            "crop_rot={crop_rot:.3f} crop_setup={crop_setup:.3f} "
            "crop_begin={crop_begin:.3f} crop_resolve={crop_resolve:.3f} "
            "crop_context={crop_context:.3f} crop_editor={crop_editor:.3f} "
            "crop_post={crop_post:.3f} crop_end={crop_end:.3f} "
            "crop_finish={crop_finish:.3f}"
            .format(
                frame=row.get("current_frame_num"),
                play=row.get("play_video"),
                loop=get_nested(row, "frame_loop_ms") or 0.0,
                mask=get_nested(row, "metrics.total_draw_ms") or 0.0,
                ui=get_nested(row, "frame_perf.ui.build_ms") or 0.0,
                imgui=get_nested(row, "frame_perf.ui.imgui_render_ms") or 0.0,
                gl=get_nested(row, "frame_perf.render.gl_draw_ms") or 0.0,
                swap=get_nested(row, "frame_perf.render.swap_ms") or 0.0,
                cap_sleep=get_nested(
                    row, "frame_perf.render.frame_cap_sleep_ms"
                )
                or 0.0,
                cam_upload=get_nested(row, "frame_perf.camera_pipeline.upload_ms") or 0.0,
                plot=get_nested(row, "frame_perf.camera_pipeline.plot_image_ui_ms") or 0.0,
                dec_wait=get_nested(row, "frame_perf.decoder.buffer_wait_ms") or 0.0,
                shape_overlay=get_nested(
                    row, "frame_perf.camera_pipeline.subject_shape_overlay_ms"
                )
                or 0.0,
                tail_overlay=get_nested(
                    row,
                    "frame_perf.camera_pipeline.tail_kinematics_overlay_ms",
                )
                or 0.0,
                crop=get_nested(row, "frame_perf.ui.crop_preview_ms") or 0.0,
                crop_source=get_path(
                    row, "frame_perf.ui.crop_preview_perf.crop_source"
                ),
                rotated=get_path(
                    row, "frame_perf.ui.crop_preview_perf.rotated_requested"
                ),
                mod32=get_path(row, "frame_perf.ui.crop_preview_perf.frame_mod32"),
                crop_refresh=get_nested(
                    row, "frame_perf.ui.crop_preview_perf.refresh_ms"
                )
                or 0.0,
                crop_render=get_nested(
                    row, "frame_perf.ui.crop_preview_perf.render_crop_texture_ms"
                )
                or 0.0,
                crop_raw=get_nested(
                    row, "frame_perf.ui.crop_preview_perf.get_raw_detections_ms"
                )
                or 0.0,
                crop_rot=get_nested(
                    row, "frame_perf.ui.crop_preview_perf.render_rotated_texture_ms"
                )
                or 0.0,
                crop_begin=get_nested(
                    row, "frame_perf.ui.crop_preview_perf.imgui_begin_ms"
                )
                or 0.0,
                crop_setup=get_nested(
                    row, "frame_perf.ui.crop_preview_perf.window_setup_ms"
                )
                or 0.0,
                crop_resolve=get_nested(
                    row, "frame_perf.ui.crop_preview_perf.resolve_selection_ms"
                )
                or 0.0,
                crop_context=get_nested(
                    row, "frame_perf.ui.crop_preview_perf.context_build_ms"
                )
                or 0.0,
                crop_editor=get_nested(
                    row, "frame_perf.ui.crop_preview_perf.editor_panel_ms"
                )
                or 0.0,
                crop_post=get_nested(
                    row, "frame_perf.ui.crop_preview_perf.post_panel_ms"
                )
                or 0.0,
                crop_end=get_nested(
                    row, "frame_perf.ui.crop_preview_perf.imgui_end_ms"
                )
                or 0.0,
                crop_finish=get_nested(
                    row, "frame_perf.ui.crop_preview_perf.finish_ms"
                )
                or 0.0,
            )
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
