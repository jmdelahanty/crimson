#!/usr/bin/env python3
"""Create an SVG cache/read profile from canonical detection benchmark CSV."""

from __future__ import annotations

import argparse
import csv
import math
from collections import defaultdict
from html import escape
from pathlib import Path
from statistics import median
from typing import Callable


WIDTH = 1500
HEIGHT = 1080
PANEL_WIDTH = 700
PANEL_HEIGHT = 300
LEFT = 75
TOP = 75
GAP_X = 45
GAP_Y = 45
COLORS = {"regular": "#1769aa", "hybrid": "#c73e1d"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot Crimson canonical detection cache benchmark results."
    )
    parser.add_argument("csv_path", type=Path)
    parser.add_argument("--output", type=Path, default=None)
    return parser.parse_args()


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def aggregate(
    rows: list[dict[str, str]], workload: str, metric: str
) -> dict[tuple[str, int, int], float]:
    values: dict[tuple[str, int, int], list[float]] = defaultdict(list)
    for row in rows:
        if row["workload"] != workload or row.get(metric, "") == "":
            continue
        key = (row["layout"], int(row["pass_index"]), int(row["cache_bytes"]))
        values[key].append(float(row[metric]))
    return {key: median(samples) for key, samples in values.items()}


def aggregate_samples(
    rows: list[dict[str, str]], workload: str, metric: str
) -> dict[tuple[str, int, int], list[float]]:
    values: dict[tuple[str, int, int], list[float]] = defaultdict(list)
    for row in rows:
        if row["workload"] != workload or row.get(metric, "") == "":
            continue
        key = (row["layout"], int(row["pass_index"]), int(row["cache_bytes"]))
        values[key].append(float(row[metric]))
    return values


def format_number(value: float) -> str:
    magnitude = abs(value)
    if magnitude >= 1_000_000_000:
        return f"{value / 1_000_000_000:.1f}G"
    if magnitude >= 1_000_000:
        return f"{value / 1_000_000:.1f}M"
    if magnitude >= 1_000:
        return f"{value / 1_000:.1f}k"
    if magnitude >= 100:
        return f"{value:.0f}"
    if magnitude >= 10:
        return f"{value:.1f}"
    return f"{value:.2f}"


def linear_scale(values: list[float]) -> tuple[Callable[[float], float], list[float]]:
    maximum = max(values) if values else 1.0
    maximum = maximum * 1.08 if maximum > 0 else 1.0
    ticks = [maximum * index / 4 for index in range(5)]
    return lambda value: value / maximum, ticks


def log_scale(values: list[float]) -> tuple[Callable[[float], float], list[float]]:
    positive = [value for value in values if value > 0]
    if not positive:
        return linear_scale(values)
    lower_power = math.floor(math.log10(min(positive)))
    upper_power = math.ceil(math.log10(max(positive)))
    if upper_power == lower_power:
        upper_power += 1
    ticks = [10.0**power for power in range(lower_power, upper_power + 1)]
    span = upper_power - lower_power

    def scale(value: float) -> float:
        if value <= 0:
            return 0.0
        return (math.log10(value) - lower_power) / span

    return scale, ticks


def svg_text(
    x: float,
    y: float,
    value: str,
    size: int = 15,
    anchor: str = "start",
    weight: int = 400,
    fill: str = "#202124",
) -> str:
    return (
        f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" '
        f'text-anchor="{anchor}" font-weight="{weight}" fill="{fill}">'
        f"{escape(value)}</text>"
    )


def panel(
    x: float,
    y: float,
    title: str,
    subtitle: str,
    values: dict[tuple[str, int, int], float],
    cache_values: list[int],
    log_y: bool,
) -> list[str]:
    output = [
        f'<rect x="{x}" y="{y}" width="{PANEL_WIDTH}" height="{PANEL_HEIGHT}" '
        'fill="#ffffff" stroke="#c9cdd2"/>',
        svg_text(x + 18, y + 28, title, size=18, weight=600),
        svg_text(x + 18, y + 49, subtitle, size=12, fill="#5f6368"),
    ]
    plot_x = x + 75
    plot_y = y + 65
    plot_width = PANEL_WIDTH - 105
    plot_height = PANEL_HEIGHT - 115
    all_values = list(values.values())
    scale, ticks = log_scale(all_values) if log_y else linear_scale(all_values)

    for tick in ticks:
        fraction = max(0.0, min(1.0, scale(tick)))
        tick_y = plot_y + plot_height * (1.0 - fraction)
        output.append(
            f'<line x1="{plot_x}" y1="{tick_y:.1f}" '
            f'x2="{plot_x + plot_width}" y2="{tick_y:.1f}" '
            'stroke="#e8eaed"/>'
        )
        output.append(
            svg_text(plot_x - 10, tick_y + 5, format_number(tick), 11, "end")
        )

    denominator = max(1, len(cache_values) - 1)
    x_positions = {
        cache: plot_x + plot_width * index / denominator
        for index, cache in enumerate(cache_values)
    }
    for cache, position in x_positions.items():
        output.append(
            f'<line x1="{position:.1f}" y1="{plot_y}" '
            f'x2="{position:.1f}" y2="{plot_y + plot_height}" '
            'stroke="#f1f3f4"/>'
        )
        output.append(
            svg_text(
                position,
                plot_y + plot_height + 23,
                format_number(cache / (1024 * 1024)),
                12,
                "middle",
            )
        )
    output.append(
        svg_text(
            plot_x + plot_width / 2,
            plot_y + plot_height + 43,
            "TensorStore cache limit (MiB)",
            12,
            "middle",
        )
    )

    for layout in ("regular", "hybrid"):
        for pass_index in (0, 1):
            points = []
            for cache in cache_values:
                value = values.get((layout, pass_index, cache))
                if value is None:
                    continue
                fraction = max(0.0, min(1.0, scale(value)))
                points.append(
                    (x_positions[cache], plot_y + plot_height * (1.0 - fraction), value)
                )
            if not points:
                continue
            dash = "" if pass_index == 0 else ' stroke-dasharray="7 5"'
            output.append(
                '<polyline points="'
                + " ".join(f"{px:.1f},{py:.1f}" for px, py, _ in points)
                + f'" fill="none" stroke="{COLORS[layout]}" stroke-width="2.5"{dash}/>'
            )
            for point_x, point_y, value in points:
                output.append(
                    f'<circle cx="{point_x:.1f}" cy="{point_y:.1f}" r="4" '
                    f'fill="{COLORS[layout]}"><title>{escape(layout)} pass '
                    f'{pass_index}: {value:.4f}</title></circle>'
                )
    return output


def matplotlib_plot(
    rows: list[dict[str, str]], output_path: Path, specifications: list[tuple]
) -> None:
    import matplotlib.pyplot as plt

    cache_values = sorted({int(row["cache_bytes"]) for row in rows})
    x_values = [value / (1024 * 1024) for value in cache_values]
    figure, axes = plt.subplots(3, 2, figsize=(14, 10), constrained_layout=True)
    markers = {0: "o", 1: "s"}
    line_styles = {0: "-", 1: "--"}
    for axis, (title, subtitle, workload, metric, log_y) in zip(
        axes.flat, specifications
    ):
        samples = aggregate_samples(rows, workload, metric)
        for layout in ("regular", "hybrid"):
            for pass_index in (0, 1):
                medians: list[float] = []
                lower: list[float] = []
                upper: list[float] = []
                for cache_bytes in cache_values:
                    group = samples.get((layout, pass_index, cache_bytes), [])
                    center = median(group)
                    medians.append(center)
                    lower.append(center - min(group))
                    upper.append(max(group) - center)
                axis.errorbar(
                    x_values,
                    medians,
                    yerr=[lower, upper],
                    color=COLORS[layout],
                    marker=markers[pass_index],
                    linestyle=line_styles[pass_index],
                    linewidth=2,
                    capsize=4,
                    label=f"{layout}, pass {pass_index}",
                )
        if log_y:
            axis.set_yscale("log")
        axis.set_title(title, loc="left", fontweight="semibold", pad=28)
        axis.text(
            0,
            1.01,
            subtitle,
            transform=axis.transAxes,
            fontsize=9,
            color="#5f6368",
            va="bottom",
        )
        axis.set_xlabel("TensorStore cache limit (MiB)")
        axis.grid(True, which="both", alpha=0.25)
        axis.set_xticks(x_values)
    handles, labels = axes.flat[0].get_legend_handles_labels()
    figure.legend(handles, labels, loc="outside lower center", ncol=4)
    figure.suptitle(
        "Crimson canonical detection storage profile\n"
        "points are five-run medians; error bars span min to max",
        fontsize=16,
        fontweight="semibold",
    )
    figure.savefig(output_path, dpi=180)
    plt.close(figure)


def main() -> None:
    args = parse_args()
    output_path = args.output or args.csv_path.with_suffix(".svg")
    rows = load_rows(args.csv_path)
    if not rows:
        raise SystemExit("No benchmark rows found")
    specifications = [
        (
            "UI random-frame p95 latency",
            "Median across repetitions; lower is better (log scale)",
            "random_frames_ui",
            "p95_unit_ms",
            True,
        ),
        (
            "UI random-frame file bytes",
            "Median TensorStore file bytes per workload; lower is better (log scale)",
            "random_frames_ui",
            "file_bytes",
            True,
        ),
        (
            "Contract forward traversal throughput",
            "700-frame windows; higher is better (log scale)",
            "sequential_contract_forward",
            "storage_frames_per_second",
            True,
        ),
        (
            "Contract forward file reads",
            "Median file-range reads; lower is better (log scale)",
            "sequential_contract_forward",
            "file_reads",
            True,
        ),
        (
            "Contract forward TensorStore cache hits",
            "Decoded/index cache reuse; higher indicates reuse",
            "sequential_contract_forward",
            "cache_hits",
            False,
        ),
        (
            "Contract forward TensorStore cache misses",
            "Requests absent from TensorStore cache; lower is better (log scale)",
            "sequential_contract_forward",
            "cache_misses",
            True,
        ),
    ]
    if output_path.suffix.lower() != ".svg":
        matplotlib_plot(rows, output_path, specifications)
        print(output_path)
        return

    cache_values = sorted({int(row["cache_bytes"]) for row in rows})
    elements = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{HEIGHT}" '
        f'viewBox="0 0 {WIDTH} {HEIGHT}">',
        '<rect width="100%" height="100%" fill="#f6f8fa"/>',
        svg_text(LEFT, 38, "Crimson canonical detection storage profile", 25, weight=650),
        svg_text(
            LEFT,
            61,
            "Solid: workload first pass. Dashed: immediate repeat. All workloads share one process cache.",
            13,
            fill="#5f6368",
        ),
    ]
    for index, (title, subtitle, workload, metric, log_y) in enumerate(
        specifications
    ):
        column = index % 2
        row = index // 2
        x = LEFT + column * (PANEL_WIDTH + GAP_X)
        y = TOP + row * (PANEL_HEIGHT + GAP_Y)
        elements.extend(
            panel(
                x,
                y,
                title,
                subtitle,
                aggregate(rows, workload, metric),
                cache_values,
                log_y,
            )
        )

    legend_y = HEIGHT - 22
    legend_x = LEFT
    for layout in ("regular", "hybrid"):
        elements.append(
            f'<line x1="{legend_x}" y1="{legend_y - 5}" '
            f'x2="{legend_x + 34}" y2="{legend_y - 5}" '
            f'stroke="{COLORS[layout]}" stroke-width="3"/>'
        )
        elements.append(svg_text(legend_x + 43, legend_y, layout, 13))
        legend_x += 145
    elements.append("</svg>")
    output_path.write_text("\n".join(elements) + "\n")
    print(output_path)


if __name__ == "__main__":
    main()
