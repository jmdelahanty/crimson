#!/usr/bin/env python3
"""Create ordinary and sharded scratch Zarrs for Crimson read qualification."""

from __future__ import annotations

import argparse
import json
import math
import shutil
import socket
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import numpy as np
import zarr

from benchmark_detection_run_sharding import ARRAY_NAMES, load_source_bundle


KEYPOINT_RUN = "refined_keypoints_sleepyfish_kp_allclips_20260708_01"
KEYPOINT_PARENT = "refined_keypoints_runs"
DETECT_RUN = "detect_qualification"
REFINED_DETECT_RUN = "refined_detect_qualification"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build equivalent ordinary, 32K-sharded, and 131K-sharded "
            "minimal analysis Zarrs from actual finalized refined data."
        )
    )
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    parser.add_argument(
        "--layouts",
        default="ordinary,32768,131072",
        help="Comma-separated outer shard row counts plus optional ordinary.",
    )
    parser.add_argument("--inner-chunk-rows", type=int, default=1024)
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Replace an existing output root.",
    )
    return parser.parse_args()


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def read_attributes(group_path: Path) -> dict[str, Any]:
    metadata = json.loads((group_path / "zarr.json").read_text())
    attributes = metadata.get("attributes", {})
    return attributes if isinstance(attributes, dict) else {}


def parse_layouts(spec: str, inner_rows: int) -> list[tuple[str, int | None]]:
    layouts: list[tuple[str, int | None]] = []
    seen: set[int | None] = set()
    for raw in spec.split(","):
        token = raw.strip().lower()
        if not token:
            continue
        if token == "ordinary":
            shard_rows = None
            label = "ordinary"
        else:
            shard_rows = int(token)
            if shard_rows <= 0 or shard_rows % inner_rows != 0:
                raise ValueError(
                    f"shard rows {shard_rows} must be a positive multiple of "
                    f"inner chunk rows {inner_rows}"
                )
            label = f"shard_{shard_rows}"
        if shard_rows in seen:
            continue
        seen.add(shard_rows)
        layouts.append((label, shard_rows))
    if not layouts:
        raise ValueError("No layouts requested")
    return layouts


def create_group(root: zarr.Group, path: str, attrs: dict[str, Any]) -> zarr.Group:
    group = root.require_group(path)
    if attrs:
        group.attrs.update(attrs)
    return group


def array_create_kwargs(
    source: zarr.Array,
    shard_rows: int | None,
    inner_rows: int,
) -> dict[str, Any]:
    trailing = tuple(int(value) for value in source.shape[1:])
    row_chunk = min(inner_rows, int(source.shape[0]))
    kwargs: dict[str, Any] = {
        "shape": source.shape,
        "dtype": source.dtype,
        "chunks": (row_chunk, *trailing),
        "serializer": source.serializer,
        "compressors": source.compressors,
        "filters": source.filters,
        "fill_value": source.fill_value,
        "attributes": dict(source.attrs),
        "overwrite": True,
    }
    if shard_rows is not None and source.shape[0] > row_chunk:
        kwargs["shards"] = (shard_rows, *trailing)
    return kwargs


def create_numeric_array(
    group: zarr.Group,
    name: str,
    values: np.ndarray,
    shard_rows: int | None,
    inner_rows: int,
) -> zarr.Array:
    trailing = tuple(int(value) for value in values.shape[1:])
    row_chunk = min(inner_rows, int(values.shape[0]))
    kwargs: dict[str, Any] = {
        "name": name,
        "shape": values.shape,
        "dtype": values.dtype,
        "chunks": (row_chunk, *trailing),
        "overwrite": True,
    }
    if shard_rows is not None and values.shape[0] > row_chunk:
        kwargs["shards"] = (shard_rows, *trailing)
    destination = group.create_array(**kwargs)
    destination[:] = values
    return destination


def prepare_fixture_roots(
    output_root: Path,
    layouts: list[tuple[str, int | None]],
    source_root_attrs: dict[str, Any],
) -> dict[str, zarr.Group]:
    roots: dict[str, zarr.Group] = {}
    source_video_metadata = source_root_attrs.get("source_video_metadata", {})
    root_attrs = {
        "fixture_kind": "crimson_refined_tabular_read_qualification",
        "fixture_created_at_utc": utc_now(),
        "fps": float(source_root_attrs.get("fps", 30.0)),
        "total_frames": int(source_root_attrs.get("total_frames", 0)),
        "source_path": str(source_root_attrs.get("source_path", "")),
        "source_video_path": str(source_root_attrs.get("source_video_path", "")),
        "source_video_metadata": source_video_metadata,
    }
    if isinstance(source_video_metadata, dict):
        root_attrs["image_width"] = int(source_video_metadata.get("width", 0))
        root_attrs["image_height"] = int(source_video_metadata.get("height", 0))
        # These are the operational names consumed by Crimson's metadata
        # loader. Keep image_width/image_height as fixture diagnostics too.
        root_attrs["video_width"] = root_attrs["image_width"]
        root_attrs["video_height"] = root_attrs["image_height"]

    for label, shard_rows in layouts:
        fixture_path = output_root / f"{label}.zarr"
        root = zarr.create_group(store=fixture_path, overwrite=True)
        attrs = dict(root_attrs)
        attrs["layout_label"] = label
        attrs["outer_shard_rows"] = shard_rows
        root.attrs.update(attrs)
        create_group(root, "raw_video", {
            "source_path": attrs["source_path"],
            "source_video_path": attrs["source_video_path"],
            "fps": attrs["fps"],
            "total_frames": attrs["total_frames"],
            "video_width": attrs.get("image_width", 0),
            "video_height": attrs.get("image_height", 0),
        })
        create_group(root, "detect_runs", {"latest": DETECT_RUN})
        create_group(root, f"detect_runs/{DETECT_RUN}", {
            "method": "qualification_base_from_refined_instances",
            "image_width": attrs.get("image_width", 0),
            "image_height": attrs.get("image_height", 0),
        })
        create_group(root, "refined_detect_runs", {"latest": REFINED_DETECT_RUN})
        create_group(root, f"refined_detect_runs/{REFINED_DETECT_RUN}", {
            "method": "qualification_materialized_refined_instances",
            "source_kind_code_map": {
                "none": 0,
                "raw_detect": 1,
                "interpolated": 2,
                "manual": 3,
            },
        })
        create_group(root, f"refined_detect_runs/{REFINED_DETECT_RUN}/instances", {})
        create_group(root, KEYPOINT_PARENT, {"latest": KEYPOINT_RUN})
        roots[label] = root
    return roots


def write_detection_bundle(
    roots: dict[str, zarr.Group],
    layouts: list[tuple[str, int | None]],
    data: dict[str, np.ndarray],
    inner_rows: int,
) -> None:
    base_mapping = {
        "frame_indices": data["frame_indices"],
        "bbox_norm_coords": data["bbox_norm_coords"],
        "scores": data["confidence_scores"],
        "class_ids": data["class_ids"],
        "frame_counts": data["frame_counts"],
        "n_detections": data["frame_counts"],
    }
    for label, shard_rows in layouts:
        print(f"[{label}] writing base detection arrays", flush=True)
        base = roots[label][f"detect_runs/{DETECT_RUN}"]
        for name, values in base_mapping.items():
            create_numeric_array(base, name, values, shard_rows, inner_rows)

        print(f"[{label}] writing refined detection instances", flush=True)
        instances = roots[label][
            f"refined_detect_runs/{REFINED_DETECT_RUN}/instances"
        ]
        for name in ARRAY_NAMES:
            create_numeric_array(instances, name, data[name], shard_rows, inner_rows)


def write_keypoint_and_crop_arrays(
    source_root: Path,
    roots: dict[str, zarr.Group],
    layouts: list[tuple[str, int | None]],
    inner_rows: int,
) -> list[str]:
    source_run_path = source_root / KEYPOINT_PARENT / KEYPOINT_RUN
    run_attrs = read_attributes(source_run_path)
    source_crop_run = str(run_attrs.get("source_crop_run", ""))
    if not source_crop_run:
        raise ValueError("Refined keypoint run has no source_crop_run")
    source_crop_path = source_root / "crop_runs" / source_crop_run
    crop_attrs = read_attributes(source_crop_path)

    for root in roots.values():
        create_group(root, f"{KEYPOINT_PARENT}/{KEYPOINT_RUN}", run_attrs)
        create_group(root, "crop_runs", {"latest": source_crop_run})
        create_group(root, f"crop_runs/{source_crop_run}", crop_attrs)

    array_names = sorted(
        path.name
        for path in source_run_path.iterdir()
        if path.is_dir() and (path / "zarr.json").is_file() and path.name != "reason"
    )
    for array_index, name in enumerate(array_names, start=1):
        source = zarr.open_array(store=source_run_path / name, mode="r")
        print(
            f"[keypoints {array_index}/{len(array_names)}] {name} "
            f"shape={source.shape} dtype={source.dtype}",
            flush=True,
        )
        destinations: dict[str, zarr.Array] = {}
        for label, shard_rows in layouts:
            group = roots[label][f"{KEYPOINT_PARENT}/{KEYPOINT_RUN}"]
            destinations[label] = group.create_array(
                name=name,
                **array_create_kwargs(source, shard_rows, inner_rows),
            )

        block_rows = max(
            [shard_rows or inner_rows for _, shard_rows in layouts] + [inner_rows]
        )
        for start in range(0, int(source.shape[0]), block_rows):
            end = min(int(source.shape[0]), start + block_rows)
            values = np.asarray(source[start:end])
            for destination in destinations.values():
                destination[start:end] = values

    crop_array_names = ["roi_coordinates_full", "frame_indices"]
    for name in crop_array_names:
        source = zarr.open_array(store=source_crop_path / name, mode="r")
        print(f"[crop] {name} shape={source.shape} dtype={source.dtype}", flush=True)
        destinations = {}
        for label, shard_rows in layouts:
            group = roots[label][f"crop_runs/{source_crop_run}"]
            destinations[label] = group.create_array(
                name=name,
                **array_create_kwargs(source, shard_rows, inner_rows),
            )
        block_rows = max(
            [shard_rows or inner_rows for _, shard_rows in layouts] + [inner_rows]
        )
        for start in range(0, int(source.shape[0]), block_rows):
            end = min(int(source.shape[0]), start + block_rows)
            values = np.asarray(source[start:end])
            for destination in destinations.values():
                destination[start:end] = values

    return array_names


def scan_files(path: Path) -> tuple[int, int]:
    files = [entry for entry in path.rglob("*") if entry.is_file()]
    return len(files), sum(entry.stat().st_size for entry in files)


def main() -> int:
    args = parse_args()
    source_root = args.source_root.resolve()
    output_root = args.output_root.resolve()
    layouts = parse_layouts(args.layouts, args.inner_chunk_rows)
    if output_root.exists():
        if not args.overwrite:
            raise FileExistsError(f"Output already exists: {output_root}")
        shutil.rmtree(output_root)
    output_root.mkdir(parents=True)

    started = time.perf_counter()
    source_root_attrs = read_attributes(source_root)
    roots = prepare_fixture_roots(output_root, layouts, source_root_attrs)

    instance_paths = sorted(
        source_root.glob(
            "clips/clip_*/cameras/*/refined_detect_runs/*/instances"
        )
    )
    if not instance_paths:
        raise FileNotFoundError("No per-clip refined detection instances found")
    print(f"Loading {len(instance_paths)} refined detection instance groups", flush=True)
    detection_data = load_source_bundle(instance_paths)
    print(
        f"Loaded detection bundle: {detection_data['bbox_norm_coords'].shape[0]} "
        f"detections, {sum(values.nbytes for values in detection_data.values()) / 1e6:.1f} MB raw",
        flush=True,
    )
    write_detection_bundle(
        roots, layouts, detection_data, args.inner_chunk_rows
    )
    del detection_data

    keypoint_arrays = write_keypoint_and_crop_arrays(
        source_root, roots, layouts, args.inner_chunk_rows
    )

    fixture_stats = {}
    for label, shard_rows in layouts:
        fixture_path = output_root / f"{label}.zarr"
        file_count, stored_bytes = scan_files(fixture_path)
        fixture_stats[label] = {
            "path": str(fixture_path),
            "outer_shard_rows": shard_rows,
            "file_count": file_count,
            "stored_bytes": stored_bytes,
        }
        print(
            f"[{label}] files={file_count} stored={stored_bytes / 1e6:.1f} MB",
            flush=True,
        )

    manifest = {
        "schema_id": "crimson_refined_read_qualification_fixtures_v1",
        "created_at_utc": utc_now(),
        "host": socket.gethostname(),
        "source_root": str(source_root),
        "source_keypoint_run": KEYPOINT_RUN,
        "source_refined_detection_instances": len(instance_paths),
        "keypoint_arrays_copied": keypoint_arrays,
        "keypoint_arrays_omitted": ["reason"],
        "omission_reason": "Crimson reads and writes fixed-width reason_bytes first; current TensorStore build does not decode Zarr v3 string dtype",
        "inner_chunk_rows": args.inner_chunk_rows,
        "fixtures": fixture_stats,
        "elapsed_seconds": time.perf_counter() - started,
    }
    (output_root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Manifest: {output_root / 'manifest.json'}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
