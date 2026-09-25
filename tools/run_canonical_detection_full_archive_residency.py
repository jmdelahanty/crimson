#!/usr/bin/env python3
"""Run the frozen Phase 5O.5 full-archive residency interference gate."""

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

MAINTAINED_PRODUCTS = (
    "canonical_detection",
    "keypoints",
    "subject_masks",
    "subject_shape",
    "eye_geometry",
    "motion",
    "eye_angles",
    "tail_kinematics",
    "crop_geometry",
)

LIMITS = {
    "ready_regression_ratio": 1.10,
    "ready_regression_ms": 5_000.0,
    "peak_rss_regression_bytes": 64 * 1024 * 1024,
    "product_regression_ratio": 1.10,
    "first_page_regression_ratio": 1.10,
    "first_page_regression_ms": 250.0,
    "current_frame_regression_ratio": 1.10,
    "current_frame_regression_ms": 250.0,
    "post_warmup_deadline_miss_rate": 0.01,
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


def nested(document: dict[str, Any], *keys: str) -> Any:
    current: Any = document
    for key in keys:
        current = current[key]
    return current


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        raise ValueError("Cannot reduce an empty sample")
    ordered = sorted(values)
    return ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]


def distribution(values: list[float]) -> dict[str, Any]:
    return {
        "values": values,
        "median": statistics.median(values),
        "p95_nearest_rank": percentile(values, 0.95),
        "maximum": max(values),
    }


def trial_metrics(document: dict[str, Any]) -> dict[str, Any]:
    forward = nested(document, "traversal", "forward")
    reverse = nested(document, "traversal", "reverse")
    post_warmup_pages = int(forward["post_warmup_pages"]) + int(
        reverse["post_warmup_pages"]
    )
    post_warmup_misses = int(forward["post_warmup_misses"]) + int(
        reverse["post_warmup_misses"]
    )
    residency = nested(document, "residency", "metrics")
    products = nested(document, "loading", "products")
    return {
        "required_products_ready_ms": float(
            nested(document, "loading", "required_products_ready_elapsed_ms")
        ),
        "loading_elapsed_ms": float(nested(document, "loading", "elapsed_ms")),
        "first_page_request_to_publish_ms": float(
            nested(
                document,
                "first_presentations",
                "first_detection_scheduling",
                "request_to_publish_ms",
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
        "current_frame_request_to_publish_ms": float(
            nested(document, "interference_probe", "request_to_publish_ms")
        ),
        "current_frame_inferred_queue_wait_ms": float(
            nested(
                document,
                "interference_probe",
                "inferred_queue_wait_lower_bound_ms",
            )
        ),
        "product_elapsed_ms": {
            product: float(products[product]["elapsed_ms"])
            for product in MAINTAINED_PRODUCTS
        },
        "peak_rss_bytes": int(document["peak_rss_bytes"]),
        "loading_peak_rss_bytes": int(nested(document, "loading", "peak_rss_bytes")),
        "total_file_bytes": int(nested(document, "physical_total", "file_bytes")),
        "post_warmup_pages": post_warmup_pages,
        "post_warmup_misses": post_warmup_misses,
        "post_warmup_miss_rate": (
            post_warmup_misses / post_warmup_pages if post_warmup_pages else 0.0
        ),
        "forward_digest": forward["logical_digest_fnv1a64"],
        "reverse_digest": reverse["logical_digest_fnv1a64"],
        "failed_pages": int(forward["failed_pages"]) + int(reverse["failed_pages"]),
        "stale_page_publications": int(
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
        "shutdown_ms": float(document["shutdown_ms"]),
        "residency_state": residency["state"],
        "residency_publications": int(residency["publications"]),
        "residency_stale_chunks": int(residency["stale_chunks"]),
        "residency_failed_chunks": int(residency["failed_chunks"]),
        "resident_retained_bytes": int(residency["retained_bytes"]),
        "resident_elapsed_ms": float(residency["elapsed_ms"]),
        "resident_ready_elapsed_ms": (
            float(nested(document, "residency", "ready_elapsed_ms"))
            if "ready_elapsed_ms" in document["residency"]
            else None
        ),
    }


def summarize(trials: list[dict[str, Any]]) -> dict[str, Any]:
    metrics = [trial["metrics"] for trial in trials]
    numeric = (
        "required_products_ready_ms",
        "loading_elapsed_ms",
        "first_page_request_to_publish_ms",
        "first_page_inferred_queue_wait_ms",
        "current_frame_request_to_publish_ms",
        "current_frame_inferred_queue_wait_ms",
        "peak_rss_bytes",
        "loading_peak_rss_bytes",
        "total_file_bytes",
        "post_warmup_miss_rate",
        "shutdown_ms",
        "resident_elapsed_ms",
    )
    result = {
        field: distribution([float(metric[field]) for metric in metrics])
        for field in numeric
    }
    result["products"] = {
        product: distribution(
            [float(metric["product_elapsed_ms"][product]) for metric in metrics]
        )
        for product in MAINTAINED_PRODUCTS
    }
    result["post_warmup_pages"] = sum(
        metric["post_warmup_pages"] for metric in metrics
    )
    result["post_warmup_misses"] = sum(
        metric["post_warmup_misses"] for metric in metrics
    )
    return result


def gate(name: str, passed: bool, observed: Any, limit: Any) -> dict[str, Any]:
    return {
        "name": name,
        "pass": bool(passed),
        "observed": observed,
        "limit": limit,
    }


def within_regression(
    baseline: float, candidate: float, ratio_limit: float, delta_limit: float
) -> tuple[bool, dict[str, float]]:
    ratio = candidate / baseline if baseline > 0.0 else (1.0 if candidate == 0.0 else math.inf)
    delta = candidate - baseline
    return ratio <= ratio_limit and delta <= delta_limit, {
        "paged": baseline,
        "resident": candidate,
        "ratio": ratio,
        "delta": delta,
    }


def reduce_trials(trials: list[dict[str, Any]]) -> dict[str, Any]:
    complete = [trial for trial in trials if trial.get("pass")]
    by_strategy = {
        strategy: [trial for trial in complete if trial["strategy"] == strategy]
        for strategy in ("paged", "resident")
    }
    if any(len(group) != 5 for group in by_strategy.values()):
        return {
            "status": "incomplete",
            "full_archive_residency_gate_satisfied": False,
            "production_residency_enabled": False,
            "completed_trials": {
                strategy: len(group) for strategy, group in by_strategy.items()
            },
            "trial_failures": [
                {
                    "strategy": trial["strategy"],
                    "repetition": trial["repetition"],
                    "error": trial.get("error", "trial failed"),
                }
                for trial in trials
                if not trial.get("pass")
            ],
        }

    summaries = {
        strategy: summarize(group) for strategy, group in by_strategy.items()
    }
    paged = summaries["paged"]
    resident = summaries["resident"]

    correctness_failures: list[str] = []
    forward_digests = set()
    reverse_digests = set()
    for trial in complete:
        strategy = trial["strategy"]
        metrics = trial["metrics"]
        forward_digests.add(metrics["forward_digest"])
        reverse_digests.add(metrics["reverse_digest"])
        checks = {
            "offset_reads": metrics["offset_reads"] == 1,
            "fallback_metadata_reads": metrics["fallback_metadata_reads"] == 0,
            "fallback_dtype_opens": metrics["fallback_dtype_opens"] == 0,
            "failed_pages": metrics["failed_pages"] == 0,
            "stale_page_publications": metrics["stale_page_publications"] == 0,
            "scheduler_failed": metrics["scheduler_failed"] == 0,
            "scheduler_work_exceptions": metrics["scheduler_work_exceptions"] == 0,
            "shutdown": metrics["shutdown_ms"] <= LIMITS["shutdown_ms"],
        }
        if strategy == "resident":
            checks.update(
                {
                    "residency_state": metrics["residency_state"] == "ready",
                    "residency_publications": metrics["residency_publications"] == 1,
                    "residency_stale_chunks": metrics["residency_stale_chunks"] == 0,
                    "residency_failed_chunks": metrics["residency_failed_chunks"] == 0,
                    "resident_retained_bytes": metrics["resident_retained_bytes"]
                    == 28_490_088,
                }
            )
        else:
            checks.update(
                {
                    "residency_state": metrics["residency_state"] == "disabled",
                    "residency_publications": metrics["residency_publications"] == 0,
                    "resident_retained_bytes": metrics["resident_retained_bytes"] == 0,
                }
            )
        for check, passed in checks.items():
            if not passed:
                correctness_failures.append(
                    f"{strategy} repetition {trial['repetition']}: {check}"
                )
    if len(forward_digests) != 1:
        correctness_failures.append("forward traversal digests differ")
    if len(reverse_digests) != 1:
        correctness_failures.append("reverse traversal digests differ")

    ready_pass, ready_observed = within_regression(
        paged["required_products_ready_ms"]["median"],
        resident["required_products_ready_ms"]["median"],
        LIMITS["ready_regression_ratio"],
        LIMITS["ready_regression_ms"],
    )
    first_page_pass, first_page_observed = within_regression(
        paged["first_page_request_to_publish_ms"]["median"],
        resident["first_page_request_to_publish_ms"]["median"],
        LIMITS["first_page_regression_ratio"],
        LIMITS["first_page_regression_ms"],
    )
    current_pass, current_observed = within_regression(
        paged["current_frame_request_to_publish_ms"]["median"],
        resident["current_frame_request_to_publish_ms"]["median"],
        LIMITS["current_frame_regression_ratio"],
        LIMITS["current_frame_regression_ms"],
    )
    queue_pass, queue_observed = within_regression(
        paged["current_frame_inferred_queue_wait_ms"]["median"],
        resident["current_frame_inferred_queue_wait_ms"]["median"],
        LIMITS["current_frame_regression_ratio"],
        LIMITS["current_frame_regression_ms"],
    )
    rss_delta = (
        resident["peak_rss_bytes"]["median"] - paged["peak_rss_bytes"]["median"]
    )
    product_regressions = {
        product: {
            "paged_ms": paged["products"][product]["median"],
            "resident_ms": resident["products"][product]["median"],
            "ratio": (
                resident["products"][product]["median"]
                / paged["products"][product]["median"]
            ),
        }
        for product in MAINTAINED_PRODUCTS
    }
    product_failures = {
        product: values
        for product, values in product_regressions.items()
        if values["ratio"] > LIMITS["product_regression_ratio"]
    }
    paged_miss_rate = (
        paged["post_warmup_misses"] / paged["post_warmup_pages"]
        if paged["post_warmup_pages"]
        else 0.0
    )
    resident_miss_rate = (
        resident["post_warmup_misses"] / resident["post_warmup_pages"]
        if resident["post_warmup_pages"]
        else 0.0
    )

    gates = [
        gate("correctness", not correctness_failures, correctness_failures, []),
        gate(
            "median_required_products_ready_regression",
            ready_pass,
            ready_observed,
            {
                "ratio": LIMITS["ready_regression_ratio"],
                "milliseconds": LIMITS["ready_regression_ms"],
            },
        ),
        gate(
            "median_first_page_regression",
            first_page_pass,
            first_page_observed,
            {
                "ratio": LIMITS["first_page_regression_ratio"],
                "milliseconds": LIMITS["first_page_regression_ms"],
            },
        ),
        gate(
            "median_current_frame_latency_regression",
            current_pass,
            current_observed,
            {
                "ratio": LIMITS["current_frame_regression_ratio"],
                "milliseconds": LIMITS["current_frame_regression_ms"],
            },
        ),
        gate(
            "median_current_frame_queue_wait_regression",
            queue_pass,
            queue_observed,
            {
                "ratio": LIMITS["current_frame_regression_ratio"],
                "milliseconds": LIMITS["current_frame_regression_ms"],
            },
        ),
        gate(
            "maintained_product_initialization_regression",
            not product_failures,
            product_failures,
            LIMITS["product_regression_ratio"],
        ),
        gate(
            "median_peak_rss_regression",
            rss_delta <= LIMITS["peak_rss_regression_bytes"],
            rss_delta,
            LIMITS["peak_rss_regression_bytes"],
        ),
        gate(
            "playback_deadline_miss_regression",
            resident_miss_rate <= paged_miss_rate
            and resident_miss_rate <= LIMITS["post_warmup_deadline_miss_rate"],
            {"paged": paged_miss_rate, "resident": resident_miss_rate},
            {
                "no_regression": True,
                "absolute_limit": LIMITS["post_warmup_deadline_miss_rate"],
            },
        ),
    ]
    passed = all(item["pass"] for item in gates)
    return {
        "status": "pass" if passed else "fail",
        "full_archive_residency_gate_satisfied": passed,
        "production_residency_enabled": False,
        "production_policy_requires_separate_review": True,
        "layout_matrix_remains_paused": passed,
        "summaries": summaries,
        "product_regressions": product_regressions,
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
    hybrid = root / "hybrid.zarr" / "zarr.json"
    if not hybrid.is_file():
        raise RuntimeError(f"Hybrid fixture archive is unavailable: {hybrid}")
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
        (repetition, strategy)
        for repetition in range(5)
        for strategy in (
            ("paged", "resident") if repetition % 2 == 0 else ("resident", "paged")
        )
    ]
    trials: list[dict[str, Any]] = []
    started = time.time()
    for position, (repetition, strategy) in enumerate(order):
        stem = f"{position:02d}_repetition_{repetition}_hybrid_{strategy}"
        output = args.output_dir / f"{stem}.json"
        stdout_path = args.output_dir / f"{stem}.stdout.log"
        stderr_path = args.output_dir / f"{stem}.stderr.log"
        command = [
            str(binary),
            str(args.fixture_root / "hybrid.zarr"),
            args.detection_run,
            str(args.video),
            "hybrid",
            str(repetition),
            strategy,
            str(output),
        ]
        print(
            f"[FullArchiveResidency] position={position + 1}/10 "
            f"repetition={repetition} strategy={strategy}",
            flush=True,
        )
        returncode = 0
        elapsed_ms = 0.0
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
            elapsed_ms = (time.monotonic() - trial_started) * 1000.0

        trial: dict[str, Any] = {
            "position": position,
            "repetition": repetition,
            "layout": "hybrid",
            "strategy": strategy,
            "command": command,
            "result_path": str(output),
            "stdout_path": str(stdout_path),
            "stderr_path": str(stderr_path),
            "subprocess_returncode": returncode,
            "subprocess_elapsed_ms": elapsed_ms,
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
            f"[FullArchiveResidency] completed strategy={strategy} "
            f"repetition={repetition} pass={int(trial['pass'])} "
            f"elapsed_ms={elapsed_ms:.1f}",
            flush=True,
        )

    reduction = reduce_trials(trials)
    aggregate = {
        "schema_id": "crimson.canonical_detection_full_archive_residency_gate",
        "schema_version": 1,
        "classification": "full_duration_residency_interference",
        "created_unix_seconds": time.time(),
        "wall_elapsed_seconds": time.time() - started,
        "fixture_root": str(args.fixture_root),
        "fixture_evidence": fixture_evidence,
        "detection_run": args.detection_run,
        "video": str(args.video),
        "binary": str(binary),
        "balanced_order": [
            {"position": index, "repetition": repetition, "strategy": strategy}
            for index, (repetition, strategy) in enumerate(order)
        ],
        "cache_policy": {
            "tensorstore_cache_bytes": 64 * 1024 * 1024,
            "resident_budget_bytes": 64 * 1024 * 1024,
            "resident_chunk_decoded_bytes": 512 * 1024,
            "speculative_workers": 1,
            "scheduler_workers": 4,
            "page_frames": 70,
        },
        "queue_regression_interpretation": (
            "A 10% and 250 ms paired-median tolerance distinguishes material "
            "regression from process-first mount noise; both limits must pass."
        ),
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
        f"[FullArchiveResidency] status={reduction['status']} "
        f"evidence={aggregate_path}",
        flush=True,
    )
    return 0 if reduction["full_archive_residency_gate_satisfied"] else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(
            f"run_canonical_detection_full_archive_residency: ERROR: {error}",
            file=sys.stderr,
        )
        raise SystemExit(2)
