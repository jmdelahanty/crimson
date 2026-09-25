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
    attribution = evidence.get("memory_attribution", {})
    if not attribution.get("timeline") or not attribution.get("phase_snapshots"):
        raise ValueError("evidence has no memory-attribution timeline or snapshots")
    return evidence


def plot_timeline(axis, evidence: dict) -> None:
    timeline = evidence["memory_attribution"]["timeline"]
    elapsed = [sample["elapsed_ms"] / 1000.0 for sample in timeline]
    current = [mib(sample["current_rss_bytes"]) for sample in timeline]
    peak = [mib(sample["peak_rss_bytes"]) for sample in timeline]

    axis.plot(elapsed, current, color="#176B87", linewidth=1.7, label="Current RSS")
    axis.plot(
        elapsed,
        peak,
        color="#B2432F",
        linewidth=1.1,
        linestyle="--",
        label="Lifetime peak RSS",
    )

    product_events = [
        sample
        for sample in timeline
        if sample.get("event", "").startswith("product_ready:")
        and sample["event"].endswith(":available")
    ]
    for index, sample in enumerate(product_events):
        product = sample["event"].split(":", 2)[1].replace("_", " ")
        x_value = sample["elapsed_ms"] / 1000.0
        axis.axvline(x_value, color="#6B7280", linewidth=0.6, alpha=0.35)
        axis.text(
            x_value,
            0.98 if index % 2 == 0 else 0.81,
            product,
            transform=axis.get_xaxis_transform(),
            rotation=90,
            va="top",
            ha="right",
            fontsize=7,
            color="#374151",
        )

    axis.set_title("Process resident memory over the full-duration workload")
    axis.set_xlabel("Elapsed time (seconds)")
    axis.set_ylabel("Memory (MiB)")
    axis.grid(axis="y", color="#D1D5DB", linewidth=0.7, alpha=0.7)
    axis.legend(loc="upper left", frameon=False)


def plot_phases(axis, evidence: dict) -> None:
    snapshots = evidence["memory_attribution"]["phase_snapshots"]
    phases = [snapshot["phase"].replace("_", " ") for snapshot in snapshots]
    known = [mib(snapshot["reported_retained_bytes"]) for snapshot in snapshots]
    unattributed = [mib(snapshot["unattributed_rss_bytes"]) for snapshot in snapshots]
    positions = range(len(phases))

    axis.bar(positions, known, color="#2F855A", label="Reported retained lower bound")
    axis.bar(
        positions,
        unattributed,
        bottom=known,
        color="#D6A84B",
        label="Unattributed RSS",
    )
    axis.set_title("Attribution snapshots by workload phase")
    axis.set_ylabel("Current RSS (MiB)")
    axis.set_xticks(list(positions), phases, rotation=28, ha="right")
    axis.grid(axis="y", color="#D1D5DB", linewidth=0.7, alpha=0.7)
    axis.legend(loc="upper right", frameon=False)


def plot_owners(axis, evidence: dict) -> None:
    snapshots = evidence["memory_attribution"]["phase_snapshots"]
    snapshot = next(
        value for value in snapshots if value["phase"] == "before_shutdown"
    )
    owners = [owner for owner in snapshot["owners"] if owner["retained_bytes"] > 0]
    owners.sort(key=lambda owner: owner["retained_bytes"])
    names = [owner["owner"].replace("_", " ") for owner in owners]
    values = [mib(owner["retained_bytes"]) for owner in owners]
    colors = ["#5B8E7D" if value < 50 else "#8A5A44" for value in values]

    bars = axis.barh(names, values, color=colors)
    axis.bar_label(bars, fmt="%.1f", padding=4, fontsize=8)
    axis.set_title("Reported retained owners before shutdown")
    axis.set_xlabel("Retained memory lower bound (MiB)")
    axis.grid(axis="x", color="#D1D5DB", linewidth=0.7, alpha=0.7)
    axis.margins(x=0.12)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Plot Crimson full-archive process-memory attribution evidence"
    )
    parser.add_argument("evidence", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    evidence = load_evidence(args.evidence)
    figure, axes = plt.subplots(3, 1, figsize=(13, 14), constrained_layout=True)
    plot_timeline(axes[0], evidence)
    plot_phases(axes[1], evidence)
    plot_owners(axes[2], evidence)
    figure.suptitle(
        "Crimson full-duration memory attribution\n"
        f"{evidence['layout']} / {evidence['strategy']} | "
        f"commit {evidence['crimson_commit'][:12]}",
        fontsize=15,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(args.output, dpi=180)
    plt.close(figure)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
