#!/usr/bin/env python3
"""
Verify that the dense chaser interpolation dataset contains an entry for every
camera frame mapped by frame_alignment/camera_to_stimulus_frame_corrected.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Tuple

import numpy as np
import zarr


def _detect_latest_run(group: zarr.hierarchy.Group) -> Tuple[str | None, str | None]:
    for attr in ("latest", "latest_completed", "latest_success", "latest_run"):
        value = group.attrs.get(attr)
        if isinstance(value, str) and value in group:
            return value, attr
    keys = sorted(group.keys())
    if keys:
        return keys[-1], None
    return None, None


def _load_array(group: zarr.hierarchy.Group, path: str) -> np.ndarray:
    node = group
    for part in path.split("/"):
        if part not in node:
            raise KeyError(f"Missing path component '{part}' in '{path}'")
        node = node[part]
    if not isinstance(node, zarr.Array):
        raise TypeError(f"'{path}' is not a Zarr array")
    return node[:]


def _load_column(group: zarr.hierarchy.Group, candidates: tuple[str, ...]) -> np.ndarray:
    for name in candidates:
        if name in group:
            node = group[name]
            if isinstance(node, zarr.Array):
                return node[:]
    raise KeyError(f"None of the columns {candidates} present under {group.path}")


def analyze(path: Path, run_name: str | None) -> int:
    try:
        root = zarr.open(str(path), mode="r")
    except Exception as exc:  # pragma: no cover - diagnostic for CLI use
        print(f"ERROR: failed to open {path}: {exc}", file=sys.stderr)
        return 2

    if "analysis" not in root or "stimulus_runs" not in root["analysis"]:
        print("ERROR: archive does not contain analysis/stimulus_runs/", file=sys.stderr)
        return 3

    runs_group: zarr.hierarchy.Group = root["analysis"]["stimulus_runs"]
    chosen_attr = None
    if run_name:
        if run_name not in runs_group:
            print(
                f"ERROR: stimulus run '{run_name}' not found. Available runs: {sorted(runs_group.keys())}",
                file=sys.stderr,
            )
            return 4
        chosen_run = run_name
    else:
        chosen_run, chosen_attr = _detect_latest_run(runs_group)
        if not chosen_run:
            print("ERROR: unable to determine stimulus run (no --run and no latest attr)", file=sys.stderr)
            return 5

    print(f"Using stimulus run: {chosen_run}")
    if chosen_attr:
        print(f"  (selected via {chosen_attr} attribute)")

    run_group = runs_group[chosen_run]

    try:
        stim_lookup = _load_array(run_group, "frame_alignment/camera_to_stimulus_frame_corrected")
    except Exception as exc:
        print(f"ERROR: failed to read camera_to_stimulus_frame_corrected: {exc}", file=sys.stderr)
        return 6

    camera_count = stim_lookup.shape[0]
    valid_mask = stim_lookup >= 0
    valid_cameras = np.nonzero(valid_mask)[0]
    print(f"Camera frames in corrected timeline: total={camera_count}, mapped={valid_cameras.size}")

    tracking_base = run_group.get("tracking_data")
    if tracking_base is None or "chaser_states_interpolated" not in tracking_base:
        print("ERROR: tracking_data/chaser_states_interpolated group missing", file=sys.stderr)
        return 7

    interp_group: zarr.hierarchy.Group = tracking_base["chaser_states_interpolated"]

    try:
        camera_frames = _load_column(interp_group, ("camera_frame_id", "payload_frame_id"))
    except KeyError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 8

    if camera_frames.ndim != 1:
        print("ERROR: camera_frame_id column is not 1D", file=sys.stderr)
        return 9

    camera_frames = camera_frames.astype(np.int64)
    seen_mask = np.zeros(camera_count, dtype=bool)
    valid_camera_frames = camera_frames[camera_frames >= 0]
    valid_camera_frames = valid_camera_frames[valid_camera_frames < camera_count]
    seen_mask[valid_camera_frames] = True

    missing_mask = valid_mask & (~seen_mask)
    missing_indices = np.nonzero(missing_mask)[0]

    print(f"Interpolated chaser rows: {camera_frames.size}")
    print(f"Camera frames covered: {seen_mask.sum()} (expected {valid_cameras.size})")

    if missing_indices.size == 0:
        print("All mapped camera frames have at least one interpolated chaser state.")
    else:
        print(f"MISSING {missing_indices.size} camera frames with no interpolated chaser state!")
        sample = missing_indices[:20]
        print("  Sample missing frames:", ", ".join(str(int(x)) for x in sample))

    return 0 if missing_indices.size == 0 else 10


def main() -> int:  # pragma: no cover - CLI entry
    parser = argparse.ArgumentParser(description="Check chaser_states_interpolated coverage per camera frame.")
    parser.add_argument("zarr_path", type=Path, help="Path to the Zarr archive root.")
    parser.add_argument("--run", dest="run_name", help="Stimulus run to inspect (defaults to latest*).")
    args = parser.parse_args()
    return analyze(args.zarr_path, args.run_name)


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
