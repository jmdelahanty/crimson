#!/usr/bin/env python3
"""Plot the paired refined-detection physical-profile canary."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

import matplotlib.pyplot as plt


LAYOUTS = ("regular", "access_aware")
LABELS = ("Regular", "Access-aware")
COLORS = ("#3973AC", "#C75B39")


def values(
    trials: list[dict[str, Any]], layout: str, metric: str
) -> list[float]:
    return [
        float(trial["metrics"][metric])
        for trial in trials
        if trial.get("pass") and trial["layout"] == layout
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
        [sample / scale for sample in values(trials, layout, metric)]
        for layout in LAYOUTS
    ]
    plot = axis.boxplot(series, patch_artist=True, widths=0.5, showmeans=True)
    for patch, color in zip(plot["boxes"], COLORS):
        patch.set_facecolor(color)
        patch.set_alpha(0.82)
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
        "refined_detection_physical_profile_comparison.png"
    )

    plt.rcParams.update({"font.size": 9, "axes.titleweight": "bold"})
    figure, axes = plt.subplots(2, 2, figsize=(10.5, 7.2), constrained_layout=True)
    draw_boxplot(
        axes[0, 0], trials, "ready_ms", "Detection readiness", "Elapsed (ms)"
    )
    draw_boxplot(
        axes[0, 1],
        trials,
        "current_frame_p95_ms",
        "Current-frame p95",
        "Request to publication (ms)",
    )
    draw_boxplot(
        axes[1, 0],
        trials,
        "total_file_bytes",
        "Whole-process file transfer",
        "TensorStore file bytes (MiB)",
        1024.0 * 1024.0,
    )
    draw_boxplot(
        axes[1, 1],
        trials,
        "traversal_file_bytes",
        "7,000-frame traversal transfer",
        "TensorStore file bytes (MiB)",
        1024.0 * 1024.0,
    )

    comparisons = document["verdict"]["comparisons"]
    traversal_reduction = (
        1.0 - comparisons["access_aware_traversal_file_bytes_ratio"]
    ) * 100.0
    total_reduction = (
        1.0 - comparisons["access_aware_total_file_bytes_ratio"]
    ) * 100.0
    figure.suptitle(
        "Crimson refined-detection physical-profile canary\n"
        f"Access-aware reduction: {traversal_reduction:.1f}% traversal, "
        f"{total_reduction:.1f}% whole process",
        fontsize=14,
    )
    figure.savefig(output, dpi=180)
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
