#!/usr/bin/env python3
"""
Inspect refined eye-mask datasets inside a Palette Zarr store.

Example:
    python tools/inspect_eye_masks.py /path/to/session.zarr --rois 0 1 2

Reports basic metadata (dtype, shape, chunks) and per-ROI statistics so you can
see whether the masks are full of ones (rendering as a solid block), mostly
zeros, or contain unexpected values.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Iterable, Optional

import numpy as np
import zarr


def load_json_attr(attrs, key: str):
    value = attrs.get(key)
    if value is None:
        return None
    if isinstance(value, str):
        try:
            return json.loads(value)
        except json.JSONDecodeError:
            return value
    return value


def pick_latest_run(group: zarr.Group, subgroup: str) -> Optional[str]:
    if subgroup not in group:
        return None
    node = group[subgroup]
    latest = node.attrs.get("latest")
    if latest:
        return latest
    runs = sorted(node.group_keys())
    return runs[-1] if runs else None


def summarize_roi(mask: np.ndarray) -> dict:
    """Return simple statistics for a binary mask array (channels, rows, cols)."""
    stats = {}
    if mask.ndim != 3:
        stats["error"] = f"Unexpected ndim: {mask.ndim}"
        return stats

    channels, rows, cols = mask.shape
    stats["shape"] = (channels, rows, cols)
    stats["dtype"] = str(mask.dtype)
    total_pixels = rows * cols

    for c in range(channels):
        channel = mask[c]
        unique_vals, counts = np.unique(channel, return_counts=True)
        stats[f"channel_{c}_unique"] = dict(zip(map(int, unique_vals), counts.tolist()))
        stats[f"channel_{c}_nonzero_pct"] = float(np.count_nonzero(channel)) / total_pixels
        stats[f"channel_{c}_min"] = float(channel.min())
        stats[f"channel_{c}_max"] = float(channel.max())

    return stats


def inspect_masks(zarr_path: Path, rois: Iterable[int]) -> int:
    if not zarr_path.exists():
        print(f"[ERROR] Path not found: {zarr_path}", file=sys.stderr)
        return 2

    root = zarr.open(zarr_path, mode="r")
    print(f"[INFO] Opened Zarr store: {zarr_path}")

    run_name = pick_latest_run(root, "refined_eye_masks_runs")
    if run_name is None:
        print("[ERROR] No refined_eye_masks_runs subgroup found.", file=sys.stderr)
        return 1
    group = root["refined_eye_masks_runs"][run_name]
    print(f"[INFO] Using refined eye-mask run: {run_name}")

    if "masks_roi" not in group:
        print("[ERROR] masks_roi dataset not present.", file=sys.stderr)
        return 1

    masks = group["masks_roi"]
    print(f"[INFO] masks_roi dtype:      {masks.dtype}")
    print(f"[INFO] masks_roi shape:      {masks.shape}")
    print(f"[INFO] masks_roi chunks:     {masks.chunks}")

    if "frame_indices" in group:
        frame_indices = group["frame_indices"][:]
        print(f"[INFO] frame_indices length: {len(frame_indices)}")
    else:
        print("[WARN] frame_indices missing from this run.")

    rois = sorted(set(rois))
    max_roi_index = masks.shape[0] - 1
    print(f"[INFO] Inspecting ROIs: {rois} (max valid index {max_roi_index})")

    for roi in rois:
        if roi < 0 or roi > max_roi_index:
            print(f"[WARN] ROI {roi} out of range; skipping.")
            continue
        mask = masks.get_basic_selection((slice(roi, roi + 1), slice(None), slice(None), slice(None)))
        mask = np.asarray(mask)[0]  # drop roi axis
        stats = summarize_roi(mask)
        print(f"[ROI {roi}] stats:")
        for key, value in stats.items():
            print(f"    {key}: {value}")

    return 0


def parse_args(argv: Optional[list[str]] = None):
    parser = argparse.ArgumentParser(description="Inspect refined eye-mask datasets.")
    parser.add_argument("zarr_path", type=Path, help="Path to Palette Zarr store.")
    parser.add_argument(
        "--rois",
        type=int,
        nargs="*",
        default=[0, 1, 2, 3, 4],
        help="ROI indices to inspect (default: first five).",
    )
    return parser.parse_args(argv)


def main(argv: Optional[list[str]] = None) -> int:
    args = parse_args(argv)
    return inspect_masks(args.zarr_path, args.rois)


if __name__ == "__main__":
    raise SystemExit(main())
