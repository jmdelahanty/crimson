#!/usr/bin/env python3
"""Plot Phase 5O.5 detection-residency benchmark evidence."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

import matplotlib.pyplot as plt


CONDITIONS = [
    "hybrid_paged",
    "hybrid_resident",
    "regular_paged",
    "regular_resident",
]
LABELS = ["Hybrid\npaged", "Hybrid\nresident", "Regular\npaged", "Regular\nresident"]
COLORS = ["#3973AC", "#3B8C6E", "#A85E38", "#76589B"]


def load_trials(path: Path) -> list[dict[str, Any]]:
    trials = json.loads(path.read_text())
    complete = [trial for trial in trials if trial.get("pass")]
    if not complete:
        raise SystemExit("No successful trials are available to plot")
    return complete


def values(trials: list[dict[str, Any]], condition: str, metric: str) -> list[float]:
    return [
        float(trial["metrics"][metric])
        for trial in trials
        if trial["condition"] == condition
    ]


def draw_boxplot(
    axis: Any,
    trials: list[dict[str, Any]],
    metric: str,
    title: str,
    ylabel: str,
    scale: float = 1.0,
) -> None:
    series = [[value / scale for value in values(trials, condition, metric)] for condition in CONDITIONS]
    plot = axis.boxplot(series, patch_artist=True, widths=0.58, showmeans=True)
    for patch, color in zip(plot["boxes"], COLORS):
        patch.set_facecolor(color)
        patch.set_alpha(0.78)
    axis.set_xticks(range(1, len(LABELS) + 1), LABELS)
    axis.set_title(title)
    axis.set_ylabel(ylabel)
    axis.grid(axis="y", alpha=0.25)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("evidence", type=Path, help="Evidence directory or trials.json")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    trials_path = args.evidence / "trials.json" if args.evidence.is_dir() else args.evidence
    output = args.output or trials_path.with_name("residency_strategy_comparison.png")
    trials = load_trials(trials_path)

    plt.rcParams.update({"font.size": 9, "axes.titleweight": "bold"})
    figure, axes = plt.subplots(2, 2, figsize=(11, 7.5), constrained_layout=True)
    draw_boxplot(
        axes[0, 0],
        trials,
        "first_page_presentation_ms",
        "First page before strategy transition",
        "Presentation latency (ms)",
    )
    draw_boxplot(
        axes[0, 1],
        trials,
        "random_p95_ms",
        "120 random frames",
        "Per-process p95 (ms)",
    )
    draw_boxplot(
        axes[1, 0],
        trials,
        "traversal_file_bytes",
        "3,500-frame traversal transfer",
        "File bytes (MiB)",
        1024 * 1024,
    )
    draw_boxplot(
        axes[1, 1],
        trials,
        "peak_rss_bytes",
        "Detection-process peak memory",
        "Peak RSS (MiB)",
        1024 * 1024,
    )
    figure.suptitle("Crimson canonical-detection paging vs UI residency", fontsize=14)
    figure.savefig(output, dpi=180)
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
