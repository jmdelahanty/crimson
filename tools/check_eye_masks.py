#!/usr/bin/env python3
"""
Quick diagnostic script for Palette Zarr eye-mask data.

Usage:
    python tools/check_eye_masks.py /path/to/session.zarr

Reports:
    - Availability of keypoints runs used for headings
    - Presence of refined_eye_masks_runs and the latest run name
    - Dataset shapes and basic sanity checks (ROI count alignment)
"""

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Optional

try:
    import zarr
except ImportError:
    print("[ERROR] zarr is not installed in the current environment.", file=sys.stderr)
    raise


def find_latest_run(group: Any, subgroup: str) -> Optional[str]:
    """Replicates the 'latest' attr convention used in Palette runs."""
    if subgroup not in group:
        return None
    root = group[subgroup]
    latest = root.attrs.get("latest")
    if latest:
        return latest
    runs = sorted([name for name in root.group_keys()])
    return runs[-1] if runs else None


def load_json_attr(attrs, key):
    value = attrs.get(key)
    if value is None:
        return None
    if isinstance(value, str):
        try:
            return json.loads(value)
        except json.JSONDecodeError:
            return value
    return value


def check_eye_masks(zarr_path: Path) -> int:
    if not zarr_path.exists():
        print(f"[ERROR] Path not found: {zarr_path}", file=sys.stderr)
        return 2

    root = zarr.open(zarr_path, mode="r")
    print(f"[INFO] Opened Zarr store: {zarr_path}")

    kp_latest = find_latest_run(root, "keypoints_runs")
    if kp_latest is None:
        print("[WARN] No keypoints_runs subgroup found.")
    else:
        kp_group = root["keypoints_runs"][kp_latest]
        print(f"[INFO] Latest keypoints run: {kp_latest}")
        for name in ("frame_indices", "heading", "detection_success"):
            print(f"    {name:20s}: {'present' if name in kp_group else 'MISSING'}")
        labels = load_json_attr(kp_group.attrs, "keypoint_labels")
        if labels:
            print(f"    keypoint_labels: {labels}")

    eye_latest = find_latest_run(root, "refined_eye_masks_runs")
    if eye_latest is None:
        print("[WARN] No refined_eye_masks_runs subgroup found.")
        return 1

    eye_group = root["refined_eye_masks_runs"][eye_latest]
    print(f"[INFO] Latest refined eye-mask run: {eye_latest}")
    for name in ("frame_indices", "masks_roi", "mask_probs_roi_refined", "ellipse_params"):
        status = "present" if name in eye_group else "missing"
        print(f"    {name:20s}: {status}")

    if "frame_indices" not in eye_group or "masks_roi" not in eye_group:
        print("[ERROR] Required datasets missing; cannot continue.")
        return 1

    frame_indices = eye_group["frame_indices"][:]
    masks = eye_group["masks_roi"]
    print(f"[INFO] frame_indices shape: {frame_indices.shape}")
    print(f"[INFO] masks_roi shape:     {masks.shape}")

    if masks.ndim != 4 or masks.shape[1] < 2:
        print("[ERROR] Unexpected masks_roi dimensionality.")
        return 1

    if masks.shape[0] != frame_indices.shape[0]:
        print("[ERROR] ROI count mismatch between masks and frame_indices.")
    else:
        print("[INFO] ROI counts align between masks and frame_indices.")

    # Basic non-zero check
    nonzero = (masks[:10] > 0).sum()
    print(f"[INFO] Non-zero mask samples (first 10 ROIs): {nonzero}")

    # Optional: show provenance attr
    provenance = load_json_attr(eye_group.attrs, "provenance")
    if provenance:
        print(f"[INFO] provenance keys: {list(provenance.keys())}")

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Inspect Palette Zarr eye-mask data.")
    parser.add_argument(
        "zarr_path",
        type=Path,
        help="Path to the Palette Zarr store (directory ending in .zarr/.zr3)",
    )
    args = parser.parse_args()
    return check_eye_masks(args.zarr_path)


if __name__ == "__main__":
    raise SystemExit(main())
