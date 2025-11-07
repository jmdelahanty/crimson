#!/usr/bin/env python3
"""
Report the raw-video and stimulus-alignment sources used to synchronize playback
inside a Palette Zarr archive.

Given a Zarr root, the script prints:
  • The raw video metadata (source path, fps, dimensions) plus the array(s) we
    would read frames from.
  • The stimulus run selected for alignment together with the paths holding the
    camera→metadata mapping and the stimulus-frame numbers.
Use this to confirm exactly which zarr nodes were consulted when the UI
successfully synchronized raw and stimulus videos.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Optional, Tuple

import numpy as np
import zarr

try:
    from zarr.hierarchy import Group as ZarrGroup  # type: ignore
except Exception:  # pragma: no cover - compatibility
    from zarr import Group as ZarrGroup  # type: ignore

try:
    from zarr.core import Array as ZarrArray  # type: ignore
except Exception:  # pragma: no cover
    from zarr import Array as ZarrArray  # type: ignore


def _detect_latest_run(group: ZarrGroup) -> Tuple[Optional[str], Optional[str]]:
    """Mirror the loader heuristic that prefers explicit latest* attrs."""
    for attr in ("latest", "latest_completed", "latest_success", "latest_run"):
        value = group.attrs.get(attr)
        if isinstance(value, str) and value in group:
            return value, attr
    keys = sorted(group.keys())
    if keys:
        return keys[-1], None
    return None, None


def _resolve_array(root: ZarrGroup, path: str) -> Optional[ZarrArray]:
    node: ZarrGroup | ZarrArray = root
    for part in path.split("/"):
        if not isinstance(node, ZarrGroup) or part not in node:
            return None
        node = node[part]
    return node if isinstance(node, ZarrArray) else None


def _fmt_shape(shape: Tuple[int, ...]) -> str:
    return "[" + ", ".join(str(dim) for dim in shape) + "]"


def _print_raw_video_summary(root: zarr.hierarchy.Group) -> None:
    print("\nRaw video source")
    if "raw_video" not in root:
        print("  raw_video group not present.")
        return
    group = root["raw_video"]
    attrs = group.attrs
    source = attrs.get("source_path") or attrs.get("source_video") or "<unknown>"
    fps = attrs.get("fps") or attrs.get("frame_rate")
    total_frames = attrs.get("total_frames")
    width = attrs.get("video_width")
    height = attrs.get("video_height")
    print(f"  source_path: {source}")
    if fps:
        print(f"  fps: {fps}")
    if total_frames:
        print(f"  total_frames: {total_frames}")
    if width and height:
        print(f"  resolution: {width}x{height}")

    for candidate in ("raw_video/images_full", "raw_video/images_ds"):
        array = _resolve_array(root, candidate)
        if array is None:
            continue
        print(f"  frames array: {candidate} dtype={array.dtype} shape={_fmt_shape(array.shape)}")
        break
    else:
        print("  frames array: <not found>")


def _print_stimulus_alignment(
    root: zarr.hierarchy.Group,
    explicit_run: Optional[str],
) -> None:
    print("\nStimulus alignment source")
    if "analysis" not in root or "stimulus_runs" not in root["analysis"]:
        print("  analysis/stimulus_runs/ missing.")
        return

    runs_group = root["analysis"]["stimulus_runs"]
    if explicit_run:
        run_name = explicit_run
        chosen_attr = None
        if run_name not in runs_group:
            print(f"  Requested run '{run_name}' not present. Available: {sorted(runs_group.keys())}")
            return
    else:
        run_name, chosen_attr = _detect_latest_run(runs_group)
        if not run_name:
            print("  Could not determine a stimulus run (no latest* attribute and none specified).")
            return

    run_group = runs_group[run_name]
    print(f"  run: analysis/stimulus_runs/{run_name}/")
    if chosen_attr:
        print(f"    (selected via '{chosen_attr}' attribute)")

    attrs = run_group.attrs
    created = attrs.get("created_at") or attrs.get("created_at_utc")
    if created:
        print(f"  created_at: {created}")
    if "coordinate_transform" in attrs:
        print("  coordinate_transform attr present")

    frame_align_path = "frame_alignment/camera_to_metadata_index"
    meta_stim_path = "video_metadata/frame_metadata/stimulus_frame_num"
    mapping_array = _resolve_array(run_group, frame_align_path)
    stim_array = _resolve_array(run_group, meta_stim_path)

    if mapping_array:
        print(
            f"  camera_to_metadata_index: {frame_align_path} "
            f"dtype={mapping_array.dtype} shape={_fmt_shape(mapping_array.shape)}"
        )
        valid = np.count_nonzero(mapping_array[:] >= 0)
        print(f"    valid camera frames mapped: {valid}")
    else:
        print(f"  camera_to_metadata_index array missing at {frame_align_path}")

    if stim_array:
        print(
            f"  stimulus_frame_num: {meta_stim_path} "
            f"dtype={stim_array.dtype} shape={_fmt_shape(stim_array.shape)}"
        )
    else:
        print(f"  stimulus_frame_num array missing at {meta_stim_path}")

    events_path = "events/stimulus_frame_num"
    if _resolve_array(run_group, events_path):
        print("  events metadata: analysis/stimulus_runs/"
              f"{run_name}/{events_path}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Show which Zarr paths feed raw/stimulus video synchronization."
    )
    parser.add_argument("zarr_path", type=Path, help="Path to the Zarr archive root.")
    parser.add_argument(
        "--stimulus-run",
        help="Stimulus run name (default: use latest*/filesystem heuristic).",
    )
    args = parser.parse_args()

    store_path = args.zarr_path
    if not store_path.exists():
        print(f"ERROR: {store_path} does not exist.", file=sys.stderr)
        return 2

    try:
        root = zarr.open(str(store_path), mode="r")
    except Exception as exc:  # pragma: no cover - informative path
        print(f"ERROR: failed to open {store_path}: {exc}", file=sys.stderr)
        return 3

    print(f"Archive: {store_path}")
    _print_raw_video_summary(root)
    _print_stimulus_alignment(root, args.stimulus_run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
