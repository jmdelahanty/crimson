#!/usr/bin/env python3
"""Prepare, run, summarize, and clean the Crimson TensorStore PRFS benchmark."""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import shutil
import socket
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

LAYOUTS: tuple[tuple[str, int | None], ...] = (
    ("ordinary_1024", None),
    ("shard_32768", 32_768),
    ("shard_131072", 131_072),
    ("shard_262144", 262_144),
)
WORKLOADS = ("random_row", "random_1024", "shuffled_repeat", "sequential_scan")
INNER_ROWS = 1024


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    prepare = subparsers.add_parser("prepare")
    prepare.add_argument("--source-array", required=True, type=Path)
    prepare.add_argument("--output-root", required=True, type=Path)
    prepare.add_argument("--overwrite", action="store_true")

    run = subparsers.add_parser("run")
    run.add_argument("--output-root", required=True, type=Path)
    run.add_argument("--binary", required=True, type=Path)
    run.add_argument("--host-label", default=socket.gethostname())
    run.add_argument("--repetitions", type=int, default=3)

    cleanup = subparsers.add_parser("cleanup")
    cleanup.add_argument("--output-root", required=True, type=Path)

    return parser.parse_args()


def array_create_kwargs(source: zarr.Array, destination: Path, shard_rows: int | None) -> dict[str, Any]:
    kwargs: dict[str, Any] = {
        "store": destination,
        "shape": source.shape,
        "dtype": source.dtype,
        "chunks": (INNER_ROWS, *source.shape[1:]),
        "zarr_format": 3,
        "overwrite": True,
    }
    if shard_rows is not None:
        kwargs["shards"] = (shard_rows, *source.shape[1:])
    for name in ("serializer", "compressors", "filters", "fill_value"):
        value = getattr(source, name, None)
        if value is not None and (name != "filters" or value):
            kwargs[name] = value
    return kwargs


def decoded_digest(array: zarr.Array, row_step: int) -> str:
    import hashlib

    digest = hashlib.sha256()
    for start in range(0, int(array.shape[0]), row_step):
        stop = min(int(array.shape[0]), start + row_step)
        values = np.ascontiguousarray(array[start:stop, ...])
        digest.update(values.view(np.uint8))
    return digest.hexdigest()


def prepare_payloads(source_path: Path, output_root: Path, overwrite: bool) -> None:
    # Preparation needs the Palette Python environment, but the `run`
    # subcommand intentionally remains standard-library-only so the same
    # harness can execute on bare LSF compute nodes.
    global np, zarr
    import numpy as np
    import zarr

    source = zarr.open_array(source_path, mode="r")
    if tuple(source.shape[1:]) != (5, 2) or source.dtype != np.dtype("float64"):
        raise ValueError(f"expected float64[N,5,2] keypoints_img, got {source.dtype}{source.shape}")
    payload_root = output_root / "payloads"
    payload_root.mkdir(parents=True, exist_ok=True)
    manifest: dict[str, Any] = {
        "schema": "crimson.tensorstore_keypoint_read_payloads.v1",
        "source_array": str(source_path.resolve()),
        "source_shape": list(source.shape),
        "source_dtype": str(source.dtype),
        "inner_chunk_rows": INNER_ROWS,
        "layouts": [],
    }
    source_digest = decoded_digest(source, 131_072)
    for label, shard_rows in LAYOUTS:
        destination_path = payload_root / label / "keypoints_img"
        if destination_path.exists():
            if not overwrite:
                raise FileExistsError(f"payload already exists: {destination_path}")
            shutil.rmtree(destination_path.parent)
        destination_path.parent.mkdir(parents=True, exist_ok=True)
        started = time.perf_counter()
        destination = zarr.create_array(**array_create_kwargs(source, destination_path, shard_rows))
        write_step = shard_rows or INNER_ROWS
        for start in range(0, int(source.shape[0]), write_step):
            stop = min(int(source.shape[0]), start + write_step)
            destination[start:stop, ...] = source[start:stop, ...]
        copy_seconds = time.perf_counter() - started
        destination_digest = decoded_digest(destination, 131_072)
        if destination_digest != source_digest:
            raise RuntimeError(f"decoded digest mismatch for {label}")
        manifest["layouts"].append(
            {
                "label": label,
                "shard_rows": shard_rows,
                "array_path": str(destination_path),
                "copy_seconds": copy_seconds,
                "decoded_sha256": destination_digest,
            }
        )
        print(f"[PREPARED] {label} in {copy_seconds:.3f}s", flush=True)
    manifest["source_decoded_sha256"] = source_digest
    (output_root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def evict_payload_files(path: Path) -> dict[str, int]:
    attempted = 0
    advised = 0
    if not hasattr(os, "posix_fadvise") or not hasattr(os, "POSIX_FADV_DONTNEED"):
        return {"attempted": 0, "advised": 0}
    for root, _directories, filenames in os.walk(path):
        for filename in filenames:
            attempted += 1
            file_path = Path(root) / filename
            try:
                descriptor = os.open(file_path, os.O_RDONLY)
                try:
                    os.posix_fadvise(descriptor, 0, 0, os.POSIX_FADV_DONTNEED)
                    advised += 1
                finally:
                    os.close(descriptor)
            except OSError:
                pass
    return {"attempted": attempted, "advised": advised}


def parse_result(output: str) -> dict[str, Any]:
    for line in reversed(output.splitlines()):
        if line.startswith("RESULT_JSON="):
            return json.loads(line.removeprefix("RESULT_JSON="))
    raise RuntimeError(f"benchmark output did not contain RESULT_JSON: {output[-1000:]}")


def flatten_results(result: dict[str, Any], host_label: str, eviction: dict[str, int]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for pass_result in result["passes"]:
        row = {
            "host_label": host_label,
            "layout": result["layout"],
            "workload": result["workload"],
            "repetition": result["repetition"],
            "seed": result["seed"],
            "phase": pass_result["phase"],
            "eviction_files_attempted": eviction["attempted"],
            "eviction_files_advised": eviction["advised"],
        }
        row.update(pass_result)
        logical = max(1, int(row["logical_bytes"]))
        row["read_bytes_over_logical"] = float(row["read_bytes_delta"]) / logical
        row["rchar_over_logical"] = float(row["rchar_delta"]) / logical
        rows.append(row)
    return rows


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    fieldnames = list(rows[0]) if rows else []
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def median(rows: list[dict[str, Any]], field: str) -> float:
    return float(statistics.median(float(row[field]) for row in rows))


def write_summary(path: Path, host_label: str, rows: list[dict[str, Any]]) -> None:
    lines = [
        f"# TensorStore keypoints_img read benchmark — {host_label}",
        "",
        "Fresh-process passes follow a best-effort `POSIX_FADV_DONTNEED` eviction. "
        "PRFS/server cache state is not controllable, so physical-read counters are observational.",
        "",
        "| Layout | Workload | Phase | wall s | p50 ms | p95 ms | p99 ms | RSS MiB | read/logical | rchar/logical | syscr |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for layout, _shards in LAYOUTS:
        for workload in WORKLOADS:
            for phase in ("fresh_process", "warm_repeat"):
                selected = [
                    row for row in rows
                    if row["layout"] == layout
                    and row["workload"] == workload
                    and row["phase"] == phase
                ]
                if not selected:
                    continue
                lines.append(
                    f"| {layout} | {workload} | {phase} | "
                    f"{median(selected, 'wall_seconds'):.4f} | "
                    f"{median(selected, 'latency_p50_ms'):.3f} | "
                    f"{median(selected, 'latency_p95_ms'):.3f} | "
                    f"{median(selected, 'latency_p99_ms'):.3f} | "
                    f"{median(selected, 'max_rss_bytes') / (1024 * 1024):.1f} | "
                    f"{median(selected, 'read_bytes_over_logical'):.3f} | "
                    f"{median(selected, 'rchar_over_logical'):.3f} | "
                    f"{median(selected, 'syscr_delta'):.0f} |"
                )
    path.write_text("\n".join(lines) + "\n")


def run_benchmark(output_root: Path, binary: Path, host_label: str, repetitions: int) -> None:
    if repetitions < 1:
        raise ValueError("--repetitions must be positive")
    manifest = json.loads((output_root / "manifest.json").read_text())
    paths = {item["label"]: Path(item["array_path"]) for item in manifest["layouts"]}
    result_dir = output_root / "results" / host_label
    result_dir.mkdir(parents=True, exist_ok=True)
    raw_path = result_dir / "raw.jsonl"
    flat_rows: list[dict[str, Any]] = []
    with raw_path.open("w") as raw_stream:
        for repetition in range(repetitions):
            order = list(LAYOUTS)
            if repetition % 2:
                order.reverse()
            for label, _shards in order:
                array_path = paths[label]
                for workload_index, workload in enumerate(WORKLOADS):
                    eviction = evict_payload_files(array_path)
                    seed = 20260714 + repetition * 100 + workload_index
                    completed = subprocess.run(
                        [
                            str(binary),
                            str(array_path),
                            label,
                            workload,
                            str(repetition),
                            str(seed),
                        ],
                        check=False,
                        text=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                    )
                    if completed.returncode != 0:
                        raise RuntimeError(
                            f"benchmark command failed with exit {completed.returncode}:\n"
                            f"{completed.stdout}"
                        )
                    result = parse_result(completed.stdout)
                    result["host_label"] = host_label
                    result["eviction"] = eviction
                    raw_stream.write(json.dumps(result, sort_keys=True) + "\n")
                    raw_stream.flush()
                    flat_rows.extend(flatten_results(result, host_label, eviction))
                    print(
                        f"[MEASURED] rep={repetition} {label} {workload}",
                        flush=True,
                    )
    write_csv(result_dir / "results.csv", flat_rows)
    write_summary(result_dir / "summary.md", host_label, flat_rows)


def main() -> int:
    args = parse_args()
    if args.command == "prepare":
        prepare_payloads(args.source_array, args.output_root, args.overwrite)
    elif args.command == "run":
        run_benchmark(args.output_root, args.binary, args.host_label, args.repetitions)
    elif args.command == "cleanup":
        payload_root = args.output_root / "payloads"
        if payload_root.exists():
            shutil.rmtree(payload_root)
        print(f"[CLEANED] {payload_root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
