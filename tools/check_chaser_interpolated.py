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

    camera_to_stimulus: np.ndarray | None = None
    mapping_variant = "corrected"
    try:
        direct = _load_array(run_group, "frame_alignment/camera_to_stimulus_frame_corrected")
        camera_to_stimulus = direct.astype(np.int64, copy=False)
    except KeyError:
        camera_to_stimulus = None
    except Exception as exc:
        print(f"WARNING: failed to read camera_to_stimulus_frame_corrected: {exc}", file=sys.stderr)
        camera_to_stimulus = None

    if camera_to_stimulus is None:
        mapping_variant = "legacy"
        try:
            cam_to_meta = _load_array(run_group, "frame_alignment/camera_to_metadata_index").astype(np.int64, copy=False)
            stim_frame_meta = _load_array(run_group, "video_metadata/frame_metadata/stimulus_frame_num").astype(np.int64, copy=False)
        except Exception as exc:
            print("ERROR: could not load corrected or legacy camera→stimulus mapping:", file=sys.stderr)
            print(f"  {exc}", file=sys.stderr)
            return 6

        camera_to_stimulus = np.full(cam_to_meta.shape[0], -1, dtype=np.int64)
        for cam_idx, meta_idx in enumerate(cam_to_meta):
            if 0 <= meta_idx < stim_frame_meta.shape[0]:
                camera_to_stimulus[cam_idx] = int(stim_frame_meta[int(meta_idx)])

    camera_count = camera_to_stimulus.shape[0]
    valid_mask = camera_to_stimulus >= 0
    valid_count = int(valid_mask.sum())
    print(
        f"Camera frames ({mapping_variant}): total={camera_count}, mapped={valid_count}"
    )

    tracking_base = run_group.get("tracking_data")
    dataset_group = None
    dataset_label = ""
    if tracking_base is not None and "chaser_states_interpolated" in tracking_base:
        dataset_group = tracking_base["chaser_states_interpolated"]
        dataset_label = "chaser_states_interpolated"
    elif tracking_base is not None and "chaser_states" in tracking_base:
        dataset_group = tracking_base["chaser_states"]
        dataset_label = "chaser_states"
    else:
        print("ERROR: no chaser state datasets found under tracking_data/", file=sys.stderr)
        return 7

    try:
        stimulus_frames = _load_column(dataset_group, ("stimulus_frame_num",))
    except KeyError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 8

    if stimulus_frames.ndim != 1:
        print("ERROR: stimulus_frame_num column is not 1D", file=sys.stderr)
        return 9

    stimulus_frames = stimulus_frames.astype(np.int64, copy=False)
    valid_stimulus = stimulus_frames[stimulus_frames >= 0]
    if valid_stimulus.size == 0:
        print("ERROR: dataset contains no valid stimulus_frame_num entries", file=sys.stderr)
        return 10

    max_camera_stim = int(camera_to_stimulus[valid_mask].max()) if valid_count else -1
    max_needed = int(max(valid_stimulus.max(), max_camera_stim))
    presence = np.zeros(max_needed + 1, dtype=bool)
    presence[valid_stimulus] = True

    safe_indices = camera_to_stimulus.copy()
    safe_indices[~valid_mask] = -1
    safe_indices[safe_indices > max_needed] = -1

    covered_mask = np.zeros_like(valid_mask)
    valid_indices = safe_indices >= 0
    covered_mask[valid_indices] = presence[safe_indices[valid_indices]]
    missing_mask = valid_mask & (~covered_mask)
    missing_indices = np.nonzero(missing_mask)[0]

    print(f"Dataset: {dataset_label} (rows={stimulus_frames.size})")
    print(f"Camera frames covered: {covered_mask.sum()} / {valid_count}")

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
