#!/usr/bin/env python3

import argparse
import json
import os
import tempfile
from pathlib import Path

os.environ.setdefault(
    "MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "crimson-matplotlib-cache")
)

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


MIB = 1024 * 1024


def mib(value: int | float) -> float:
    return value / MIB


def load_evidence(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as stream:
        evidence = json.load(stream)
    endurance = evidence.get("endurance")
    if not endurance or not endurance.get("cycles"):
        raise ValueError("evidence has no endurance cycles")
    return evidence


def plot_memory(axis, endurance: dict) -> None:
    cycles = endurance["cycles"]
    x_values = [cycle["cycle"] for cycle in cycles]
    rss = [mib(cycle["memory"]["process"]["current_rss_bytes"]) for cycle in cycles]
    retained = [mib(cycle["memory"]["reported_retained_bytes"]) for cycle in cycles]
    axis.plot(x_values, rss, marker="o", color="#176B87", label="Current RSS")
    axis.plot(
        x_values,
        retained,
        marker="s",
        color="#2F855A",
        label="Reported retained lower bound",
    )
    warmup = endurance["rss_plateau"]["policy"]["warmup_samples"]
    axis.axvspan(-0.5, warmup - 0.5, color="#D1D5DB", alpha=0.35, label="Warmup")
    axis.set_title("Cycle-end memory plateau")
    axis.set_xlabel("Endurance cycle")
    axis.set_ylabel("Memory (MiB)")
    axis.grid(axis="y", color="#D1D5DB", linewidth=0.7, alpha=0.7)
    axis.legend(frameon=False)


def plot_cycle_io(axis, endurance: dict) -> None:
    cycles = endurance["cycles"]
    x_values = [cycle["cycle"] for cycle in cycles]
    file_bytes = [mib(cycle["physical"]["file_bytes"]) for cycle in cycles]
    file_reads = [cycle["physical"]["file_reads"] for cycle in cycles]
    bars = axis.bar(x_values, file_bytes, color="#4C78A8", label="Transferred bytes")
    axis.bar_label(bars, fmt="%.1f", padding=3, fontsize=8)
    axis.set_title("Physical file traffic per cycle")
    axis.set_xlabel("Endurance cycle")
    axis.set_ylabel("Transferred bytes (MiB)")
    axis.grid(axis="y", color="#D1D5DB", linewidth=0.7, alpha=0.7)
    reads_axis = axis.twinx()
    reads_axis.plot(x_values, file_reads, color="#B2432F", marker="o", label="File reads")
    reads_axis.set_ylabel("File reads")
    handles, labels = axis.get_legend_handles_labels()
    more_handles, more_labels = reads_axis.get_legend_handles_labels()
    axis.legend(handles + more_handles, labels + more_labels, frameon=False)


def plot_cache_pressure(axis, endurance: dict) -> None:
    cycles = endurance["cycles"]
    x_values = [cycle["cycle"] for cycle in cycles]
    detection = [
        mib(cycle["product_metrics"]["canonical_detection"]["cached_bytes"])
        for cycle in cycles
    ]
    mask_mapping = [
        mib(cycle["product_metrics"]["subject_masks"]["cached_mapping_bytes"])
        for cycle in cycles
    ]
    mask_payload = [
        mib(cycle["product_metrics"]["subject_masks"]["cached_payload_bytes"])
        for cycle in cycles
    ]
    axis.plot(x_values, detection, marker="o", label="Detection pages")
    axis.plot(x_values, mask_mapping, marker="s", label="Mask mapping pages")
    axis.plot(x_values, mask_payload, marker="^", label="Mask payload chunks")
    axis.set_title("Application-owned cache occupancy")
    axis.set_xlabel("Endurance cycle")
    axis.set_ylabel("Current retained bytes (MiB)")
    axis.grid(axis="y", color="#D1D5DB", linewidth=0.7, alpha=0.7)
    axis.legend(frameon=False)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Plot Crimson full-archive endurance evidence"
    )
    parser.add_argument("evidence", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    evidence = load_evidence(args.evidence)
    endurance = evidence["endurance"]
    figure, axes = plt.subplots(3, 1, figsize=(13, 13), constrained_layout=True)
    plot_memory(axes[0], endurance)
    plot_cycle_io(axes[1], endurance)
    plot_cache_pressure(axes[2], endurance)
    figure.suptitle(
        "Crimson full-archive endurance checkpoint\n"
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
