#!/usr/bin/env python3
"""Plot the Crimson cache/read-ahead prefetch matrix."""

from __future__ import annotations

import argparse
import csv
import os
import tempfile
from collections import defaultdict
from pathlib import Path
from statistics import median


MIB = 1024 * 1024
COLORS = {0: "#5f6368", 350: "#1769aa", 700: "#c73e1d"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("prefetch_csv", type=Path)
    parser.add_argument("seek_csv", type=Path)
    parser.add_argument("--output-prefix", type=Path, required=True)
    return parser.parse_args()


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def aggregate(
    rows: list[dict[str, str]],
    metric: str,
    direction: str | None = None,
    first_phase_only: bool = False,
) -> dict[tuple[str, int, int], float]:
    values: dict[tuple[str, int, int], list[float]] = defaultdict(list)
    for row in rows:
        if direction is not None and row.get("direction") != direction:
            continue
        if first_phase_only and int(row.get("direction_order_index", "-1")) != 0:
            continue
        key = (
            row["layout"],
            int(row["cache_bytes"]),
            int(row["read_ahead_frames"]),
        )
        values[key].append(float(row[metric]))
    return {key: median(samples) for key, samples in values.items()}


def plot_lines(axis, values: dict[tuple[str, int, int], float], ylabel: str) -> None:
    caches = sorted(
        {cache for layout, cache, _ in values if layout == "hybrid"}
    )
    lookaheads = sorted(
        {ahead for layout, _, ahead in values if layout == "hybrid"}
    )
    for ahead in lookaheads:
        y_values = [values[("hybrid", cache, ahead)] for cache in caches]
        axis.plot(
            [cache / MIB for cache in caches],
            y_values,
            color=COLORS.get(ahead, "#188038"),
            marker="o",
            linewidth=2,
            label=f"hybrid, {ahead / 700:.1f} s",
        )
    anchor = values.get(("regular", 128 * MIB, 0))
    if anchor is not None:
        axis.scatter(
            [128],
            [anchor],
            color="#7b1fa2",
            marker="D",
            s=60,
            label="regular anchor",
            zorder=5,
        )
    axis.set_xlabel("TensorStore cache limit (MiB)")
    axis.set_ylabel(ylabel)
    axis.set_xticks([cache / MIB for cache in caches])
    axis.grid(True, alpha=0.25)


def main() -> None:
    args = parse_args()
    os.environ.setdefault(
        "MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "crimson-matplotlib-cache")
    )
    import matplotlib.pyplot as plt

    prefetch = load_rows(args.prefetch_csv)
    seeks = load_rows(args.seek_csv)
    if not prefetch or not seeks:
        raise SystemExit("Benchmark CSV files are empty")

    figure, axes = plt.subplots(2, 2, figsize=(14, 9), constrained_layout=True)
    plots = [
        (
            axes[0, 0],
            aggregate(
                prefetch,
                "post_warmup_deadline_miss_rate",
                first_phase_only=True,
            ),
            "First-phase overlay deadline misses",
            "post-warmup miss rate",
        ),
        (
            axes[0, 1],
            aggregate(prefetch, "file_bytes", first_phase_only=True),
            "First-phase physical transfer",
            "file bytes",
        ),
        (
            axes[1, 0],
            aggregate(seeks, "cancellation_p95_ms"),
            "Random-seek cancellation",
            "cancellation p95 (ms)",
        ),
        (
            axes[1, 1],
            aggregate(seeks, "peak_rss_bytes"),
            "Total process memory",
            "peak RSS (bytes)",
        ),
    ]
    for axis, values, title, ylabel in plots:
        plot_lines(axis, values, ylabel)
        axis.set_title(title, loc="left", fontweight="semibold")
    axes[0, 0].axhline(0.01, color="#d93025", linestyle="--", linewidth=1)
    axes[1, 0].axhline(250.0, color="#d93025", linestyle="--", linewidth=1)
    axes[1, 1].axhline(768 * MIB, color="#d93025", linestyle="--", linewidth=1)
    handles, labels = axes[0, 0].get_legend_handles_labels()
    figure.legend(handles, labels, loc="outside lower center", ncol=4)
    figure.suptitle(
        "Crimson canonical detection prefetch profile\n"
        "points are five-process medians; red lines are frozen limits",
        fontsize=16,
        fontweight="semibold",
    )
    args.output_prefix.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(args.output_prefix.with_suffix(".png"), dpi=180)
    figure.savefig(args.output_prefix.with_suffix(".svg"))
    plt.close(figure)


if __name__ == "__main__":
    main()
