#!/usr/bin/env python3
"""Run the frozen Crimson canonical-detection prefetch matrix."""

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


MIB = 1024 * 1024
SOURCE_FPS = 700
PRODUCTION_CACHE_BYTES = 64 * MIB
FROZEN_GATES = {
    "post_warmup_deadline_miss_rate_max": 0.01,
    "cancellation_p95_ms_max": 250.0,
    "post_cancel_file_bytes_p95_max": 1 * MIB,
    "stale_publications_max": 0,
    "peak_rss_bytes_max": 768 * MIB,
    "minimum_peak_concurrent_field_reads": 2,
    "offset_read_calls": 1,
}
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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run 75 hybrid cache/read-ahead trials plus five fresh regular "
            "128 MiB/no-prefetch anchors."
        )
    )
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument(
        "--repetitions", nargs="+", default=[f"{index:03d}" for index in range(5)]
    )
    parser.add_argument("--cache-mib", nargs="+", type=int, default=[0, 16, 32, 64, 128])
    parser.add_argument(
        "--read-ahead-seconds", nargs="+", type=float, default=[0.0, 0.5, 1.0]
    )
    parser.add_argument("--playback-frames", type=int, default=3500)
    parser.add_argument(
        "--network-label", default="mounted_smb_os_cache_uncontrolled"
    )
    parser.add_argument("--no-anchor", action="store_true")
    parser.add_argument("--no-resume", action="store_true")
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


def read_ahead_frames(seconds: float) -> int:
    frames = seconds * SOURCE_FPS
    rounded = round(frames)
    if not math.isclose(frames, rounded, abs_tol=1e-9):
        raise RuntimeError(f"Read-ahead {seconds} seconds is not frame-aligned")
    return rounded


def validate_result(
    result: dict[str, Any],
    layout: str,
    repetition: str,
    cache_bytes: int,
    lookahead_frames: int,
    playback_frames: int,
) -> None:
    expected = {
        "schema_id": "crimson.canonical_detection_storage_benchmark",
        "status": "pass",
        "layout": layout,
        "repetition": repetition,
        "cache_bytes": cache_bytes,
        "mode": "prefetch",
        "offset_read_calls_final": 1,
        "production_cache_bytes": PRODUCTION_CACHE_BYTES,
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
    prefetch = result.get("prefetch", {})
    config = prefetch.get("config", {})
    if config.get("read_ahead_frames") != lookahead_frames:
        raise RuntimeError("Read-ahead configuration mismatch")
    if config.get("playback_frames") != playback_frames:
        raise RuntimeError("Playback frame count mismatch")
    if prefetch.get("frozen_gates") != FROZEN_GATES:
        raise RuntimeError("Benchmark and runner frozen gates differ")
    expected_order = (
        ["reverse", "forward", "random_seek"]
        if int(repetition) % 2
        else ["forward", "reverse", "random_seek"]
    )
    if prefetch.get("phase_order") != expected_order:
        raise RuntimeError("Playback phase order was not balanced by repetition")
    for direction in ("forward", "reverse"):
        phase = prefetch.get(direction, {})
        if phase.get("offset_read_calls_after") != 1:
            raise RuntimeError(f"{direction} reread frame_row_offsets")
        if phase.get("failed_reads") != 0 or phase.get("cache_rejections") != 0:
            raise RuntimeError(f"{direction} had failed or rejected reads")
    seek = prefetch.get("random_seek_cancellation", {})
    if seek.get("offset_read_calls_after") != 1 or seek.get("failed_reads") != 0:
        raise RuntimeError("Seek phase violated the retained-offset/read contract")


def load_existing(
    path: Path,
    layout: str,
    repetition: str,
    cache_bytes: int,
    lookahead_frames: int,
    playback_frames: int,
) -> dict[str, Any] | None:
    if not path.is_file():
        return None
    try:
        result = json.loads(path.read_text())
        validate_result(
            result,
            layout,
            repetition,
            cache_bytes,
            lookahead_frames,
            playback_frames,
        )
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
    lookahead_frames: int,
    playback_frames: int,
    output_path: Path,
) -> dict[str, Any]:
    command = [
        str(binary),
        str(store),
        str(cache_bytes),
        layout,
        repetition,
        str(palette_result),
        "prefetch",
        str(lookahead_frames),
        str(playback_frames),
    ]
    print("[run] " + " ".join(command), flush=True)
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
    validate_result(
        result,
        layout,
        repetition,
        cache_bytes,
        lookahead_frames,
        playback_frames,
    )
    output_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return result


def execution_plan(
    repetitions: list[str], cache_bytes: list[int], lookahead_frames: list[int], anchor: bool
) -> list[tuple[str, str, int, int]]:
    plan: list[tuple[str, str, int, int]] = []
    base = [
        ("hybrid", cache, lookahead)
        for cache in cache_bytes
        for lookahead in lookahead_frames
    ]
    for repetition_index, repetition in enumerate(repetitions):
        offset = (repetition_index * 3) % len(base)
        conditions = base[offset:] + base[:offset]
        if repetition_index % 2:
            conditions.reverse()
        if anchor:
            anchor_position = (repetition_index * 5) % (len(conditions) + 1)
            conditions.insert(anchor_position, ("regular", 128 * MIB, 0))
        plan.extend((repetition, *condition) for condition in conditions)
    return plan


def phase_rows(results: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    repetition_order: dict[str, int] = defaultdict(int)
    for process_order, result in enumerate(results):
        order_within_repetition = repetition_order[result["repetition"]]
        repetition_order[result["repetition"]] += 1
        prefetch = result["prefetch"]
        config = prefetch["config"]
        phase_order = prefetch["phase_order"]
        for direction in ("forward", "reverse"):
            phase = prefetch[direction]
            metrics = phase["metrics"]
            scheduler = phase["scheduler"]
            cache = phase["presentation_cache"]
            rows.append(
                {
                    "process_order": process_order,
                    "repetition_order": order_within_repetition,
                    "repetition": result["repetition"],
                    "layout": result["layout"],
                    "cache_bytes": result["cache_bytes"],
                    "cache_mib": result["cache_bytes"] / MIB,
                    "read_ahead_frames": config["read_ahead_frames"],
                    "read_ahead_seconds": config["read_ahead_seconds"],
                    "direction": direction,
                    "direction_order_index": phase_order.index(direction),
                    "deadline_miss_rate": phase["deadline_miss_rate"],
                    "post_warmup_deadline_miss_rate": phase[
                        "post_warmup_deadline_miss_rate"
                    ],
                    "presentation_misses": phase["presentation_misses"],
                    "page_read_p95_ms": phase["page_read_p95_ms"],
                    "queue_delay_p95_ms": phase["queue_delay_p95_ms"],
                    "late_completion_p95_ms": phase["late_completion_p95_ms"],
                    "file_reads": metrics["file_reads"],
                    "file_bytes": metrics["file_bytes"],
                    "tensorstore_cache_hits": metrics["cache_hits"],
                    "tensorstore_cache_misses": metrics["cache_misses"],
                    "tensorstore_cache_evictions": metrics["cache_evictions"],
                    "peak_concurrent_field_reads": phase[
                        "peak_concurrent_field_reads"
                    ],
                    "scheduler_promotions": scheduler["promotions"],
                    "scheduler_cancellations": scheduler["cancelled_requests"],
                    "speculative_pages": phase["speculative_pages"],
                    "useful_speculative_pages": phase["useful_speculative_pages"],
                    "useful_speculative_ratio": phase["useful_speculative_ratio"],
                    "presentation_cache_evictions": cache["evictions"],
                    "presentation_cache_peak_bytes": cache["peak_cpu_bytes"],
                    "peak_rss_bytes": phase["peak_rss_bytes"],
                }
            )
    return rows


def initialization_rows(results: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    repetition_order: dict[str, int] = defaultdict(int)
    for process_order, result in enumerate(results):
        order_within_repetition = repetition_order[result["repetition"]]
        repetition_order[result["repetition"]] += 1
        offsets = result["initialization"]["frame_row_offsets"]
        config = result["prefetch"]["config"]
        rows.append(
            {
                "process_order": process_order,
                "repetition_order": order_within_repetition,
                "repetition": result["repetition"],
                "layout": result["layout"],
                "cache_bytes": result["cache_bytes"],
                "read_ahead_frames": config["read_ahead_frames"],
                "offset_read_ms": offsets["read_ms"],
                "offset_file_reads": offsets["metrics"]["file_reads"],
                "offset_file_bytes": offsets["metrics"]["file_bytes"],
                "offset_retained_bytes": offsets["retained_bytes"],
                "peak_rss_bytes_after_offsets": result["initialization"][
                    "peak_rss_bytes_after_offsets"
                ],
            }
        )
    return rows


def seek_rows(results: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    repetition_order: dict[str, int] = defaultdict(int)
    for process_order, result in enumerate(results):
        order_within_repetition = repetition_order[result["repetition"]]
        repetition_order[result["repetition"]] += 1
        seek = result["prefetch"]["random_seek_cancellation"]
        config = result["prefetch"]["config"]
        rows.append(
            {
                "process_order": process_order,
                "repetition_order": order_within_repetition,
                "repetition": result["repetition"],
                "layout": result["layout"],
                "cache_bytes": result["cache_bytes"],
                "cache_mib": result["cache_bytes"] / MIB,
                "read_ahead_frames": config["read_ahead_frames"],
                "read_ahead_seconds": config["read_ahead_seconds"],
                "seek_count": seek["seek_count"],
                "cancellation_p95_ms": seek["cancellation_p95_ms"],
                "post_cancel_file_bytes_p95": seek[
                    "post_cancel_file_bytes_p95"
                ],
                "post_cancel_file_bytes_total": seek[
                    "post_cancel_file_bytes_total"
                ],
                "discarded_after_read": seek["discarded_after_read"],
                "stale_publications": seek["stale_publications"],
                "peak_rss_bytes": seek["peak_rss_bytes"],
            }
        )
    return rows


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        raise RuntimeError(f"No rows for {path.name}")
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def aggregate(
    results: list[dict[str, Any]],
    phases: list[dict[str, Any]],
    seeks: list[dict[str, Any]],
    initializations: list[dict[str, Any]],
) -> dict[str, Any]:
    groups: dict[str, Any] = {}
    gates: list[dict[str, Any]] = []
    phase_groups: dict[tuple[str, int, int, str], list[dict[str, Any]]] = defaultdict(list)
    seek_groups: dict[tuple[str, int, int], list[dict[str, Any]]] = defaultdict(list)
    init_groups: dict[tuple[str, int, int], list[dict[str, Any]]] = defaultdict(list)
    for row in phases:
        phase_groups[
            (
                row["layout"],
                row["cache_bytes"],
                row["read_ahead_frames"],
                row["direction"],
            )
        ].append(row)
    for row in seeks:
        seek_groups[
            (row["layout"], row["cache_bytes"], row["read_ahead_frames"])
        ].append(row)
    for row in initializations:
        init_groups[
            (row["layout"], row["cache_bytes"], row["read_ahead_frames"])
        ].append(row)

    for key, rows in sorted(phase_groups.items()):
        layout, cache_bytes, lookahead, direction = key
        name = f"{layout}/cache_{cache_bytes}/ahead_{lookahead}/{direction}"
        groups[name] = {
            "post_warmup_deadline_miss_rate": describe(
                [float(row["post_warmup_deadline_miss_rate"]) for row in rows]
            ),
            "page_read_p95_ms": describe(
                [float(row["page_read_p95_ms"]) for row in rows]
            ),
            "queue_delay_p95_ms": describe(
                [float(row["queue_delay_p95_ms"]) for row in rows]
            ),
            "file_bytes": describe([float(row["file_bytes"]) for row in rows]),
            "tensorstore_cache_evictions": describe(
                [float(row["tensorstore_cache_evictions"]) for row in rows]
            ),
            "presentation_cache_peak_bytes": describe(
                [float(row["presentation_cache_peak_bytes"]) for row in rows]
            ),
            "useful_speculative_ratio": describe(
                [float(row["useful_speculative_ratio"]) for row in rows]
            ),
            "peak_rss_bytes": describe(
                [float(row["peak_rss_bytes"]) for row in rows]
            ),
        }
        if layout == "hybrid":
            measured = percentile(
                [float(row["post_warmup_deadline_miss_rate"]) for row in rows],
                0.95,
            )
            gates.append(
                {
                    "name": "post_warmup_deadline_miss_rate",
                    "cache_bytes": cache_bytes,
                    "read_ahead_frames": lookahead,
                    "direction": direction,
                    "measured": measured,
                    "limit": FROZEN_GATES[
                        "post_warmup_deadline_miss_rate_max"
                    ],
                    "passed": measured
                    <= FROZEN_GATES["post_warmup_deadline_miss_rate_max"],
                }
            )
            minimum_concurrency = min(
                int(row["peak_concurrent_field_reads"]) for row in rows
            )
            gates.append(
                {
                    "name": "concurrent_ui_field_reads",
                    "cache_bytes": cache_bytes,
                    "read_ahead_frames": lookahead,
                    "direction": direction,
                    "measured": minimum_concurrency,
                    "limit": FROZEN_GATES[
                        "minimum_peak_concurrent_field_reads"
                    ],
                    "passed": minimum_concurrency
                    >= FROZEN_GATES["minimum_peak_concurrent_field_reads"],
                }
            )

    for key, rows in sorted(seek_groups.items()):
        layout, cache_bytes, lookahead = key
        if layout != "hybrid":
            continue
        cancellation = percentile(
            [float(row["cancellation_p95_ms"]) for row in rows], 0.95
        )
        wasted = percentile(
            [float(row["post_cancel_file_bytes_p95"]) for row in rows], 0.95
        )
        stale = sum(int(row["stale_publications"]) for row in rows)
        peak_rss = percentile([float(row["peak_rss_bytes"]) for row in rows], 0.95)
        gates.extend(
            [
                {
                    "name": "cancellation_latency",
                    "cache_bytes": cache_bytes,
                    "read_ahead_frames": lookahead,
                    "measured": cancellation,
                    "limit": FROZEN_GATES["cancellation_p95_ms_max"],
                    "passed": cancellation <= FROZEN_GATES["cancellation_p95_ms_max"],
                },
                {
                    "name": "post_cancel_file_bytes",
                    "cache_bytes": cache_bytes,
                    "read_ahead_frames": lookahead,
                    "measured": wasted,
                    "limit": FROZEN_GATES["post_cancel_file_bytes_p95_max"],
                    "passed": wasted
                    <= FROZEN_GATES["post_cancel_file_bytes_p95_max"],
                },
                {
                    "name": "stale_publications",
                    "cache_bytes": cache_bytes,
                    "read_ahead_frames": lookahead,
                    "measured": stale,
                    "limit": FROZEN_GATES["stale_publications_max"],
                    "passed": stale <= FROZEN_GATES["stale_publications_max"],
                },
                {
                    "name": "peak_rss",
                    "cache_bytes": cache_bytes,
                    "read_ahead_frames": lookahead,
                    "measured": peak_rss,
                    "limit": FROZEN_GATES["peak_rss_bytes_max"],
                    "passed": peak_rss <= FROZEN_GATES["peak_rss_bytes_max"],
                },
            ]
        )

    offset_initialization: dict[str, Any] = {}
    for key, rows in sorted(init_groups.items()):
        layout, cache_bytes, lookahead = key
        name = f"{layout}/cache_{cache_bytes}/ahead_{lookahead}"
        offset_initialization[name] = {
            "read_ms": describe([float(row["offset_read_ms"]) for row in rows]),
            "file_bytes": describe(
                [float(row["offset_file_bytes"]) for row in rows]
            ),
            "retained_bytes": describe(
                [float(row["offset_retained_bytes"]) for row in rows]
            ),
        }

    process_first_offsets = [
        row for row in initializations if row["repetition_order"] == 0
    ]
    hybrid_offsets = [row for row in initializations if row["layout"] == "hybrid"]
    regular_offsets = [row for row in initializations if row["layout"] == "regular"]
    offset_context = {
        "process_first_os_cache_uncontrolled": describe(
            [float(row["offset_read_ms"]) for row in process_first_offsets]
        ),
        "hybrid_process_first_os_cache_uncontrolled": describe(
            [
                float(row["offset_read_ms"])
                for row in process_first_offsets
                if row["layout"] == "hybrid"
            ]
        ),
        "hybrid_all_processes_mixes_filesystem_warmth": describe(
            [float(row["offset_read_ms"]) for row in hybrid_offsets]
        ),
        "regular_anchor_distinct_store": (
            describe([float(row["offset_read_ms"]) for row in regular_offsets])
            if regular_offsets
            else None
        ),
    }

    knees: dict[str, Any] = {}
    hybrid_phase_first = [
        row
        for row in phases
        if row["layout"] == "hybrid" and row["direction_order_index"] == 0
    ]
    for lookahead in sorted(
        {int(row["read_ahead_frames"]) for row in hybrid_phase_first}
    ):
        candidates: list[dict[str, float | int]] = []
        for cache_bytes in sorted(
            {int(row["cache_bytes"]) for row in hybrid_phase_first}
        ):
            rows = [
                row
                for row in hybrid_phase_first
                if row["read_ahead_frames"] == lookahead
                and row["cache_bytes"] == cache_bytes
            ]
            candidates.append(
                {
                    "cache_bytes": cache_bytes,
                    "miss_rate": median(
                        float(row["post_warmup_deadline_miss_rate"]) for row in rows
                    ),
                    "file_bytes": median(float(row["file_bytes"]) for row in rows),
                }
            )
        passing = [
            item
            for item in candidates
            if item["miss_rate"]
            <= FROZEN_GATES["post_warmup_deadline_miss_rate_max"]
        ]
        minimum_bytes = min((float(item["file_bytes"]) for item in passing), default=math.inf)
        knee = next(
            (
                item
                for item in passing
                if float(item["file_bytes"]) <= minimum_bytes * 1.10
            ),
            None,
        )
        knees[str(lookahead)] = {
            "rule": (
                "smallest passing cache within 10 percent of minimum file bytes "
                "during the first playback phase"
            ),
            "candidate": knee,
            "observations": candidates,
        }

    return {
        "frozen_gates": FROZEN_GATES,
        "production_tensorstore_cache_bytes": PRODUCTION_CACHE_BYTES,
        "groups": groups,
        "offset_initialization_separate_subtest": offset_initialization,
        "offset_initialization_context": offset_context,
        "cache_knee_candidates": knees,
        "gates": gates,
        "all_prefetch_gates_passed": all(item["passed"] for item in gates),
        "promotion_status": "blocked_pending_full_analysis_fixture",
        "process_count": len(results),
    }


def environment(repo: Path, args: argparse.Namespace) -> dict[str, Any]:
    mount_lines = [
        line for line in capture(["mount"]).splitlines() if "/Volumes/johnsonlab" in line
    ]
    tensorstore_root = repo / "build/macos-arm64-release/_deps/tensorstore-src"
    return {
        "captured_at": datetime.now(timezone.utc).isoformat(),
        "crimson_commit": capture(["git", "rev-parse", "HEAD"], repo),
        "tensorstore_commit": capture(["git", "rev-parse", "HEAD"], tensorstore_root),
        "platform": platform.platform(),
        "macos": capture(["sw_vers"]),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "network_label": args.network_label,
        "mount": mount_lines,
        "binary": str(args.binary.resolve()),
        "fixture_root": str(args.root.resolve()),
        "cache_mib": args.cache_mib,
        "read_ahead_seconds": args.read_ahead_seconds,
        "playback_frames": args.playback_frames,
        "production_tensorstore_cache_bytes": PRODUCTION_CACHE_BYTES,
        "frozen_gates": FROZEN_GATES,
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
    cache_bytes = [value * MIB for value in args.cache_mib]
    lookahead = [read_ahead_frames(value) for value in args.read_ahead_seconds]
    fixtures = {
        (repetition, layout): discover_fixture(root, repetition, layout)
        for repetition in args.repetitions
        for layout in LAYOUT_PATTERNS
    }
    plan = execution_plan(
        args.repetitions, cache_bytes, lookahead, anchor=not args.no_anchor
    )
    (output_dir / "environment.json").write_text(
        json.dumps(environment(repo, args), indent=2, sort_keys=True) + "\n"
    )
    (output_dir / "execution_plan.json").write_text(
        json.dumps(
            [
                {
                    "order": index,
                    "repetition": repetition,
                    "layout": layout,
                    "cache_bytes": cache,
                    "read_ahead_frames": ahead,
                }
                for index, (repetition, layout, cache, ahead) in enumerate(plan)
            ],
            indent=2,
        )
        + "\n"
    )

    results: list[dict[str, Any]] = []
    for repetition, layout, cache, ahead in plan:
        output_path = output_dir / (
            f"repetition_{repetition}__{layout}__cache_{cache}__ahead_{ahead}.json"
        )
        existing = None
        if not args.no_resume:
            existing = load_existing(
                output_path,
                layout,
                repetition,
                cache,
                ahead,
                args.playback_frames,
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
                cache,
                layout,
                repetition,
                ahead,
                args.playback_frames,
                output_path,
            )
        )

    phases = phase_rows(results)
    initializations = initialization_rows(results)
    seeks = seek_rows(results)
    write_csv(output_dir / "prefetch.csv", phases)
    write_csv(output_dir / "offset_initialization.csv", initializations)
    write_csv(output_dir / "seek_cancellation.csv", seeks)
    summary = aggregate(results, phases, seeks, initializations)
    (output_dir / "results.json").write_text(
        json.dumps(
            {
                "schema_id": "crimson.canonical_detection_prefetch_matrix",
                "schema_version": 1,
                "summary": summary,
                "results": results,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n"
    )
    (output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n"
    )
    print(
        f"[complete] processes={len(results)} "
        f"all_prefetch_gates_passed={summary['all_prefetch_gates_passed']} "
        f"output={output_dir}",
        flush=True,
    )
    return 0 if summary["all_prefetch_gates_passed"] else 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as exception:
        print(f"error: {exception}", file=sys.stderr)
        raise SystemExit(1)
