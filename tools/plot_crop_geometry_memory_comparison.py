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
    snapshots = evidence.get("memory_attribution", {}).get("phase_snapshots", [])
    if not snapshots:
        raise ValueError(f"{path} has no memory-attribution snapshots")
    return evidence


def snapshot(evidence: dict, phase: str) -> dict:
    return next(
        value
        for value in evidence["memory_attribution"]["phase_snapshots"]
        if value["phase"] == phase
    )


def crop_bytes(evidence: dict) -> int:
    ready = snapshot(evidence, "required_products_ready")
    return ready["repository_breakdown"]["crop_geometry"][
        "reported_retained_bytes"
    ]


def plot_crop_owner(axis, labels: list[str], evidence: list[dict]) -> None:
    values = [mib(crop_bytes(value)) for value in evidence]
    colors = ["#8A5A44", "#2F855A", "#2F855A"]
    bars = axis.bar(labels, values, color=colors)
    axis.bar_label(bars, fmt="%.1f MiB", padding=4)
    axis.set_title("Crop-geometry retained lower bound")
    axis.set_ylabel("Retained memory (MiB)")
    axis.grid(axis="y", color="#D1D5DB", linewidth=0.7, alpha=0.7)
    axis.margins(y=0.15)


def plot_ready_memory(axis, labels: list[str], evidence: list[dict]) -> None:
    snapshots = [snapshot(value, "required_products_ready") for value in evidence]
    reported = [mib(value["reported_retained_bytes"]) for value in snapshots]
    unattributed = [mib(value["unattributed_rss_bytes"]) for value in snapshots]
    positions = range(len(labels))
    axis.bar(
        positions,
        reported,
        color="#2F855A",
        label="Reported retained lower bound",
    )
    axis.bar(
        positions,
        unattributed,
        bottom=reported,
        color="#D6A84B",
        label="Unattributed RSS",
    )
    axis.set_title("Required-products-ready memory")
    axis.set_ylabel("Current RSS (MiB)")
    axis.set_xticks(list(positions), labels)
    axis.grid(axis="y", color="#D1D5DB", linewidth=0.7, alpha=0.7)
    axis.legend(loc="upper right", frameon=False)


def plot_timelines(axis, labels: list[str], evidence: list[dict]) -> None:
    colors = ["#8A5A44", "#176B87", "#6B46C1"]
    for label, value, color in zip(labels, evidence, colors, strict=True):
        timeline = value["memory_attribution"]["timeline"]
        elapsed = [sample["elapsed_ms"] / 1000.0 for sample in timeline]
        current = [mib(sample["current_rss_bytes"]) for sample in timeline]
        axis.plot(elapsed, current, color=color, linewidth=1.2, label=label)
    axis.set_title("Process RSS timeline (run-to-run cache and allocator variance retained)")
    axis.set_xlabel("Elapsed time (seconds)")
    axis.set_ylabel("Current RSS (MiB)")
    axis.grid(axis="y", color="#D1D5DB", linewidth=0.7, alpha=0.7)
    axis.legend(loc="upper right", frameon=False)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare Crimson crop-geometry memory representations"
    )
    parser.add_argument("baseline", type=Path)
    parser.add_argument("compact_repetition_0", type=Path)
    parser.add_argument("compact_repetition_1", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    evidence = [
        load_evidence(args.baseline),
        load_evidence(args.compact_repetition_0),
        load_evidence(args.compact_repetition_1),
    ]
    labels = ["row/hash baseline", "compact repetition 0", "compact repetition 1"]
    figure, axes = plt.subplots(3, 1, figsize=(13, 14), constrained_layout=True)
    plot_crop_owner(axes[0], labels, evidence)
    plot_ready_memory(axes[1], labels, evidence)
    plot_timelines(axes[2], labels, evidence)
    figure.suptitle(
        "Crimson compact crop-geometry memory checkpoint\n"
        f"baseline {evidence[0]['crimson_commit'][:12]} | "
        f"compact {evidence[1]['crimson_commit'][:12]}",
        fontsize=15,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(args.output, dpi=180)
    plt.close(figure)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
