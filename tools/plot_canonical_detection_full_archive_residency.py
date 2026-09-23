#!/usr/bin/env python3
"""Plot Phase 5O.5 full-archive residency interference evidence."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

import matplotlib.pyplot as plt


STRATEGIES = ("paged", "resident")
LABELS = ("Paged", "Resident")
COLORS = ("#3973AC", "#3B8C6E")


def metric_values(
    trials: list[dict[str, Any]], strategy: str, metric: str
) -> list[float]:
    return [
        float(trial["metrics"][metric])
        for trial in trials
        if trial.get("pass") and trial["strategy"] == strategy
    ]


def draw_boxplot(
    axis: Any,
    trials: list[dict[str, Any]],
    metric: str,
    title: str,
    ylabel: str,
    scale: float = 1.0,
) -> None:
    series = [
        [value / scale for value in metric_values(trials, strategy, metric)]
        for strategy in STRATEGIES
    ]
    plot = axis.boxplot(series, patch_artist=True, widths=0.5, showmeans=True)
    for patch, color in zip(plot["boxes"], COLORS):
        patch.set_facecolor(color)
        patch.set_alpha(0.8)
    axis.set_xticks((1, 2), LABELS)
    axis.set_title(title)
    axis.set_ylabel(ylabel)
    axis.grid(axis="y", alpha=0.25)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("aggregate", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    document = json.loads(args.aggregate.read_text(encoding="utf-8"))
    trials = document["trials"]
    output = args.output or args.aggregate.with_name(
        "full_archive_residency_comparison.png"
    )

    plt.rcParams.update({"font.size": 9, "axes.titleweight": "bold"})
    figure, axes = plt.subplots(2, 2, figsize=(11, 7.5), constrained_layout=True)
    draw_boxplot(
        axes[0, 0],
        trials,
        "required_products_ready_ms",
        "Required products ready",
        "Elapsed time (s)",
        1000.0,
    )
    draw_boxplot(
        axes[0, 1],
        trials,
        "current_frame_request_to_publish_ms",
        "Demand probe during initialization",
        "Request to publication (ms)",
    )
    draw_boxplot(
        axes[1, 0],
        trials,
        "peak_rss_bytes",
        "Whole-process peak memory",
        "Peak RSS (MiB)",
        1024.0 * 1024.0,
    )

    product_ratios = document["reduction"]["product_regressions"]
    names = list(product_ratios)
    ratios = [float(product_ratios[name]["ratio"]) for name in names]
    positions = list(range(len(names)))
    axes[1, 1].barh(positions, ratios, color="#6B7280", alpha=0.85)
    axes[1, 1].axvline(1.0, color="#222222", linewidth=1.0)
    axes[1, 1].axvline(1.1, color="#B33A3A", linewidth=1.0, linestyle="--")
    axes[1, 1].set_yticks(positions, [name.replace("_", " ") for name in names])
    axes[1, 1].invert_yaxis()
    axes[1, 1].set_xlim(0.85, 1.12)
    axes[1, 1].set_xlabel("Resident / paged median")
    axes[1, 1].set_title("Maintained product initialization")
    axes[1, 1].grid(axis="x", alpha=0.25)

    figure.suptitle(
        "Crimson full-archive detection residency interference", fontsize=14
    )
    figure.savefig(output, dpi=180)
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
