#!/usr/bin/env python3
"""Run and reduce the frozen Phase 5O.4 full-duration Stage 1 matrix."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import platform
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


DEFAULT_FIXTURE_ROOT = Path(
    "/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/"
    "canonical_detection_storage/full_analysis/sleepyfish_cam2010095_v1"
)
DEFAULT_VIDEO = Path(
    "/Volumes/johnsonlab/jeremy/recordings/"
    "sleepyfish_2026_05_05_17_45_30_cam2010095/cams/"
    "Cam2010095_sleepyfish_2026_05_05_17_45_30_cam2010095.mp4"
)
DEFAULT_RUN = "crimson_storage_fixture_sleepyfish_cam2010095_v1"
DEFAULT_BINARY = Path(
    "build/macos-arm64-release/"
    "canonical_detection_full_archive_stage1_benchmark"
)

EXPECTED_HASHES = {
    "regular_manifest.json": (
        "d84f2c982ec8983a5b437c2d539fe4a3966bcb36866f28fb9daa6c1724fef9f1"
    ),
    "hybrid_manifest.json": (
        "51e4dd1ad35c3eb6481a15604e537dd756f7c4b29534f0081e5501a3c26bae74"
    ),
    "pair_manifest.json": (
        "25e49003d63f74e5c7f1aa940aa77acee8df0153476847afeb99b98238574432"
    ),
    "publication_receipt.json": (
        "22763ffce084446cfa797b567ebd7bb66d682e7e1f1f031e7af781c575fdcd1d"
    ),
}

LIMITS = {
    "ready_ms": 180_000.0,
    "hybrid_ready_regression_ratio": 1.10,
    "hybrid_ready_regression_ms": 5_000.0,
    "first_overlay_p95_ms": 1_000.0,
    "hybrid_first_overlay_regression_ms": 250.0,
    "post_warmup_deadline_miss_rate": 0.01,
    "seek_cancellation_p95_ms": 250.0,
    "post_cancel_bytes_per_seek_p95": 1_048_576.0,
    "peak_rss_bytes": 2 * 1024 * 1024 * 1024,
    "hybrid_peak_rss_regression_bytes": 128 * 1024 * 1024,
    "hybrid_total_file_bytes_ratio": 1.05,
    "hybrid_traversal_file_bytes_ratio": 0.25,
    "shutdown_ms": 2_000.0,
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def command_output(command: list[str], cwd: Path | None = None) -> str:
    try:
        completed = subprocess.run(
            command,
            cwd=cwd,
            check=False,
            capture_output=True,
            text=True,
            timeout=20,
        )
        return (completed.stdout or completed.stderr).strip()
    except (OSError, subprocess.TimeoutExpired) as error:
        return f"unavailable: {error}"


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        raise ValueError("Cannot reduce an empty sample")
    ordered = sorted(values)
    index = max(0, math.ceil(fraction * len(ordered)) - 1)
    return ordered[index]


def nested(document: dict[str, Any], *keys: str) -> Any:
    current: Any = document
    for key in keys:
        current = current[key]
    return current


def trial_metrics(document: dict[str, Any]) -> dict[str, Any]:
    forward = nested(document, "traversal", "forward")
    reverse = nested(document, "traversal", "reverse")
    post_warmup_pages = (
        forward["post_warmup_pages"] + reverse["post_warmup_pages"]
    )
    post_warmup_misses = (
        forward["post_warmup_misses"] + reverse["post_warmup_misses"]
    )
    return {
        "ready_ms": float(
            nested(document, "first_presentations", "ready_elapsed_ms")
        ),
        "first_overlay_ms": float(
            nested(
                document,
                "first_presentations",
                "first_detection_overlay_after_archive_ms",
            )
        ),
        "first_page_request_to_publish_ms": float(
            nested(
                document,
                "first_presentations",
                "first_detection_scheduling",
                "request_to_publish_ms",
            )
        ),
        "first_page_repository_read_decode_ms": float(
            nested(
                document,
                "first_presentations",
                "first_detection_scheduling",
                "repository_read_decode_max_ms",
            )
        ),
        "first_page_inferred_queue_wait_ms": float(
            nested(
                document,
                "first_presentations",
                "first_detection_scheduling",
                "inferred_queue_wait_ms",
            )
        ),
        "offset_ms": float(nested(document, "canonical_open", "offset_ms")),
        "peak_rss_bytes": int(document["peak_rss_bytes"]),
        "total_file_bytes": int(
            nested(document, "physical_total", "file_bytes")
        ),
        "traversal_file_bytes": int(
            nested(forward, "physical", "file_bytes")
            + nested(reverse, "physical", "file_bytes")
        ),
        "post_warmup_pages": int(post_warmup_pages),
        "post_warmup_misses": int(post_warmup_misses),
        "post_warmup_miss_rate": (
            float(post_warmup_misses) / float(post_warmup_pages)
            if post_warmup_pages
            else 0.0
        ),
        "seek_settle_ms": float(nested(document, "seek_burst", "settle_ms")),
        "post_cancel_bytes_per_seek": float(
            nested(
                document,
                "seek_burst",
                "post_cancel_file_bytes_per_superseded_seek",
            )
        ),
        "shutdown_ms": float(document["shutdown_ms"]),
        "forward_digest": forward["logical_digest_fnv1a64"],
        "reverse_digest": reverse["logical_digest_fnv1a64"],
        "peak_concurrent_fields": min(
            int(forward["peak_concurrent_fields"]),
            int(reverse["peak_concurrent_fields"]),
        ),
        "failed_pages": int(forward["failed_pages"] + reverse["failed_pages"]),
        "stale_publications": int(
            nested(document, "seek_burst", "stale_publications")
        ),
        "offset_reads": int(nested(document, "canonical_open", "offset_reads")),
        "fallback_metadata_reads": int(
            nested(document, "canonical_open", "fallback_metadata_reads")
        ),
        "fallback_dtype_opens": int(
            nested(document, "canonical_open", "fallback_dtype_opens")
        ),
        "scheduler_failed": int(nested(document, "scheduler", "failed")),
        "scheduler_work_exceptions": int(
            nested(document, "scheduler", "work_exceptions")
        ),
    }


def summarize_layout(trials: list[dict[str, Any]]) -> dict[str, Any]:
    metrics = [trial["metrics"] for trial in trials]
    numeric_fields = [
        "ready_ms",
        "first_overlay_ms",
        "first_page_request_to_publish_ms",
        "first_page_repository_read_decode_ms",
        "first_page_inferred_queue_wait_ms",
        "offset_ms",
        "peak_rss_bytes",
        "total_file_bytes",
        "traversal_file_bytes",
        "post_warmup_miss_rate",
        "seek_settle_ms",
        "post_cancel_bytes_per_seek",
        "shutdown_ms",
    ]
    summary: dict[str, Any] = {"trial_count": len(trials)}
    for field in numeric_fields:
        values = [float(metric[field]) for metric in metrics]
        summary[field] = {
            "values": values,
            "median": statistics.median(values),
            "p95_nearest_rank": percentile(values, 0.95),
            "maximum": max(values),
        }
    summary["post_warmup_misses"] = sum(
        metric["post_warmup_misses"] for metric in metrics
    )
    summary["post_warmup_pages"] = sum(
        metric["post_warmup_pages"] for metric in metrics
    )
    summary["post_warmup_cross_run_rate"] = (
        summary["post_warmup_misses"] / summary["post_warmup_pages"]
        if summary["post_warmup_pages"]
        else 0.0
    )
    return summary


def gate(name: str, passed: bool, observed: Any, limit: Any) -> dict[str, Any]:
    return {
        "name": name,
        "pass": bool(passed),
        "observed": observed,
        "limit": limit,
    }


def reduce_trials(trials: list[dict[str, Any]]) -> dict[str, Any]:
    complete = [trial for trial in trials if trial.get("pass")]
    by_layout = {
        layout: [trial for trial in complete if trial["layout"] == layout]
        for layout in ("regular", "hybrid")
    }
    if any(len(value) != 5 for value in by_layout.values()):
        return {
            "status": "incomplete",
            "full_duration_gate_satisfied": False,
            "profile_promoted": False,
            "completed_trials": {key: len(value) for key, value in by_layout.items()},
            "trial_failures": [
                {
                    "layout": trial["layout"],
                    "repetition": trial["repetition"],
                    "error": trial.get("error", "trial failed"),
                }
                for trial in trials
                if not trial.get("pass")
            ],
        }

    summaries = {
        layout: summarize_layout(layout_trials)
        for layout, layout_trials in by_layout.items()
    }
    regular = summaries["regular"]
    hybrid = summaries["hybrid"]
    ready_regression_ms = (
        hybrid["ready_ms"]["median"] - regular["ready_ms"]["median"]
    )
    ready_ratio = (
        hybrid["ready_ms"]["median"] / regular["ready_ms"]["median"]
    )
    overlay_regression_ms = (
        hybrid["first_overlay_ms"]["p95_nearest_rank"]
        - regular["first_overlay_ms"]["p95_nearest_rank"]
    )
    rss_regression = (
        hybrid["peak_rss_bytes"]["median"]
        - regular["peak_rss_bytes"]["median"]
    )
    total_bytes_ratio = (
        hybrid["total_file_bytes"]["median"]
        / regular["total_file_bytes"]["median"]
    )
    traversal_bytes_ratio = (
        hybrid["traversal_file_bytes"]["median"]
        / regular["traversal_file_bytes"]["median"]
    )

    correctness_failures: list[str] = []
    forward_digests = set()
    reverse_digests = set()
    for trial in complete:
        metrics = trial["metrics"]
        forward_digests.add(metrics["forward_digest"])
        reverse_digests.add(metrics["reverse_digest"])
        checks = {
            "offset_reads": metrics["offset_reads"] == 1,
            "fallback_metadata_reads": metrics["fallback_metadata_reads"] == 0,
            "fallback_dtype_opens": metrics["fallback_dtype_opens"] == 0,
            "failed_pages": metrics["failed_pages"] == 0,
            "stale_publications": metrics["stale_publications"] == 0,
            "scheduler_failed": metrics["scheduler_failed"] == 0,
            "scheduler_work_exceptions": metrics["scheduler_work_exceptions"] == 0,
            "peak_concurrent_fields": metrics["peak_concurrent_fields"] >= 2,
        }
        for check, passed in checks.items():
            if not passed:
                correctness_failures.append(
                    f"{trial['layout']} repetition {trial['repetition']}: {check}"
                )
    if len(forward_digests) != 1:
        correctness_failures.append(
            "forward logical detection digests are not identical across trials"
        )
    if len(reverse_digests) != 1:
        correctness_failures.append(
            "reverse logical detection digests are not identical across trials"
        )

    gates = [
        gate(
            "correctness",
            not correctness_failures,
            correctness_failures,
            "no failures and one digest per traversal direction",
        ),
        gate(
            "every_ready_time",
            all(
                trial["metrics"]["ready_ms"] <= LIMITS["ready_ms"]
                for trial in complete
            ),
            max(trial["metrics"]["ready_ms"] for trial in complete),
            LIMITS["ready_ms"],
        ),
        gate(
            "hybrid_median_ready_regression",
            ready_ratio <= LIMITS["hybrid_ready_regression_ratio"]
            and ready_regression_ms <= LIMITS["hybrid_ready_regression_ms"],
            {"ratio": ready_ratio, "milliseconds": ready_regression_ms},
            {
                "ratio": LIMITS["hybrid_ready_regression_ratio"],
                "milliseconds": LIMITS["hybrid_ready_regression_ms"],
            },
        ),
        gate(
            "first_overlay_p95",
            max(
                regular["first_overlay_ms"]["p95_nearest_rank"],
                hybrid["first_overlay_ms"]["p95_nearest_rank"],
            )
            <= LIMITS["first_overlay_p95_ms"],
            {
                "regular": regular["first_overlay_ms"]["p95_nearest_rank"],
                "hybrid": hybrid["first_overlay_ms"]["p95_nearest_rank"],
            },
            LIMITS["first_overlay_p95_ms"],
        ),
        gate(
            "hybrid_first_overlay_p95_regression",
            overlay_regression_ms
            <= LIMITS["hybrid_first_overlay_regression_ms"],
            overlay_regression_ms,
            LIMITS["hybrid_first_overlay_regression_ms"],
        ),
        gate(
            "post_warmup_deadline_miss_rate",
            max(
                regular["post_warmup_cross_run_rate"],
                hybrid["post_warmup_cross_run_rate"],
            )
            <= LIMITS["post_warmup_deadline_miss_rate"],
            {
                "regular": regular["post_warmup_cross_run_rate"],
                "hybrid": hybrid["post_warmup_cross_run_rate"],
            },
            LIMITS["post_warmup_deadline_miss_rate"],
        ),
        gate(
            "seek_cancellation_p95",
            max(
                regular["seek_settle_ms"]["p95_nearest_rank"],
                hybrid["seek_settle_ms"]["p95_nearest_rank"],
            )
            <= LIMITS["seek_cancellation_p95_ms"],
            {
                "regular": regular["seek_settle_ms"]["p95_nearest_rank"],
                "hybrid": hybrid["seek_settle_ms"]["p95_nearest_rank"],
            },
            LIMITS["seek_cancellation_p95_ms"],
        ),
        gate(
            "post_cancel_bytes_per_seek_p95",
            max(
                regular["post_cancel_bytes_per_seek"]["p95_nearest_rank"],
                hybrid["post_cancel_bytes_per_seek"]["p95_nearest_rank"],
            )
            <= LIMITS["post_cancel_bytes_per_seek_p95"],
            {
                "regular": regular["post_cancel_bytes_per_seek"][
                    "p95_nearest_rank"
                ],
                "hybrid": hybrid["post_cancel_bytes_per_seek"][
                    "p95_nearest_rank"
                ],
            },
            LIMITS["post_cancel_bytes_per_seek_p95"],
        ),
        gate(
            "every_peak_rss",
            all(
                trial["metrics"]["peak_rss_bytes"] <= LIMITS["peak_rss_bytes"]
                for trial in complete
            ),
            max(trial["metrics"]["peak_rss_bytes"] for trial in complete),
            LIMITS["peak_rss_bytes"],
        ),
        gate(
            "hybrid_median_peak_rss_regression",
            rss_regression <= LIMITS["hybrid_peak_rss_regression_bytes"],
            rss_regression,
            LIMITS["hybrid_peak_rss_regression_bytes"],
        ),
        gate(
            "hybrid_total_file_bytes_ratio",
            total_bytes_ratio <= LIMITS["hybrid_total_file_bytes_ratio"],
            total_bytes_ratio,
            LIMITS["hybrid_total_file_bytes_ratio"],
        ),
        gate(
            "hybrid_detection_traversal_file_bytes_ratio",
            traversal_bytes_ratio
            <= LIMITS["hybrid_traversal_file_bytes_ratio"],
            traversal_bytes_ratio,
            LIMITS["hybrid_traversal_file_bytes_ratio"],
        ),
        gate(
            "every_shutdown",
            all(
                trial["metrics"]["shutdown_ms"] <= LIMITS["shutdown_ms"]
                for trial in complete
            ),
            max(trial["metrics"]["shutdown_ms"] for trial in complete),
            LIMITS["shutdown_ms"],
        ),
    ]
    passed = all(item["pass"] for item in gates)
    return {
        "status": "pass" if passed else "fail",
        "full_duration_gate_satisfied": passed,
        "hybrid_physical_profile_stage1_accepted": passed,
        "promotion_recommended": passed,
        "profile_promoted": False,
        "promotion_scope": "separate versioned storage-policy decision",
        "summaries": summaries,
        "comparisons": {
            "hybrid_ready_ratio": ready_ratio,
            "hybrid_ready_regression_ms": ready_regression_ms,
            "hybrid_first_overlay_p95_regression_ms": overlay_regression_ms,
            "hybrid_peak_rss_median_regression_bytes": rss_regression,
            "hybrid_total_file_bytes_ratio": total_bytes_ratio,
            "hybrid_detection_traversal_file_bytes_ratio": traversal_bytes_ratio,
        },
        "gates": gates,
    }


def validate_fixture(root: Path) -> dict[str, Any]:
    evidence: dict[str, Any] = {}
    for name, expected in EXPECTED_HASHES.items():
        path = root / name
        actual = sha256(path)
        if actual != expected:
            raise RuntimeError(
                f"Fixture evidence hash mismatch for {path}: {actual} != {expected}"
            )
        evidence[name] = {"path": str(path), "sha256": actual}
    for layout in ("regular", "hybrid"):
        root_metadata = root / f"{layout}.zarr" / "zarr.json"
        if not root_metadata.is_file():
            raise RuntimeError(f"Fixture archive is unavailable: {root_metadata}")
    return evidence


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture-root", type=Path, default=DEFAULT_FIXTURE_ROOT)
    parser.add_argument("--video", type=Path, default=DEFAULT_VIDEO)
    parser.add_argument("--detection-run", default=DEFAULT_RUN)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=float, default=300.0)
    parser.add_argument("--resume", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo = Path.cwd().resolve()
    binary = args.binary if args.binary.is_absolute() else repo / args.binary
    if not binary.is_file():
        raise RuntimeError(f"Benchmark binary is unavailable: {binary}")
    if not args.video.is_file():
        raise RuntimeError(f"Video is unavailable: {args.video}")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    fixture_evidence = validate_fixture(args.fixture_root)

    order = [
        (repetition, layout)
        for repetition in range(5)
        for layout in (
            ("regular", "hybrid")
            if repetition % 2 == 0
            else ("hybrid", "regular")
        )
    ]
    trials: list[dict[str, Any]] = []
    started = time.time()
    for position, (repetition, layout) in enumerate(order):
        stem = f"{position:02d}_repetition_{repetition}_{layout}"
        output = args.output_dir / f"{stem}.json"
        stdout_path = args.output_dir / f"{stem}.stdout.log"
        stderr_path = args.output_dir / f"{stem}.stderr.log"
        command = [
            str(binary),
            str(args.fixture_root / f"{layout}.zarr"),
            args.detection_run,
            str(args.video),
            layout,
            str(repetition),
            str(output),
        ]
        print(
            f"[Stage1Matrix] position={position + 1}/10 repetition={repetition} "
            f"layout={layout}",
            flush=True,
        )
        returncode = 0
        elapsed = 0.0
        timed_out = False
        if not (args.resume and output.is_file()):
            trial_started = time.monotonic()
            try:
                completed = subprocess.run(
                    command,
                    cwd=repo,
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout=args.timeout_seconds,
                )
                returncode = completed.returncode
                stdout_path.write_text(completed.stdout, encoding="utf-8")
                stderr_path.write_text(completed.stderr, encoding="utf-8")
            except subprocess.TimeoutExpired as error:
                timed_out = True
                returncode = 124
                stdout_path.write_text(error.stdout or "", encoding="utf-8")
                stderr_path.write_text(error.stderr or "", encoding="utf-8")
            elapsed = (time.monotonic() - trial_started) * 1000.0
        trial: dict[str, Any] = {
            "position": position,
            "repetition": repetition,
            "layout": layout,
            "command": command,
            "result_path": str(output),
            "stdout_path": str(stdout_path),
            "stderr_path": str(stderr_path),
            "subprocess_returncode": returncode,
            "subprocess_elapsed_ms": elapsed,
            "timed_out": timed_out,
            "pass": False,
        }
        if output.is_file():
            try:
                document = json.loads(output.read_text(encoding="utf-8"))
                trial["pass"] = bool(document.get("pass")) and returncode == 0
                if trial["pass"]:
                    trial["metrics"] = trial_metrics(document)
                else:
                    trial["error"] = document.get("error", "trial did not pass")
            except (OSError, ValueError, KeyError, TypeError) as error:
                trial["error"] = f"Could not reduce trial evidence: {error}"
        else:
            trial["error"] = "Trial produced no structured evidence"
        trials.append(trial)
        print(
            f"[Stage1Matrix] completed layout={layout} repetition={repetition} "
            f"pass={int(trial['pass'])} elapsed_ms={elapsed:.1f}",
            flush=True,
        )

    reduction = reduce_trials(trials)
    aggregate = {
        "schema_id": "crimson.canonical_detection_full_archive_stage1_matrix",
        "schema_version": 1,
        "classification": "full_duration_stage1",
        "created_unix_seconds": time.time(),
        "wall_elapsed_seconds": time.time() - started,
        "fixture_root": str(args.fixture_root),
        "fixture_evidence": fixture_evidence,
        "detection_run": args.detection_run,
        "video": str(args.video),
        "binary": str(binary),
        "balanced_order": [
            {"position": index, "repetition": repetition, "layout": layout}
            for index, (repetition, layout) in enumerate(order)
        ],
        "cache_policy": {
            "tensorstore_cache_bytes": 64 * 1024 * 1024,
            "speculative_time_read_ahead_frames": 0,
            "asynchronous_demand_lead_pages": 1,
            "page_frames": 70,
        },
        "environment": {
            "crimson_commit": command_output(["git", "rev-parse", "HEAD"], repo),
            "crimson_worktree": command_output(
                ["git", "status", "--short", "--untracked-files=no"], repo
            ),
            "platform": platform.platform(),
            "machine": platform.machine(),
            "macos": command_output(["sw_vers"]),
            "mount": command_output(["mount"]),
            "network": command_output(["scutil", "--nwi"]),
            "vpn": command_output(["scutil", "--nc", "list"]),
            "cache_condition": "process-first, OS/filesystem/server cache uncontrolled",
        },
        "limits": LIMITS,
        "trials": trials,
        "reduction": reduction,
    }
    aggregate_path = args.output_dir / "aggregate.json"
    aggregate_path.write_text(json.dumps(aggregate, indent=2) + "\n", encoding="utf-8")
    print(
        f"[Stage1Matrix] status={reduction['status']} evidence={aggregate_path}",
        flush=True,
    )
    return 0 if reduction["full_duration_gate_satisfied"] else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # Fail closed before or outside a trial.
        print(f"run_canonical_detection_full_archive_stage1: ERROR: {error}", file=sys.stderr)
        raise SystemExit(2)
