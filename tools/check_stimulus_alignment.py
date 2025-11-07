#!/usr/bin/env python3
"""
Inspect the camera->stimulus alignment inside a Palette Zarr archive.

Reports how either the legacy mapping
(`analysis/stimulus_runs/<run>/frame_alignment/camera_to_metadata_index`) or the
corrected mapping (`.../camera_to_metadata_index_corrected`) translates into
stimulus frames so we can verify the effective playback ratio (stimulus frame
delta per camera frame delta).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Tuple

import numpy as np
import zarr


def _detect_latest_run(group: zarr.hierarchy.Group) -> Tuple[str | None, str | None]:
    """Return (run_name, attr) using the same heuristic as other tools."""
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


def _format_pairs(camera_frames: np.ndarray, stimulus_frames: np.ndarray, limit: int) -> str:
    lines = []
    for cam, stim in zip(camera_frames[:limit], stimulus_frames[:limit]):
        lines.append(f"  camera {cam:8d} -> stimulus {stim:8d}")
    if camera_frames.size > limit:
        lines.append(f"  ... ({camera_frames.size - limit} additional pairs)")
    return "\n".join(lines)


def _load_mapping_arrays(
    run_group: zarr.hierarchy.Group, camera_path: str, stim_path: str
) -> tuple[np.ndarray, np.ndarray]:
    camera = _load_array(run_group, camera_path)
    stim = _load_array(run_group, stim_path)
    return camera, stim


def analyze_alignment(
    store_path: Path,
    run_name: str | None,
    sample: int,
    mapping_mode: str,
) -> int:
    if not store_path.exists():
        print(f"ERROR: {store_path} does not exist", file=sys.stderr)
        return 2

    try:
        root = zarr.open(str(store_path), mode="r")
    except Exception as exc:
        print(f"ERROR: failed to open {store_path}: {exc}", file=sys.stderr)
        return 3

    if "analysis" not in root or "stimulus_runs" not in root["analysis"]:
        print("ERROR: archive does not contain analysis/stimulus_runs/", file=sys.stderr)
        return 4

    runs_group = root["analysis"]["stimulus_runs"]
    run_names = sorted(runs_group.keys())
    print(f"Stimulus runs available: {run_names}")

    chosen_attr = None
    chosen_run = run_name
    if chosen_run:
        if chosen_run not in runs_group:
            print(
                f"ERROR: stimulus run '{chosen_run}' not present. "
                f"Available runs: {run_names}",
                file=sys.stderr,
            )
            return 5
    else:
        chosen_run, chosen_attr = _detect_latest_run(runs_group)
        if not chosen_run:
            print(
                "ERROR: could not determine a stimulus run (no --run provided and "
                "no latest* attribute present).",
                file=sys.stderr,
            )
            return 6

    run_group = runs_group[chosen_run]
    print(f"Using stimulus run: {chosen_run}")
    if chosen_attr:
        print(f"  (selected via {chosen_attr} attribute)")

    mapping_used = "legacy"
    camera_to_metadata = stimulus_frame_nums = None

    def try_corrected() -> bool:
        nonlocal camera_to_metadata, stimulus_frame_nums, mapping_used
        try:
            camera_to_metadata, stimulus_frame_nums = _load_mapping_arrays(
                run_group,
                "frame_alignment/camera_to_metadata_index_corrected",
                "video_metadata/frame_metadata/stimulus_frame_num_corrected",
            )
        except (KeyError, TypeError):
            return False
        else:
            mapping_used = "corrected"
            return True

    if mapping_mode in {"auto", "corrected"}:
        if not try_corrected():
            if mapping_mode == "corrected":
                print(
                    "ERROR: corrected mapping arrays are missing from this run.",
                    file=sys.stderr,
                )
                return 7

    if camera_to_metadata is None or stimulus_frame_nums is None:
        try:
            camera_to_metadata, stimulus_frame_nums = _load_mapping_arrays(
                run_group,
                "frame_alignment/camera_to_metadata_index",
                "video_metadata/frame_metadata/stimulus_frame_num",
            )
        except (KeyError, TypeError) as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 7
        mapping_used = "legacy"

    print(f"Mapping variant: {mapping_used}")

    if camera_to_metadata.ndim != 1:
        print("ERROR: camera_to_metadata_index is not 1D", file=sys.stderr)
        return 8
    if stimulus_frame_nums.ndim != 1:
        print("ERROR: stimulus_frame_num is not 1D", file=sys.stderr)
        return 9

    camera_frames: list[int] = []
    stimulus_frames: list[int] = []
    skipped_out_of_range = 0
    for cam_idx, meta_idx in enumerate(camera_to_metadata):
        if meta_idx < 0:
            continue
        if meta_idx >= stimulus_frame_nums.shape[0]:
            skipped_out_of_range += 1
            continue
        camera_frames.append(int(cam_idx))
        stimulus_frames.append(int(stimulus_frame_nums[int(meta_idx)]))

    if not camera_frames:
        print("ERROR: no valid camera/stimulus pairs found.", file=sys.stderr)
        return 10

    camera_frames_np = np.asarray(camera_frames, dtype=np.int64)
    stimulus_frames_np = np.asarray(stimulus_frames, dtype=np.int64)

    print(f"Total mapped camera frames: {camera_frames_np.size}")
    if skipped_out_of_range:
        print(f"  (Skipped {skipped_out_of_range} metadata indices outside stimulus_frame_num)")

    diffs_cam = np.diff(camera_frames_np)
    diffs_stim = np.diff(stimulus_frames_np)

    valid = diffs_cam != 0
    if not np.any(valid):
        print("ERROR: no deltas with non-zero camera step found.", file=sys.stderr)
        return 11

    ratios = diffs_stim[valid] / diffs_cam[valid]
    print("Stimulus delta / Camera delta ratio statistics:")
    print(f"  min:   {ratios.min():.4f}")
    print(f"  max:   {ratios.max():.4f}")
    print(f"  mean:  {ratios.mean():.4f}")
    print(f"  median:{np.median(ratios):.4f}")

    zero_steps = np.count_nonzero(diffs_stim == 0)
    backwards = np.count_nonzero(diffs_stim < 0)
    if zero_steps or backwards:
        print(f"  zero stimulus deltas: {zero_steps}")
        print(f"  negative stimulus deltas: {backwards}")

    print(f"\nFirst {sample} mapped frames:")
    print(_format_pairs(camera_frames_np, stimulus_frames_np, sample))

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check camera -> stimulus frame alignment ratios inside a Palette Zarr archive.",
    )
    parser.add_argument("zarr_path", type=Path, help="Path to the Zarr archive root.")
    parser.add_argument("--run", dest="run_name", help="Stimulus run to inspect (defaults to latest*).")
    parser.add_argument(
        "--sample",
        type=int,
        default=20,
        help="How many camera/stimulus pairs to print (default: 20).",
    )
    parser.add_argument(
        "--mapping",
        choices=("auto", "legacy", "corrected"),
        default="auto",
        help="Which mapping to inspect (default: auto, prefer corrected when available).",
    )

    args = parser.parse_args()
    return analyze_alignment(args.zarr_path, args.run_name, args.sample, args.mapping)


if __name__ == "__main__":
    sys.exit(main())
