#!/usr/bin/env python3
"""Render an SVG summary of keypoint-v2 long-duration aggregate evidence."""

from __future__ import annotations

import argparse
import json
import math
from html import escape
from pathlib import Path
from typing import Any, Callable


WIDTH = 1540
HEIGHT = 1490
PANEL_WIDTH = 720
PANEL_HEIGHT = 300
LEFT = 45
TOP = 95
GAP_X = 35
GAP_Y = 35
COLORS = (
    "#1769aa", "#c73e1d", "#188038", "#9334e6",
    "#b06000", "#007b83", "#ad1457", "#5f6368",
)

PANELS = (
    ("First-presentation readiness", "Fresh process through first overlay", (
        ("first_presentation_readiness_ms", "ready", 1.0),), "ms"),
    ("Exact repository open", "Manifest, declarations, handles, and offsets", (
        ("repository_open_ms", "repository", 1.0),), "ms"),
    ("First requested frame", "Request through validated publication", (
        ("first_presentation.elapsed_ms", "first frame", 1.0),), "ms"),
    ("Warm random-frame p95", "Second deterministic random pass", (
        ("random_warm.p95_ms", "warm p95", 1.0),), "ms"),
    ("Directional 70-frame page p95", "Solid: forward; hollow: reverse", (
        ("forward_traversal.page_p95_ms", "forward", 1.0),
        ("reverse_traversal.page_p95_ms", "reverse", 1.0),), "ms"),
    ("Current-frame scheduling", "Solid: queue maximum; hollow: service maximum", (
        ("scheduler_metrics.timing_by_priority.current_frame.queue_maximum_ms",
         "queue", 1.0),
        ("scheduler_metrics.timing_by_priority.current_frame.service_maximum_ms",
         "service", 1.0),), "ms"),
    ("Process file transfer", "TensorStore file-kvstore bytes", (
        ("process_physical.file_bytes", "transfer", 1.0 / (1024 * 1024)),),
     "MiB"),
    ("Memory and configured cache", "Solid: peak RSS; hollow: cache upper bound", (
        ("peak_rss_bytes", "RSS", 1.0 / (1024 * 1024)),
        ("configured_cache_bytes", "cache limit", 1.0 / (1024 * 1024)),),
     "MiB"),
)


def text(x: float, y: float, value: str, size: int = 13,
         anchor: str = "start", weight: int = 400,
         fill: str = "#202124") -> str:
    return (
        f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" '
        f'text-anchor="{anchor}" font-weight="{weight}" fill="{fill}">'
        f"{escape(value)}</text>"
    )


def format_value(value: float) -> str:
    magnitude = abs(value)
    if magnitude >= 1_000_000:
        return f"{value / 1_000_000:.1f}M"
    if magnitude >= 1_000:
        return f"{value / 1_000:.1f}k"
    if magnitude >= 100:
        return f"{value:.0f}"
    if magnitude >= 10:
        return f"{value:.1f}"
    return f"{value:.2f}"


def short_label(value: str, maximum: int = 24) -> str:
    if len(value) <= maximum:
        return value
    return value[: maximum - 1] + "…"


def linear_scale(maximum: float) -> tuple[Callable[[float], float], list[float]]:
    limit = maximum * 1.12 if maximum > 0 else 1.0
    ticks = [limit * index / 4 for index in range(5)]
    return lambda value: max(0.0, min(1.0, value / limit)), ticks


def render_panel(x: float, y: float, title: str, subtitle: str,
                 series: tuple[tuple[str, str, float], ...], unit: str,
                 summaries: dict[str, Any]) -> list[str]:
    candidates = list(summaries)
    output = [
        f'<rect x="{x}" y="{y}" width="{PANEL_WIDTH}" '
        f'height="{PANEL_HEIGHT}" fill="#ffffff" stroke="#c9cdd2"/>',
        text(x + 18, y + 28, title, 18, weight=600),
        text(x + 18, y + 49, subtitle, 12, fill="#5f6368"),
    ]
    plot_x = x + 68
    plot_y = y + 66
    plot_width = PANEL_WIDTH - 92
    plot_height = PANEL_HEIGHT - 118
    scaled_values: list[float] = []
    for candidate in candidates:
        metrics = summaries[candidate].get("metrics", {})
        for path, _, multiplier in series:
            if path in metrics:
                scaled_values.extend(
                    float(value) * multiplier for value in metrics[path]["values"]
                )
    scale, ticks = linear_scale(max(scaled_values, default=0.0))
    for tick in ticks:
        tick_y = plot_y + plot_height * (1.0 - scale(tick))
        output.append(
            f'<line x1="{plot_x}" y1="{tick_y:.1f}" '
            f'x2="{plot_x + plot_width}" y2="{tick_y:.1f}" '
            'stroke="#e8eaed"/>'
        )
        output.append(text(plot_x - 9, tick_y + 4, format_value(tick),
                           10, "end", fill="#5f6368"))
    output.append(text(plot_x - 9, plot_y - 7, unit, 10, "end",
                       fill="#5f6368"))

    candidate_span = plot_width / max(1, len(candidates))
    series_span = min(34.0, candidate_span / max(2, len(series) + 1))
    for candidate_index, candidate in enumerate(candidates):
        center = plot_x + candidate_span * (candidate_index + 0.5)
        color = COLORS[candidate_index % len(COLORS)]
        metrics = summaries[candidate].get("metrics", {})
        for series_index, (path, _, multiplier) in enumerate(series):
            if path not in metrics:
                continue
            values = [float(value) * multiplier
                      for value in metrics[path]["values"]]
            median = float(metrics[path]["median"]) * multiplier
            offset = (series_index - (len(series) - 1) / 2) * series_span
            point_x = center + offset
            minimum_y = plot_y + plot_height * (1.0 - scale(min(values)))
            maximum_y = plot_y + plot_height * (1.0 - scale(max(values)))
            median_y = plot_y + plot_height * (1.0 - scale(median))
            output.append(
                f'<line x1="{point_x:.1f}" y1="{minimum_y:.1f}" '
                f'x2="{point_x:.1f}" y2="{maximum_y:.1f}" '
                f'stroke="{color}" stroke-width="2"/>'
            )
            for value_index, value in enumerate(values):
                jitter = ((value_index % 3) - 1) * 2.5
                value_y = plot_y + plot_height * (1.0 - scale(value))
                output.append(
                    f'<circle cx="{point_x + jitter:.1f}" cy="{value_y:.1f}" '
                    f'r="2.2" fill="{color}" opacity="0.45"/>'
                )
            fill = color if series_index == 0 else "#ffffff"
            output.append(
                f'<circle cx="{point_x:.1f}" cy="{median_y:.1f}" r="5" '
                f'fill="{fill}" stroke="{color}" stroke-width="2"/>'
            )
        output.append(text(center, plot_y + plot_height + 24,
                           short_label(candidate), 10, "middle"))

    if len(series) > 1:
        legend_x = x + 18
        legend_y = y + PANEL_HEIGHT - 12
        for index, (_, label, _) in enumerate(series):
            fill = "#5f6368" if index == 0 else "#ffffff"
            output.append(
                f'<circle cx="{legend_x + 5}" cy="{legend_y - 4}" r="4" '
                f'fill="{fill}" stroke="#5f6368" stroke-width="1.5"/>'
            )
            output.append(text(legend_x + 14, legend_y, label, 10,
                               fill="#5f6368"))
            legend_x += 95
    return output


def plot_aggregate(aggregate: dict[str, Any], output_path: Path) -> None:
    summaries = aggregate.get("candidate_summaries", {})
    if not summaries:
        raise RuntimeError("Aggregate has no candidate summaries")
    elements = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" '
        f'height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}">',
        '<rect width="100%" height="100%" fill="#f6f8fa"/>',
        text(LEFT, 38, "Crimson keypoint-v2 fresh-process benchmark", 25,
             weight=650),
        text(LEFT, 62,
             "Points are process trials; circles are medians; whiskers are min–max.",
             13, fill="#5f6368"),
        text(WIDTH - LEFT, 38,
             f"status: {aggregate.get('status', 'unknown')}", 14, "end", 600,
             "#188038" if aggregate.get("status") == "pass" else "#c5221f"),
    ]
    for index, (title, subtitle, series, unit) in enumerate(PANELS):
        column = index % 2
        row = index // 2
        x = LEFT + column * (PANEL_WIDTH + GAP_X)
        y = TOP + row * (PANEL_HEIGHT + GAP_Y)
        elements.extend(
            render_panel(x, y, title, subtitle, series, unit, summaries)
        )
    legend_y = HEIGHT - 18
    legend_x = LEFT
    for index, candidate in enumerate(summaries):
        color = COLORS[index % len(COLORS)]
        elements.append(
            f'<rect x="{legend_x}" y="{legend_y - 12}" width="18" height="8" '
            f'fill="{color}"/>'
        )
        elements.append(text(legend_x + 25, legend_y,
                             short_label(candidate, 32), 11))
        legend_x += min(330, 55 + len(short_label(candidate, 32)) * 7)
    elements.append("</svg>")
    output_path.write_text("\n".join(elements) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot a keypoint-v2 aggregate JSON file")
    parser.add_argument("aggregate", type=Path)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    aggregate = json.loads(args.aggregate.read_text(encoding="utf-8"))
    output = args.output or args.aggregate.with_name("summary.svg")
    plot_aggregate(aggregate, output)
    print(output)


if __name__ == "__main__":
    main()
