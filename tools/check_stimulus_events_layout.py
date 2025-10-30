#!/usr/bin/env python3
"""
Utility to report how stimulus events are stored inside a Palette-style Zarr
archive.  Stimulus events may be written either as a group of column arrays
or as a single structured array.  This script prints which layout is present
so the C++ loader (or the user) can confirm which version they need to handle.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Iterable, Tuple

import zarr


def _detect_latest_run(group: zarr.hierarchy.Group) -> Tuple[str | None, str | None]:
    """Return (run_name, attr_name) for the group's latest pointer, if present."""
    for attr in ("latest", "latest_completed", "latest_success", "latest_run"):
        value = group.attrs.get(attr)
        if isinstance(value, str) and value in group:
            return value, attr
    keys = sorted(group.keys())
    if keys:
        return keys[-1], None
    return None, None


def _describe_events_node(events: zarr.Array | zarr.hierarchy.Group) -> str:
    """Produce a human-readable description of the events container."""
    if isinstance(events, zarr.Array):
        dtype = events.dtype
        if dtype.names:
            fields = ", ".join(dtype.names)
            return f"structured array (fields: {fields})"
        return f"plain array dtype={dtype}"

    array_keys: Iterable[str] = getattr(events, "array_keys", lambda: events.keys())()
    keys = sorted(array_keys)
    key_text = ", ".join(keys) if keys else "(no child arrays)"
    return f"column arrays (dataset keys: {key_text})"


def inspect_events(store_path: Path, run_name: str | None) -> int:
    if not store_path.exists():
        print(f"ERROR: {store_path} does not exist", file=sys.stderr)
        return 2

    try:
        root = zarr.open(str(store_path), mode="r")
    except Exception as exc:
        print(f"ERROR: failed to open {store_path}: {exc}", file=sys.stderr)
        return 3

    if "analysis" not in root or "stimulus_runs" not in root["analysis"]:
        print(
            "ERROR: archive does not contain analysis/stimulus_runs/",
            file=sys.stderr,
        )
        return 4

    runs_group = root["analysis"]["stimulus_runs"]
    run_names = list(runs_group.keys())
    print(f"Found {len(run_names)} stimulus run(s).")

    chosen_run = run_name
    chosen_attr: str | None = None

    if chosen_run:
        if chosen_run not in run_names:
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
                "ERROR: could not determine a stimulus run to inspect and "
                "none was provided.",
                file=sys.stderr,
            )
            return 6

    run_group = runs_group[chosen_run]
    print(f"Inspecting run: {chosen_run}")
    if chosen_attr:
        print(f"  (selected via {chosen_attr} attribute)")

    if "events" not in run_group:
        print("  events/: NOT PRESENT")
        return 1

    events_node = run_group["events"]
    description = _describe_events_node(events_node)
    print(f"  events layout: {description}")

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Report how stimulus events are stored inside a Zarr archive."
    )
    parser.add_argument(
        "zarr_path",
        type=Path,
        help="Path to the root of the Zarr archive",
    )
    parser.add_argument(
        "--run",
        dest="run_name",
        help="Specific stimulus run to inspect (defaults to latest*)",
    )

    args = parser.parse_args()
    return inspect_events(args.zarr_path, args.run_name)


if __name__ == "__main__":
    sys.exit(main())
