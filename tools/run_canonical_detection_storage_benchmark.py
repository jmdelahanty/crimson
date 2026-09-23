#!/usr/bin/env python3
"""Run and aggregate Crimson's canonical detection storage benchmark."""

from __future__ import annotations

import argparse
import csv
import json
import math
import platform
import subprocess
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from statistics import median
from typing import Any


DEFAULT_ROOT = Path(
    "/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/"
    "canonical_detection_storage/workflows/"
    "sleepyfish_det_storage_access_aware_full_20260724_03/candidates/"
    "sleepyfish_det_storage_access_aware_full_20260724_03/frames_full"
)
DEFAULT_BINARY = Path(
    "build/macos-arm64-release/canonical_detection_storage_benchmark"
)
LAYOUT_PATTERNS = {
    "regular": "*__regular__chunk_1048576__*.zarr",
    "hybrid": (
        "*__sharded__chunk_131072__eager_chunk_1048576__"
        "shard_8388608__*.zarr"
    ),
}
WORKLOADS = (
    "random_frames_contract",
    "random_frames_ui",
    "random_ranges_contract",
    "sequential_contract_forward",
    "sequential_ui_forward",
    "sequential_ui_reverse",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run balanced process-first comparisons of Palette's regular and "
            "access-aware hybrid canonical detection stores."
        )
    )
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument(
        "--repetitions",
        nargs="+",
        default=[f"{index:03d}" for index in range(5)],
    )
    parser.add_argument(
        "--cache-bytes", nargs="+", type=int, default=[0, 128 * 1024 * 1024]
    )
    parser.add_argument("--mode", choices=("quick", "full"), default="full")
    parser.add_argument(
        "--network-label", default="mounted_smb_os_cache_uncontrolled"
    )
    parser.add_argument(
        "--no-resume", action="store_true", help="Rerun existing process results"
    )
    return parser.parse_args()


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return math.nan
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def describe(values: list[float]) -> dict[str, float | int]:
    return {
        "count": len(values),
        "min": min(values),
        "median": median(values),
        "p95": percentile(values, 0.95),
        "max": max(values),
    }


def command_text(command: list[str]) -> str:
    return " ".join(command)


def capture(command: list[str], cwd: Path | None = None) -> str:
    result = subprocess.run(
        command, cwd=cwd, text=True, capture_output=True, check=False
    )
    return result.stdout.strip() if result.returncode == 0 else ""


def discover_fixture(root: Path, repetition: str, layout: str) -> tuple[Path, Path]:
    directory = root / f"repetition_{repetition}"
    matches = sorted(directory.glob(LAYOUT_PATTERNS[layout]))
    if len(matches) != 1:
        raise RuntimeError(
            f"Expected one {layout} fixture in {directory}, found {len(matches)}"
        )
    store = matches[0]
    palette_result = store.with_suffix(".prfs-read.json")
    if not palette_result.is_file():
        raise RuntimeError(f"Missing Palette result: {palette_result}")
    return store, palette_result


def validate_result(
    result: dict[str, Any], layout: str, repetition: str, cache_bytes: int, mode: str
) -> None:
    expected = {
        "schema_id": "crimson.canonical_detection_storage_benchmark",
        "status": "pass",
        "layout": layout,
        "repetition": repetition,
        "cache_bytes": cache_bytes,
        "mode": mode,
        "offset_read_calls_final": 1,
    }
    for key, value in expected.items():
        if result.get(key) != value:
            raise RuntimeError(
                f"Invalid result field {key}: expected {value!r}, "
                f"found {result.get(key)!r}"
            )
    offsets = result.get("initialization", {}).get("frame_row_offsets", {})
    if offsets.get("read_calls") != 1 or not offsets.get("store_closed_after_read"):
        raise RuntimeError("Offsets were not read once, retained, and closed")
    for workload in WORKLOADS:
        passes = result.get("workloads", {}).get(workload, {}).get("passes", [])
        if len(passes) != 2:
            raise RuntimeError(f"{workload} does not contain two passes")
        if passes[0].get("value_digest") != passes[1].get("value_digest"):
            raise RuntimeError(f"{workload} decoded values differ between passes")
        if any(item.get("offset_read_calls_after") != 1 for item in passes):
            raise RuntimeError(f"{workload} reread frame_row_offsets")


def load_existing(
    path: Path, layout: str, repetition: str, cache_bytes: int, mode: str
) -> dict[str, Any] | None:
    if not path.is_file():
        return None
    try:
        result = json.loads(path.read_text())
        validate_result(result, layout, repetition, cache_bytes, mode)
        return result
    except (json.JSONDecodeError, OSError, RuntimeError):
        return None


def run_process(
    binary: Path,
    store: Path,
    palette_result: Path,
    cache_bytes: int,
    layout: str,
    repetition: str,
    mode: str,
    output_path: Path,
) -> dict[str, Any]:
    command = [
        str(binary),
        str(store),
        str(cache_bytes),
        layout,
        repetition,
        str(palette_result),
        mode,
    ]
    print(f"[run] {command_text(command)}", flush=True)
    completed = subprocess.run(command, text=True, capture_output=True, check=False)
    output_path.with_suffix(".stderr.log").write_text(completed.stderr)
    if completed.returncode != 0:
        raise RuntimeError(
            f"Benchmark failed ({completed.returncode}): {completed.stderr.strip()}"
        )
    lines = [line for line in completed.stdout.splitlines() if line.strip()]
    if len(lines) != 1:
        raise RuntimeError(f"Expected one JSON line, found {len(lines)}")
    result = json.loads(lines[0])
    validate_result(result, layout, repetition, cache_bytes, mode)
    output_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return result


def execution_plan(
    repetitions: list[str], cache_values: list[int]
) -> list[tuple[str, int, str]]:
    plan: list[tuple[str, int, str]] = []
    for repetition_index, repetition in enumerate(repetitions):
        caches = cache_values if repetition_index % 2 == 0 else list(reversed(cache_values))
        for cache_index, cache_bytes in enumerate(caches):
            layouts = ["regular", "hybrid"]
            if (repetition_index + cache_index) % 2:
                layouts.reverse()
            for layout in layouts:
                plan.append((repetition, cache_bytes, layout))
    return plan


def flatten_results(results: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for result in results:
        common = {
            "repetition": result["repetition"],
            "cache_bytes": result["cache_bytes"],
            "cache_mib": result["cache_bytes"] / (1024 * 1024),
            "layout": result["layout"],
            "mode": result["mode"],
        }
        for workload in WORKLOADS:
            for item in result["workloads"][workload]["passes"]:
                metrics = item["metrics"]
                rows.append(
                    {
                        **common,
                        "workload": workload,
                        "pass_index": item["pass_index"],
                        "cache_condition": item["cache_condition"],
                        "requested_units": item["requested_units"],
                        "selected_instance_rows": item["selected_instance_rows"],
                        "logical_bytes": item["logical_bytes"],
                        "read_ms": item["read_ms"],
                        "wall_ms": item["wall_ms"],
                        "p50_unit_ms": item["p50_unit_ms"],
                        "p95_unit_ms": item["p95_unit_ms"],
                        "max_unit_ms": item["max_unit_ms"],
                        "storage_frames_per_second": item.get(
                            "storage_frames_per_second", ""
                        ),
                        "file_reads": metrics["file_reads"],
                        "file_batch_reads": metrics["file_batch_reads"],
                        "file_bytes": metrics["file_bytes"],
                        "cache_hits": metrics["cache_hits"],
                        "cache_misses": metrics["cache_misses"],
                        "cache_evictions": metrics["cache_evictions"],
                        "value_digest": item["value_digest"],
                    }
                )
    return rows


def flatten_initialization(results: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for result in results:
        root = result["metadata"]["consolidated_root"]
        opened = result["initialization"]["exact_array_open"]
        offsets = result["initialization"]["frame_row_offsets"]
        rows.append(
            {
                "repetition": result["repetition"],
                "cache_bytes": result["cache_bytes"],
                "cache_mib": result["cache_bytes"] / (1024 * 1024),
                "layout": result["layout"],
                "mode": result["mode"],
                "schema_version": result["schema_version"],
                "consolidated_root_read_ms": root["read_ms"],
                "consolidated_root_file_reads": root["metrics"]["file_reads"],
                "consolidated_root_file_bytes": root["metrics"]["file_bytes"],
                "exact_array_open_ms": opened["elapsed_ms"],
                "typed_open_attempts": opened["typed_open_attempts"],
                "fallback_open_attempts": opened["fallback_open_attempts"],
                "exact_open_file_reads": opened["metrics"]["file_reads"],
                "exact_open_file_bytes": opened["metrics"]["file_bytes"],
                "offset_read_calls": offsets["read_calls"],
                "offset_read_ms": offsets["read_ms"],
                "offset_total_ms": offsets["total_ms"],
                "offset_logical_bytes": offsets["logical_bytes"],
                "offset_retained_bytes": offsets["retained_bytes"],
                "offset_file_reads": offsets["metrics"]["file_reads"],
                "offset_file_bytes": offsets["metrics"]["file_bytes"],
                "offset_cache_hits": offsets["metrics"]["cache_hits"],
                "offset_cache_misses": offsets["metrics"]["cache_misses"],
                "offset_value_digest": offsets["value_digest"],
                "offset_read_calls_final": result["offset_read_calls_final"],
            }
        )
    return rows


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        raise RuntimeError("No benchmark rows were collected")
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def assert_equivalent_values(rows: list[dict[str, Any]]) -> None:
    groups: dict[tuple[str, str, int], set[str]] = defaultdict(set)
    for row in rows:
        key = (row["repetition"], row["workload"], row["pass_index"])
        groups[key].add(row["value_digest"])
    mismatches = {key: values for key, values in groups.items() if len(values) != 1}
    if mismatches:
        raise RuntimeError(f"Decoded-value mismatch across layouts/caches: {mismatches}")


def aggregate(
    results: list[dict[str, Any]], rows: list[dict[str, Any]]
) -> dict[str, Any]:
    grouped_rows: dict[tuple[int, str, str, int], list[dict[str, Any]]] = defaultdict(
        list
    )
    for row in rows:
        grouped_rows[
            (row["cache_bytes"], row["layout"], row["workload"], row["pass_index"])
        ].append(row)

    output: dict[str, Any] = {"groups": {}, "gates": []}
    offset_groups: dict[tuple[int, str], list[float]] = defaultdict(list)
    for result in results:
        offset_groups[(result["cache_bytes"], result["layout"])].append(
            float(result["initialization"]["frame_row_offsets"]["read_ms"])
        )

    for (cache_bytes, layout), values in sorted(offset_groups.items()):
        key = f"cache_{cache_bytes}/{layout}"
        output["groups"].setdefault(key, {})["frame_row_offsets_read_ms"] = describe(
            values
        )
        measured = median(values)
        output["gates"].append(
            {
                "name": "offset_read_median_under_100_ms",
                "cache_bytes": cache_bytes,
                "layout": layout,
                "measured": measured,
                "limit": 100.0,
                "passed": measured < 100.0,
            }
        )

    for cache_bytes in sorted({result["cache_bytes"] for result in results}):
        regular = median(offset_groups[(cache_bytes, "regular")])
        hybrid = median(offset_groups[(cache_bytes, "hybrid")])
        delta = hybrid - regular
        output["gates"].append(
            {
                "name": "hybrid_offset_absolute_regression_at_most_25_ms",
                "cache_bytes": cache_bytes,
                "measured": delta,
                "limit": 25.0,
                "passed": delta <= 25.0,
            }
        )

    for key, group in sorted(grouped_rows.items()):
        cache_bytes, layout, workload, pass_index = key
        group_key = f"cache_{cache_bytes}/{layout}"
        workload_key = f"{workload}/pass_{pass_index}"
        summary = {
            "p95_unit_ms": describe([float(row["p95_unit_ms"]) for row in group]),
            "wall_ms": describe([float(row["wall_ms"]) for row in group]),
            "file_reads": describe([float(row["file_reads"]) for row in group]),
            "file_bytes": describe([float(row["file_bytes"]) for row in group]),
            "cache_hits": describe([float(row["cache_hits"]) for row in group]),
            "cache_misses": describe([float(row["cache_misses"]) for row in group]),
            "cache_evictions": describe(
                [float(row["cache_evictions"]) for row in group]
            ),
        }
        fps = [
            float(row["storage_frames_per_second"])
            for row in group
            if row["storage_frames_per_second"] != ""
        ]
        if fps:
            summary["storage_frames_per_second"] = describe(fps)
        output["groups"].setdefault(group_key, {})[workload_key] = summary

        if workload in ("random_frames_ui", "random_frames_contract"):
            measured = percentile(
                [float(row["p95_unit_ms"]) for row in group], 0.95
            )
            output["gates"].append(
                {
                    "name": (
                        "random_ui_frame_cross_repetition_p95_under_150_ms"
                        if workload == "random_frames_ui"
                        else "random_contract_frame_cross_repetition_p95_under_150_ms"
                    ),
                    "cache_bytes": cache_bytes,
                    "layout": layout,
                    "pass_index": pass_index,
                    "measured": measured,
                    "limit": 150.0,
                    "passed": measured < 150.0,
                }
            )
        if workload in ("sequential_contract_forward", "sequential_ui_forward"):
            measured = percentile(fps, 0.05)
            output["gates"].append(
                {
                    "name": "sequential_cross_repetition_p05_at_least_1400_fps",
                    "cache_bytes": cache_bytes,
                    "layout": layout,
                    "workload": workload,
                    "pass_index": pass_index,
                    "measured": measured,
                    "limit": 1400.0,
                    "passed": measured >= 1400.0,
                }
            )

    output["all_gates_passed"] = all(item["passed"] for item in output["gates"])
    return output


def environment(repo: Path, args: argparse.Namespace) -> dict[str, Any]:
    mount_lines = [
        line
        for line in capture(["mount"]).splitlines()
        if "/Volumes/johnsonlab" in line
    ]
    tensorstore_root = repo / "build/macos-arm64-release/_deps/tensorstore-src"
    return {
        "captured_at": datetime.now(timezone.utc).isoformat(),
        "crimson_commit": capture(["git", "rev-parse", "HEAD"], repo),
        "tensorstore_commit": capture(
            ["git", "rev-parse", "HEAD"], tensorstore_root
        ),
        "platform": platform.platform(),
        "macos": capture(["sw_vers"]),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "network_label": args.network_label,
        "mount": mount_lines,
        "binary": str(args.binary.resolve()),
        "fixture_root": str(args.root.resolve()),
        "cache_bytes": args.cache_bytes,
        "mode": args.mode,
    }


def main() -> int:
    args = parse_args()
    repo = Path(__file__).resolve().parents[1]
    binary = args.binary if args.binary.is_absolute() else repo / args.binary
    root = args.root.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    if not binary.is_file():
        raise RuntimeError(f"Benchmark binary does not exist: {binary}")

    fixtures = {
        (repetition, layout): discover_fixture(root, repetition, layout)
        for repetition in args.repetitions
        for layout in LAYOUT_PATTERNS
    }
    plan = execution_plan(args.repetitions, args.cache_bytes)
    (output_dir / "environment.json").write_text(
        json.dumps(environment(repo, args), indent=2, sort_keys=True) + "\n"
    )
    (output_dir / "execution_plan.json").write_text(
        json.dumps(
            [
                {
                    "order": index,
                    "repetition": repetition,
                    "cache_bytes": cache_bytes,
                    "layout": layout,
                }
                for index, (repetition, cache_bytes, layout) in enumerate(plan)
            ],
            indent=2,
        )
        + "\n"
    )

    results: list[dict[str, Any]] = []
    for repetition, cache_bytes, layout in plan:
        output_path = output_dir / (
            f"repetition_{repetition}__cache_{cache_bytes}__{layout}__{args.mode}.json"
        )
        existing = None
        if not args.no_resume:
            existing = load_existing(
                output_path, layout, repetition, cache_bytes, args.mode
            )
        if existing is not None:
            print(f"[resume] {output_path.name}", flush=True)
            results.append(existing)
            continue
        store, palette_result = fixtures[(repetition, layout)]
        results.append(
            run_process(
                binary,
                store,
                palette_result,
                cache_bytes,
                layout,
                repetition,
                args.mode,
                output_path,
            )
        )

    rows = flatten_results(results)
    assert_equivalent_values(rows)
    write_csv(output_dir / "workloads.csv", rows)
    write_csv(output_dir / "initialization.csv", flatten_initialization(results))
    summary = aggregate(results, rows)
    combined = {
        "schema_id": "crimson.canonical_detection_storage_matrix",
        "schema_version": 1,
        "process_count": len(results),
        "value_equivalence": "pass",
        "summary": summary,
        "results": results,
    }
    (output_dir / "results.json").write_text(
        json.dumps(combined, indent=2, sort_keys=True) + "\n"
    )
    (output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n"
    )
    print(
        f"[complete] processes={len(results)} "
        f"all_gates_passed={summary['all_gates_passed']} output={output_dir}",
        flush=True,
    )
    return 0 if summary["all_gates_passed"] else 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as exception:
        print(f"error: {exception}", file=sys.stderr)
        raise SystemExit(1)
