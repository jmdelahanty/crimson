#!/usr/bin/env python3
"""Benchmark publication of a representative refined-detection Zarr run."""

from __future__ import annotations

import argparse
import csv
import gc
import json
import math
import os
import platform
import shutil
import socket
import subprocess
import time
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import numpy as np
import zarr
from zarr.codecs import BytesCodec, ZstdCodec


ARRAY_NAMES = (
    "bbox_img_xyxy",
    "bbox_norm_coords",
    "class_ids",
    "confidence_scores",
    "frame_counts",
    "frame_indices",
    "frame_offsets",
    "manual_edit_flags",
    "reason_bytes",
    "refined_row_ids",
    "source_detect_row_index",
    "source_kind_codes",
)

EDIT_ARRAY_NAMES = (
    "bbox_img_xyxy",
    "bbox_norm_coords",
    "confidence_scores",
    "manual_edit_flags",
    "reason_bytes",
    "source_kind_codes",
)


@dataclass
class BenchmarkResult:
    label: str
    shard_rows: int | None
    expected_data_objects: int
    create_seconds: float
    full_write_seconds: float
    single_bbox_update_seconds: float
    single_detection_edit_seconds: float
    sample_read_seconds: float
    object_scan_seconds: float
    cleanup_seconds: float
    file_count: int
    stored_bytes: int
    raw_bytes: int
    compression_ratio: float
    status: str
    error: str = ""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Combine real per-clip refined detection instances in memory, then "
            "benchmark publishing the representative full run with ordinary "
            "chunks and several Zarr v3 outer shard sizes."
        )
    )
    parser.add_argument(
        "--source-root",
        required=True,
        type=Path,
        help="Recording analysis Zarr containing clips/* refined-detection runs.",
    )
    parser.add_argument(
        "--output-root",
        required=True,
        type=Path,
        help="Parent directory for a uniquely named benchmark result directory.",
    )
    parser.add_argument(
        "--instances-glob",
        default="clips/clip_*/cameras/*/refined_detect_runs/*/instances",
        help="Glob, relative to --source-root, selecting source instance groups.",
    )
    parser.add_argument(
        "--shard-rows",
        default="ordinary,8192,32768,131072,two,one",
        help=(
            "Comma-separated outer shard row counts. Special values: ordinary, "
            "two, one. Default: %(default)s"
        ),
    )
    parser.add_argument(
        "--inner-chunk-rows",
        type=int,
        default=1024,
        help="Logical inner chunk rows. Default: %(default)s",
    )
    parser.add_argument(
        "--zstd-level",
        type=int,
        default=0,
        help="Zstd level, matching the source refined-detection arrays.",
    )
    parser.add_argument(
        "--limit-clips",
        type=int,
        help="Use only the first N matching clips (useful for a smoke test).",
    )
    parser.add_argument(
        "--keep-arrays",
        action="store_true",
        help="Retain generated arrays instead of keeping only CSV/JSON results.",
    )
    return parser.parse_args()


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def filesystem_type(path: Path) -> str:
    try:
        return subprocess.check_output(
            ["stat", "-f", "-c", "%T", str(path)],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def resolve_variants(
    spec: str, rows: int, inner_rows: int
) -> list[tuple[str, int | None]]:
    if inner_rows <= 0:
        raise ValueError("--inner-chunk-rows must be positive")
    total_inner_chunks = math.ceil(rows / inner_rows)
    variants: list[tuple[str, int | None]] = []
    seen: set[int | None] = set()
    for token in (item.strip().lower() for item in spec.split(",")):
        if not token:
            continue
        if token == "ordinary":
            shard_rows = None
            label = "ordinary"
        elif token in {"one", "two"}:
            shard_count = 1 if token == "one" else 2
            chunks_per_shard = math.ceil(total_inner_chunks / shard_count)
            shard_rows = chunks_per_shard * inner_rows
            label = f"{token}_shard_{shard_rows}_rows"
        else:
            shard_rows = int(token)
            if shard_rows <= 0 or shard_rows % inner_rows != 0:
                raise ValueError(
                    f"shard rows {shard_rows} must be a positive multiple of "
                    f"inner chunk rows {inner_rows}"
                )
            label = f"shard_{shard_rows}_rows"
        if shard_rows in seen:
            continue
        seen.add(shard_rows)
        variants.append((label, shard_rows))
    if not variants:
        raise ValueError("No shard variants were requested")
    return variants


def scan_files(path: Path) -> tuple[int, int]:
    count = 0
    total_bytes = 0
    for root, _, files in os.walk(path):
        for filename in files:
            count += 1
            try:
                total_bytes += (Path(root) / filename).stat().st_size
            except FileNotFoundError:
                pass
    return count, total_bytes


def mutate(values: np.ndarray, delta: float = 1.0) -> np.ndarray:
    result = np.asarray(values).copy()
    flat = result.reshape(-1)
    if result.dtype == np.bool_:
        flat[0] = not bool(flat[0])
    elif np.issubdtype(result.dtype, np.floating):
        finite = np.flatnonzero(np.isfinite(flat))
        index = int(finite[0]) if finite.size else 0
        flat[index] = (flat[index] if np.isfinite(flat[index]) else 0.0) + delta
    elif np.issubdtype(result.dtype, np.integer):
        flat[0] = flat[0] + int(delta)
    else:
        raise TypeError(f"Unsupported mutation dtype: {result.dtype}")
    return result


def arrays_equal(actual: np.ndarray, expected: np.ndarray) -> bool:
    return bool(np.array_equal(actual, expected, equal_nan=True))


def source_array(path: Path) -> np.ndarray:
    return np.asarray(zarr.open_array(store=path, mode="r")[:])


def load_source_bundle(instance_paths: list[Path]) -> dict[str, np.ndarray]:
    parts: dict[str, list[np.ndarray]] = {name: [] for name in ARRAY_NAMES}
    frame_base = 0
    detection_base = 0
    combined_offsets: list[np.ndarray] = [np.array([0], dtype=np.int64)]

    for instance_path in instance_paths:
        missing = [
            name for name in ARRAY_NAMES if not (instance_path / name / "zarr.json").is_file()
        ]
        if missing:
            raise FileNotFoundError(f"{instance_path} is missing {missing}")

        clip_arrays = {
            name: source_array(instance_path / name)
            for name in ARRAY_NAMES
            if name != "frame_offsets"
        }
        detection_rows = int(clip_arrays["bbox_norm_coords"].shape[0])
        frame_rows = int(clip_arrays["frame_counts"].shape[0])
        for name, values in clip_arrays.items():
            adjusted = values
            if name == "frame_indices":
                adjusted = values.astype(np.int64) + frame_base
                adjusted = adjusted.astype(values.dtype)
            parts[name].append(adjusted)

        offsets = source_array(instance_path / "frame_offsets")
        if offsets.shape[0] != frame_rows + 1:
            raise ValueError(
                f"{instance_path}/frame_offsets has {offsets.shape[0]} rows; "
                f"expected {frame_rows + 1}"
            )
        combined_offsets.append(offsets[1:].astype(np.int64) + detection_base)
        frame_base += frame_rows
        detection_base += detection_rows

    bundle = {
        name: np.concatenate(array_parts, axis=0)
        for name, array_parts in parts.items()
        if name != "frame_offsets"
    }
    bundle["frame_offsets"] = np.concatenate(combined_offsets)
    return {name: bundle[name] for name in ARRAY_NAMES}


def write_results(
    run_dir: Path,
    metadata: dict[str, Any],
    results: list[BenchmarkResult],
) -> None:
    payload = dict(metadata)
    payload["results"] = [asdict(result) for result in results]
    json_path = run_dir / "results.json"
    temporary_json = json_path.with_suffix(".json.tmp")
    temporary_json.write_text(json.dumps(payload, indent=2) + "\n")
    temporary_json.replace(json_path)

    csv_path = run_dir / "results.csv"
    temporary_csv = csv_path.with_suffix(".csv.tmp")
    fieldnames = list(BenchmarkResult.__dataclass_fields__)
    with temporary_csv.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(asdict(result) for result in results)
    temporary_csv.replace(csv_path)


def benchmark_variant(
    arrays_dir: Path,
    label: str,
    shard_rows: int | None,
    source_data: dict[str, np.ndarray],
    inner_rows: int,
    zstd_level: int,
    keep_arrays: bool,
) -> BenchmarkResult:
    variant_path = arrays_dir / label
    shutil.rmtree(variant_path, ignore_errors=True)
    raw_bytes = sum(int(values.nbytes) for values in source_data.values())
    expected_objects = sum(
        math.ceil(values.shape[0] / (shard_rows or inner_rows))
        for values in source_data.values()
    )
    timings = {
        "create": 0.0,
        "write": 0.0,
        "bbox": 0.0,
        "edit": 0.0,
        "read": 0.0,
        "scan": 0.0,
        "cleanup": 0.0,
    }
    file_count = 0
    stored_bytes = 0
    status = "PASS"
    error = ""

    try:
        started = time.perf_counter()
        group = zarr.create_group(store=variant_path, overwrite=True)
        destinations: dict[str, Any] = {}
        for name, values in source_data.items():
            trailing_shape = tuple(int(value) for value in values.shape[1:])
            create_kwargs: dict[str, Any] = {
                "name": name,
                "shape": values.shape,
                "dtype": values.dtype,
                "chunks": (inner_rows, *trailing_shape),
                "serializer": BytesCodec(
                    endian="little" if values.dtype.itemsize > 1 else None
                ),
                "compressors": [ZstdCodec(level=zstd_level)],
                "overwrite": True,
            }
            if shard_rows is not None:
                create_kwargs["shards"] = (shard_rows, *trailing_shape)
            destinations[name] = group.create_array(**create_kwargs)
        timings["create"] = time.perf_counter() - started

        started = time.perf_counter()
        for name, values in source_data.items():
            destinations[name][:] = values
        timings["write"] = time.perf_counter() - started

        started = time.perf_counter()
        for name, values in source_data.items():
            sample_rows = sorted({0, values.shape[0] // 2, values.shape[0] - 1})
            if not arrays_equal(destinations[name][sample_rows], values[sample_rows]):
                raise RuntimeError(f"{name}: sample validation failed after full write")
        timings["read"] += time.perf_counter() - started

        row_index = source_data["bbox_norm_coords"].shape[0] // 2
        bbox_replacement = mutate(source_data["bbox_norm_coords"][row_index], 0.125)
        started = time.perf_counter()
        destinations["bbox_norm_coords"][row_index] = bbox_replacement
        timings["bbox"] = time.perf_counter() - started

        replacements: dict[str, np.ndarray] = {}
        started = time.perf_counter()
        for name in EDIT_ARRAY_NAMES:
            replacement = mutate(source_data[name][row_index], 1.0)
            destinations[name][row_index] = replacement
            replacements[name] = replacement
        timings["edit"] = time.perf_counter() - started

        started = time.perf_counter()
        if not arrays_equal(destinations["bbox_norm_coords"][row_index], replacements["bbox_norm_coords"]):
            raise RuntimeError("bbox_norm_coords: single-row update validation failed")
        for name, replacement in replacements.items():
            if not arrays_equal(destinations[name][row_index], replacement):
                raise RuntimeError(f"{name}: edit bundle validation failed")
        timings["read"] += time.perf_counter() - started

        started = time.perf_counter()
        file_count, stored_bytes = scan_files(variant_path)
        timings["scan"] = time.perf_counter() - started
    except Exception as exc:
        status = "FAIL"
        error = f"{type(exc).__name__}: {exc}"
    finally:
        if not keep_arrays:
            started = time.perf_counter()
            shutil.rmtree(variant_path, ignore_errors=True)
            timings["cleanup"] = time.perf_counter() - started
        gc.collect()

    return BenchmarkResult(
        label=label,
        shard_rows=shard_rows,
        expected_data_objects=expected_objects,
        create_seconds=timings["create"],
        full_write_seconds=timings["write"],
        single_bbox_update_seconds=timings["bbox"],
        single_detection_edit_seconds=timings["edit"],
        sample_read_seconds=timings["read"],
        object_scan_seconds=timings["scan"],
        cleanup_seconds=timings["cleanup"],
        file_count=file_count,
        stored_bytes=stored_bytes,
        raw_bytes=raw_bytes,
        compression_ratio=(stored_bytes / raw_bytes if raw_bytes else float("nan")),
        status=status,
        error=error,
    )


def main() -> int:
    args = parse_args()
    source_root = args.source_root.resolve()
    output_root = args.output_root.resolve()
    instance_paths = sorted(source_root.glob(args.instances_glob))
    if args.limit_clips is not None:
        if args.limit_clips <= 0:
            raise ValueError("--limit-clips must be positive")
        instance_paths = instance_paths[: args.limit_clips]
    if not instance_paths:
        raise FileNotFoundError(
            f"No instance groups matched {args.instances_glob!r} below {source_root}"
        )

    output_root.mkdir(parents=True, exist_ok=True)
    run_dir = output_root / (
        "detection-run-shard-benchmark-" + datetime.now().strftime("%Y%m%d-%H%M%S")
    )
    arrays_dir = run_dir / "arrays"
    arrays_dir.mkdir(parents=True)

    source_read_started = time.perf_counter()
    source_data = load_source_bundle(instance_paths)
    source_read_seconds = time.perf_counter() - source_read_started
    primary_rows = int(source_data["bbox_norm_coords"].shape[0])
    variants = resolve_variants(args.shard_rows, primary_rows, args.inner_chunk_rows)

    metadata: dict[str, Any] = {
        "schema_id": "crimson_detection_run_sharding_benchmark_v1",
        "started_at_utc": utc_now(),
        "host": socket.gethostname(),
        "platform": platform.platform(),
        "source_root": str(source_root),
        "source_instances": [str(path.relative_to(source_root)) for path in instance_paths],
        "source_instance_count": len(instance_paths),
        "arrays": {
            name: {"shape": list(values.shape), "dtype": str(values.dtype)}
            for name, values in source_data.items()
        },
        "omitted_redundant_arrays": ["reason"],
        "omission_reason": (
            "reason_bytes carries the same labels in the fixed-width representation "
            "used for cross-platform compatibility"
        ),
        "source_read_seconds": source_read_seconds,
        "raw_bytes": sum(int(values.nbytes) for values in source_data.values()),
        "inner_chunk_rows": args.inner_chunk_rows,
        "zstd_level": args.zstd_level,
        "zarr_version": zarr.__version__,
        "numpy_version": np.__version__,
        "zarr_async_concurrency": zarr.config.get("async.concurrency"),
        "output_root": str(output_root),
        "filesystem_type": filesystem_type(output_root),
        "arrays_retained": bool(args.keep_arrays),
        "timing_semantics": "client-observed synchronous API completion; no global fsync",
    }
    results: list[BenchmarkResult] = []
    write_results(run_dir, metadata, results)

    print(f"Source root: {source_root}", flush=True)
    print(f"Source instance groups: {len(instance_paths)}", flush=True)
    print(
        f"Detection rows: {primary_rows}; frame rows: "
        f"{source_data['frame_counts'].shape[0]}; arrays: {len(source_data)}; "
        f"raw={metadata['raw_bytes'] / 1_000_000:.1f} MB",
        flush=True,
    )
    print(f"Destination: {run_dir} ({metadata['filesystem_type']})", flush=True)
    print(f"Source preload: {source_read_seconds:.3f} s", flush=True)

    for label, shard_rows in variants:
        print(f"\n[{label}] shard_rows={shard_rows}", flush=True)
        result = benchmark_variant(
            arrays_dir=arrays_dir,
            label=label,
            shard_rows=shard_rows,
            source_data=source_data,
            inner_rows=args.inner_chunk_rows,
            zstd_level=args.zstd_level,
            keep_arrays=args.keep_arrays,
        )
        results.append(result)
        write_results(run_dir, metadata, results)
        print(
            f"status={result.status} create={result.create_seconds:.3f}s "
            f"write={result.full_write_seconds:.3f}s "
            f"bbox_update={result.single_bbox_update_seconds:.3f}s "
            f"edit_bundle={result.single_detection_edit_seconds:.3f}s "
            f"files={result.file_count} "
            f"stored={result.stored_bytes / 1_000_000:.1f}MB",
            flush=True,
        )
        if result.error:
            print(f"error={result.error}", flush=True)

    metadata["finished_at_utc"] = utc_now()
    write_results(run_dir, metadata, results)
    try:
        arrays_dir.rmdir()
    except OSError:
        pass
    print(f"\nResults JSON: {run_dir / 'results.json'}", flush=True)
    print(f"Results CSV:  {run_dir / 'results.csv'}", flush=True)
    return 0 if all(result.status == "PASS" for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
