#!/usr/bin/env python3
"""Benchmark Zarr v3 keypoint publication and small updates by shard size."""

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


@dataclass
class BenchmarkResult:
    label: str
    shard_rows: int | None
    expected_data_objects: int
    create_seconds: float
    full_write_seconds: float
    single_row_update_seconds: float
    inner_chunk_update_seconds: float
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
            "Write a production-shaped keypoint array with ordinary chunks and "
            "several Zarr v3 outer shard sizes."
        )
    )
    parser.add_argument(
        "--source-array",
        required=True,
        type=Path,
        help="Existing Zarr array used as the in-memory benchmark payload.",
    )
    parser.add_argument(
        "--output-root",
        required=True,
        type=Path,
        help="Parent directory for a uniquely named benchmark result directory.",
    )
    parser.add_argument(
        "--shard-rows",
        default="ordinary,2048,8192,32768,131072,two,one",
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
        help="Zstd level, matching the production keypoint arrays by default.",
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


def resolve_variants(spec: str, rows: int, inner_rows: int) -> list[tuple[str, int | None]]:
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


def mutate_first_finite(values: np.ndarray, delta: float) -> np.ndarray:
    result = np.asarray(values).copy()
    flat = result.reshape(-1)
    finite = np.flatnonzero(np.isfinite(flat))
    index = int(finite[0]) if finite.size else 0
    flat[index] = (flat[index] if np.isfinite(flat[index]) else 0.0) + delta
    return result


def arrays_equal(actual: np.ndarray, expected: np.ndarray) -> bool:
    return bool(np.array_equal(actual, expected, equal_nan=True))


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
    source_data: np.ndarray,
    inner_rows: int,
    zstd_level: int,
    keep_arrays: bool,
) -> BenchmarkResult:
    variant_path = arrays_dir / label
    shutil.rmtree(variant_path, ignore_errors=True)
    shape = tuple(int(value) for value in source_data.shape)
    chunks = (inner_rows, *shape[1:])
    expected_objects = math.ceil(shape[0] / (shard_rows or inner_rows))
    raw_bytes = int(source_data.nbytes)
    create_seconds = 0.0
    full_write_seconds = 0.0
    single_row_update_seconds = 0.0
    inner_chunk_update_seconds = 0.0
    sample_read_seconds = 0.0
    object_scan_seconds = 0.0
    cleanup_seconds = 0.0
    file_count = 0
    stored_bytes = 0
    status = "PASS"
    error = ""

    try:
        create_kwargs: dict[str, Any] = {
            "store": variant_path,
            "shape": shape,
            "dtype": source_data.dtype,
            "chunks": chunks,
            "serializer": BytesCodec(endian="little"),
            "compressors": [ZstdCodec(level=zstd_level)],
            "zarr_format": 3,
            "overwrite": True,
        }
        if shard_rows is not None:
            create_kwargs["shards"] = (shard_rows, *shape[1:])

        started = time.perf_counter()
        destination = zarr.create_array(**create_kwargs)
        create_seconds = time.perf_counter() - started

        started = time.perf_counter()
        destination[:] = source_data
        full_write_seconds = time.perf_counter() - started

        sample_rows = sorted(
            {
                0,
                min(shape[0] - 1, inner_rows - 1),
                min(shape[0] - 1, inner_rows),
                shape[0] // 2,
                shape[0] - 1,
            }
        )
        started = time.perf_counter()
        initial_samples = destination[sample_rows]
        sample_read_seconds += time.perf_counter() - started
        if not arrays_equal(initial_samples, source_data[sample_rows]):
            raise RuntimeError("sample validation failed after full write")

        row_index = shape[0] // 2
        row_replacement = mutate_first_finite(source_data[row_index], 0.125)
        started = time.perf_counter()
        destination[row_index] = row_replacement
        single_row_update_seconds = time.perf_counter() - started

        chunk_start = min(
            (shape[0] // 3 // inner_rows) * inner_rows,
            max(0, shape[0] - inner_rows),
        )
        chunk_end = min(shape[0], chunk_start + inner_rows)
        chunk_replacement = mutate_first_finite(
            source_data[chunk_start:chunk_end], 0.25
        )
        started = time.perf_counter()
        destination[chunk_start:chunk_end] = chunk_replacement
        inner_chunk_update_seconds = time.perf_counter() - started

        started = time.perf_counter()
        observed_row = destination[row_index]
        observed_chunk = destination[chunk_start:chunk_end]
        sample_read_seconds += time.perf_counter() - started
        if not arrays_equal(observed_row, row_replacement):
            raise RuntimeError("single-row update validation failed")
        if not arrays_equal(observed_chunk, chunk_replacement):
            raise RuntimeError("inner-chunk update validation failed")

        started = time.perf_counter()
        file_count, stored_bytes = scan_files(variant_path)
        object_scan_seconds = time.perf_counter() - started
    except Exception as exc:  # Keep partial measurements for diagnosis.
        status = "FAIL"
        error = f"{type(exc).__name__}: {exc}"
    finally:
        if not keep_arrays:
            started = time.perf_counter()
            shutil.rmtree(variant_path, ignore_errors=True)
            cleanup_seconds = time.perf_counter() - started
        gc.collect()

    compression_ratio = (
        float(stored_bytes) / float(raw_bytes) if raw_bytes else float("nan")
    )
    return BenchmarkResult(
        label=label,
        shard_rows=shard_rows,
        expected_data_objects=expected_objects,
        create_seconds=create_seconds,
        full_write_seconds=full_write_seconds,
        single_row_update_seconds=single_row_update_seconds,
        inner_chunk_update_seconds=inner_chunk_update_seconds,
        sample_read_seconds=sample_read_seconds,
        object_scan_seconds=object_scan_seconds,
        cleanup_seconds=cleanup_seconds,
        file_count=file_count,
        stored_bytes=stored_bytes,
        raw_bytes=raw_bytes,
        compression_ratio=compression_ratio,
        status=status,
        error=error,
    )


def main() -> int:
    args = parse_args()
    source_path = args.source_array.resolve()
    output_root = args.output_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    run_dir = output_root / (
        "keypoint-shard-benchmark-"
        + datetime.now().strftime("%Y%m%d-%H%M%S")
    )
    arrays_dir = run_dir / "arrays"
    arrays_dir.mkdir(parents=True)

    source = zarr.open_array(store=source_path, mode="r")
    if source.ndim < 2:
        raise ValueError(f"Expected a keypoint-like array, got shape {source.shape}")
    source_read_started = time.perf_counter()
    source_data = np.asarray(source[:])
    source_read_seconds = time.perf_counter() - source_read_started
    variants = resolve_variants(
        args.shard_rows, source_data.shape[0], args.inner_chunk_rows
    )

    metadata: dict[str, Any] = {
        "schema_id": "crimson_keypoint_sharding_benchmark_v1",
        "started_at_utc": utc_now(),
        "host": socket.gethostname(),
        "platform": platform.platform(),
        "source_array": str(source_path),
        "source_shape": list(source_data.shape),
        "source_dtype": str(source_data.dtype),
        "source_read_seconds": source_read_seconds,
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

    print(f"Source: {source_path}", flush=True)
    print(
        f"Shape: {source_data.shape} dtype={source_data.dtype} "
        f"raw={source_data.nbytes / 1_000_000:.1f} MB",
        flush=True,
    )
    print(
        f"Destination: {run_dir} ({metadata['filesystem_type']})",
        flush=True,
    )
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
            f"row_update={result.single_row_update_seconds:.3f}s "
            f"chunk_update={result.inner_chunk_update_seconds:.3f}s "
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
