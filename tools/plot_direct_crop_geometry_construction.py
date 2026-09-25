#!/usr/bin/env python3

import argparse
import json
from pathlib import Path

import matplotlib.pyplot as plt


MIB = 1024 * 1024


def mib(value: int) -> float:
    return value / MIB


def load_evidence(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as stream:
        evidence = json.load(stream)
    if not evidence.get("memory_attribution", {}).get("timeline"):
        raise ValueError(f"{path} has no memory-attribution timeline")
    return evidence


def crop_ready_sample(evidence: dict) -> dict:
    return next(
        sample
        for sample in evidence["memory_attribution"]["timeline"]
        if sample.get("event", "").startswith(
            "product_ready:crop_geometry:available"
        )
    )


def plot_crop_elapsed(axis, labels: list[str], evidence: list[dict]) -> None:
    values = [
        value["loading"]["products"]["crop_geometry"]["elapsed_ms"] / 1000.0
        for value in evidence
    ]
    colors = ["#8A5A44", "#8A5A44", "#2F855A", "#2F855A"]
    bars = axis.bar(labels, values, color=colors)
    axis.bar_label(bars, fmt="%.2f s", padding=4)
    axis.set_title("Crop-geometry product initialization")
    axis.set_ylabel("Elapsed time (seconds)")
    axis.grid(axis="y", color="#D1D5DB", linewidth=0.7, alpha=0.7)
    axis.margins(y=0.15)


def plot_crop_ready_rss(axis, labels: list[str], evidence: list[dict]) -> None:
    values = [mib(crop_ready_sample(value)["current_rss_bytes"]) for value in evidence]
    colors = ["#8A5A44", "#8A5A44", "#176B87", "#176B87"]
    bars = axis.bar(labels, values, color=colors)
    axis.bar_label(bars, fmt="%.1f MiB", padding=4)
    axis.set_title("Process RSS when crop geometry becomes ready")
    axis.set_ylabel("Current RSS (MiB)")
    axis.grid(axis="y", color="#D1D5DB", linewidth=0.7, alpha=0.7)
    axis.margins(y=0.15)


def plot_timelines(axis, labels: list[str], evidence: list[dict]) -> None:
    colors = ["#8A5A44", "#C47F5A", "#176B87", "#2F855A"]
    for label, value, color in zip(labels, evidence, colors, strict=True):
        timeline = value["memory_attribution"]["timeline"]
        elapsed = [sample["elapsed_ms"] / 1000.0 for sample in timeline]
        current = [mib(sample["current_rss_bytes"]) for sample in timeline]
        axis.plot(elapsed, current, color=color, linewidth=1.2, label=label)
    axis.set_title("Full-workload RSS timeline (later product variance retained)")
    axis.set_xlabel("Elapsed time (seconds)")
    axis.set_ylabel("Current RSS (MiB)")
    axis.grid(axis="y", color="#D1D5DB", linewidth=0.7, alpha=0.7)
    axis.legend(loc="upper right", frameon=False)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare adapter and direct crop-geometry construction"
    )
    parser.add_argument("compact_repetition_0", type=Path)
    parser.add_argument("compact_repetition_1", type=Path)
    parser.add_argument("direct_repetition_0", type=Path)
    parser.add_argument("direct_repetition_1", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    evidence = [
        load_evidence(args.compact_repetition_0),
        load_evidence(args.compact_repetition_1),
        load_evidence(args.direct_repetition_0),
        load_evidence(args.direct_repetition_1),
    ]
    labels = ["adapter rep 0", "adapter rep 1", "direct rep 3", "direct rep 4"]
    figure, axes = plt.subplots(3, 1, figsize=(13, 14), constrained_layout=True)
    plot_crop_elapsed(axes[0], labels, evidence)
    plot_crop_ready_rss(axes[1], labels, evidence)
    plot_timelines(axes[2], labels, evidence)
    figure.suptitle(
        "Crimson direct crop-column construction checkpoint\n"
        f"adapter {evidence[0]['crimson_commit'][:12]} | "
        f"direct {evidence[2]['crimson_commit'][:12]}",
        fontsize=15,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(args.output, dpi=180)
    plt.close(figure)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
