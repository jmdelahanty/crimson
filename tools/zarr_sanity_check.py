#!/usr/bin/env python3
"""
Quick utility to inspect a Palette-style Zarr archive and highlight whether
the expected detection/interpolation runs are present.  Useful when the C++
loader fails with "Could not load supported zarr layouts".
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Iterable

import zarr


def _format_attrs(group: zarr.hierarchy.Group) -> str:
    if not hasattr(group, "attrs"):
        return ""
    try:
        attrs_dict = dict(group.attrs)
    except Exception as exc:  # pragma: no cover - diagnostic path
        return f"    !! failed to read attrs: {exc}"
    if not attrs_dict:
        return "    (no attrs)"
    serialized = json.dumps(attrs_dict, indent=2, sort_keys=True)
    return "    attrs: " + " ".join(serialized.split())


def _summarize_run_group(
    store: zarr.hierarchy.Group,
    group_name: str,
    *,
    required_arrays: dict[str, tuple[str, ...]] | None = None,
) -> tuple[bool, bool]:
    if group_name not in store:
        print(f"\n[{group_name}] group: NOT PRESENT")
        return False, False

    group = store[group_name]
    child_names = list(group.keys())
    latest = group.attrs.get("latest") or group.attrs.get("latest_completed") or group.attrs.get("latest_success")

    print(f"\n[{group_name}] group: {len(child_names)} runs")
    print(_format_attrs(group))
    if child_names:
        print(f"    runs: {child_names}")
    if latest:
        ok = True
        print(f"    latest pointer: {latest}")
        if latest in group:
            run = group[latest]
            print(f"    -- inspecting {latest}/")
            print(_format_attrs(run))
            for key in run.keys():
                item = run[key]
                if isinstance(item, zarr.Array):
                    print(
                        f"      {key}: Array shape={item.shape} dtype={item.dtype} chunks={item.chunks}"
                    )
                else:
                    print(f"      {key}/ (Group)")
            if required_arrays:
                missing_entries: list[str] = []
                for label, alternatives in required_arrays.items():
                    if not any(name in run for name in alternatives):
                        alt_names = ", ".join(alternatives)
                        missing_entries.append(f"{label} (expected one of: {alt_names})")
                if missing_entries:
                    ok = False
                    print("    !! missing required arrays:")
                    for entry in missing_entries:
                        print(f"       - {entry}")
        else:
            ok = False
            print(f"    !! latest run '{latest}' not present in group")
    else:
        print("    !! no 'latest' attribute found")
        ok = False

    return True, ok


def _walk_tree(group: zarr.hierarchy.Group, max_depth: int, prefix: str = "") -> None:
    if max_depth < 0:
        return
    keys: Iterable[str] = sorted(group.keys())
    for name in keys:
        item = group[name]
        if isinstance(item, zarr.Array):
            print(
                f"{prefix}{name}: Array shape={item.shape} dtype={item.dtype} chunks={item.chunks}"
            )
        else:
            print(f"{prefix}{name}/ (Group)")
            if max_depth > 0:
                _walk_tree(item, max_depth - 1, prefix + "  ")


def inspect_store(path: Path, depth: int) -> int:
    if not path.exists():
        print(f"ERROR: {path} does not exist", file=sys.stderr)
        return 2

    try:
        store = zarr.open(str(path), mode="r")
    except Exception as exc:
        print(f"ERROR: failed to open {path}: {exc}", file=sys.stderr)
        return 3

    print("=" * 72)
    print(f"Inspecting Zarr archive: {path}")
    print("=" * 72)

    # Root summary
    print("\n[Root]")
    print(_format_attrs(store))
    if depth > 0:
        print(f"\nTree (depth {depth}):")
        _walk_tree(store, depth)

    status = 0

    # Palette detection layout
    detection_requirements = {
        "bounding boxes": ("bboxes", "bbox_norm_coords"),
        "per-frame counts": ("n_detections", "frame_counts"),
        "frame indices": ("frame_indices",),
    }

    present_detect, ok_detect = _summarize_run_group(
        store, "detect_runs", required_arrays=detection_requirements
    )
    if not ok_detect:
        print(
            "\nERROR: No usable detection run found. Archive must provide detect_runs/ "
            "with bbox_norm_coords (or bboxes) and frame_indices/n_detections."
        )
        status = 1

    if "refined_detect_runs" in store:
        refined_group = store["refined_detect_runs"]
        print("\n[refined_detect_runs]")
        print(_format_attrs(refined_group))
        run_names = list(refined_group.keys())
        print(f"    runs: {run_names}")
        latest_refined = refined_group.attrs.get("latest") or refined_group.attrs.get("latest_completed")
        refined_ok = False
        if latest_refined and latest_refined in refined_group:
            run = refined_group[latest_refined]
            print(f"    inspecting {latest_refined}/")
            print(_format_attrs(run))
            # Inspect known subgroups (interpolated results live here)
            subgroup_labels = [
                ("interpolated", run.get("interpolated", None)),
                ("filtered", run.get("filtered", None)),
                ("root", run),
            ]
            for label, node in subgroup_labels:
                if node is None:
                    continue
                print(f"      subgroup '{label}':")
                if isinstance(node, zarr.Array):
                    # Should not happen, but keep output tidy
                    print("        (unexpected array)")
                    continue
                keys = list(node.keys())
                if keys:
                    print(f"        keys: {keys}")
                has_boxes = any(name in node for name in ("bboxes", "bbox_norm_coords"))
                has_counts = any(name in node for name in ("n_detections", "frame_counts", "frame_indices"))
                if has_boxes and has_counts:
                    refined_ok = True
                    # Provide shape/dtype information for the key sets we care about
                    for dataset_name in ("bbox_norm_coords", "bboxes", "frame_indices", "n_detections", "frame_counts", "detection_source"):
                        if dataset_name in node:
                            arr = node[dataset_name]
                            if isinstance(arr, zarr.Array):
                                print(
                                    f"        {dataset_name}: shape={arr.shape} dtype={arr.dtype} chunks={arr.chunks}"
                                )
                else:
                    print("        !! missing boxes/counts in this subgroup")
        else:
            print("    !! latest refined detect run not found or missing 'latest' attribute")
        if not refined_ok:
            print(
                "    WARNING: No refined subgroup contained both detections and frame counts. "
                "Refined detections may be unavailable."
            )
    else:
        print("\n[refined_detect_runs] group: NOT PRESENT")

    if "analysis" in store and "stimulus_runs" in store["analysis"]:
        stim_group = store["analysis"]["stimulus_runs"]
        print("\n[analysis/stimulus_runs]")
        print(_format_attrs(stim_group))
        child_names = list(stim_group.keys())
        print(f"    runs: {child_names}")
        latest = stim_group.attrs.get("latest") or stim_group.attrs.get("latest_completed")
        if latest and latest in stim_group:
            run = stim_group[latest]
            print(f"    inspecting {latest}/")
            if "interpolation_mask" in run:
                mask = run["interpolation_mask"]
                print(
                    f"      interpolation_mask: shape={mask.shape} dtype={mask.dtype} chunks={mask.chunks}"
                )
            if "frame_alignment" in run:
                align = run["frame_alignment"]
                print("      frame_alignment/")
                for key in align.keys():
                    arr = align[key]
                    if isinstance(arr, zarr.Array):
                        print(
                            f"        {key}: shape={arr.shape} dtype={arr.dtype} chunks={arr.chunks}"
                        )
                    else:
                        print(f"        {key}/ (Group)")
    else:
        print("\n[analysis/stimulus_runs] group: NOT PRESENT")

    print("\nInspection complete.")
    return status


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Inspect Palette Zarr archives for debugging the detection layout."
    )
    parser.add_argument("zarr_path", help="Path to the .zarr directory")
    parser.add_argument(
        "--depth",
        type=int,
        default=1,
        help="How deep to recurse when printing the group tree (default: 1)",
    )
    args = parser.parse_args(argv)

    return inspect_store(Path(args.zarr_path), args.depth)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
