#!/usr/bin/env python3
"""Run and reduce the frozen Phase 5O.5 detection-residency gate."""

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
DEFAULT_BINARY = Path(
    "build/macos-arm64-release/canonical_detection_residency_benchmark"
)
DEFAULT_WORKLOAD = Path(
    "docs/reference/crimson_canonical_detection_layout_matrix_workload_v1.json"
)
DEFAULT_RUN = "crimson_storage_fixture_sleepyfish_cam2010095_v1"
WORKLOAD_SHA256 = (
    "75d958f7ef4a7162b9b945a210c25de26fb7c3020ed7e9d9551753875bcb6d57"
)
MANIFEST_HASHES = {
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

CONDITION_ORDER = {
    0: ["hybrid_paged", "hybrid_resident", "regular_paged", "regular_resident"],
    1: ["hybrid_resident", "regular_resident", "hybrid_paged", "regular_paged"],
    2: ["regular_paged", "hybrid_paged", "regular_resident", "hybrid_resident"],
    3: ["regular_resident", "regular_paged", "hybrid_resident", "hybrid_paged"],
    4: ["hybrid_paged", "regular_paged", "regular_resident", "hybrid_resident"],
}

LIMITS = {
    "first_page_service_ms": 150.0,
    "resident_first_page_ratio": 1.10,
    "resident_first_page_delta_ms": 25.0,
    "resident_preload_ms": 30_000.0,
    "resident_preload_file_bytes": 64 * 1024 * 1024,
    "resident_random_p95_ms": 5.0,
    "post_warmup_deadline_miss_rate": 0.01,
    "incremental_resident_rss_bytes": 64 * 1024 * 1024,
    "peak_rss_bytes": 768 * 1024 * 1024,
    "cancellation_and_close_ms": 250.0,
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        raise ValueError("Cannot reduce an empty sample")
    ordered = sorted(values)
    return ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]


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


def write_json(path: Path, document: Any) -> None:
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n")


def gate(name: str, passed: bool, observed: Any, limit: Any) -> dict[str, Any]:
    return {
        "name": name,
        "pass": bool(passed),
        "observed": observed,
        "limit": limit,
    }


def metric_summary(values: list[float]) -> dict[str, Any]:
    return {
        "values": values,
        "median": statistics.median(values),
        "p95_nearest_rank": percentile(values, 0.95),
        "minimum": min(values),
        "maximum": max(values),
    }


def extract_metrics(document: dict[str, Any]) -> dict[str, Any]:
    transition = document["strategy_transition"]
    residency = transition["residency"]
    traversal = document["traversal"]
    random_frames = document["random_frames"]
    return {
        "archive_open_ms": float(document["initialization"]["archive_open_ms"]),
        "repository_open_ms": float(
            document["initialization"]["repository_wall_ms"]
        ),
        "offset_read_ms": float(document["initialization"]["offset_read_ms"]),
        "first_page_presentation_ms": float(
            document["first_page"]["presentation_ms"]
        ),
        "first_page_service_ms": float(
            document["first_page"]["repository_service_max_ms"]
        ),
        "demand_probe_ms": float(transition["demand_probe_ms"]),
        "resident_preload_ms": float(residency["elapsed_ms"]),
        "resident_decoded_hot_bytes": int(residency["decoded_hot_bytes"]),
        "resident_retained_bytes": int(residency["retained_bytes"]),
        "resident_preload_file_bytes": int(
            transition["physical_upper_bound"]["file_bytes"]
        ),
        "resident_incremental_rss_bytes": int(transition["incremental_rss_bytes"]),
        "random_p95_ms": float(random_frames["presentation_p95_ms"]),
        "random_file_bytes": int(random_frames["physical"]["file_bytes"]),
        "random_field_reads": int(random_frames["repository_field_reads"]),
        "traversal_page_p95_ms": float(traversal["page_ready_p95_ms"]),
        "traversal_file_bytes": int(traversal["physical"]["file_bytes"]),
        "traversal_field_reads": int(traversal["repository_field_reads"]),
        "post_warmup_misses": int(traversal["post_warmup_misses"]),
        "post_warmup_pages": int(traversal["post_warmup_pages"]),
        "rapid_seek_settle_ms": float(document["rapid_seek"]["settle_ms"]),
        "post_cancel_bytes_per_seek": float(
            document["rapid_seek"]["post_cancel_file_bytes_per_superseded_seek"]
        ),
        "close_ms": float(document["close_ms"]),
        "peak_rss_bytes": int(document["peak_rss_bytes"]),
        "total_file_bytes": int(document["physical_total"]["file_bytes"]),
        "offset_reads": int(document["initialization"]["offset_read_calls"]),
        "fallback_metadata_reads": int(
            document["initialization"]["fallback_metadata_reads"]
        ),
        "fallback_dtype_opens": int(
            document["initialization"]["fallback_dtype_opens"]
        ),
        "residency_state": residency["state"],
        "resident_publications": int(residency["publications"]),
        "resident_stale_chunks": int(residency["stale_chunks"]),
        "resident_failed_chunks": int(residency["failed_chunks"]),
        "failed_pages": int(document["buffer"]["failed_pages"]),
        "repository_failed_reads": int(document["repository"]["failed_reads"]),
        "scheduler_failed": int(document["scheduler"]["failed"]),
        "scheduler_work_exceptions": int(
            document["scheduler"]["work_exceptions"]
        ),
        "traversal_digest": traversal["logical_digest_fnv1a64"],
        "random_digest": random_frames["logical_digest_fnv1a64"],
    }


def summarize_condition(trials: list[dict[str, Any]]) -> dict[str, Any]:
    numeric = [
        "archive_open_ms",
        "repository_open_ms",
        "offset_read_ms",
        "first_page_presentation_ms",
        "first_page_service_ms",
        "demand_probe_ms",
        "resident_preload_ms",
        "resident_preload_file_bytes",
        "resident_incremental_rss_bytes",
        "random_p95_ms",
        "random_file_bytes",
        "traversal_page_p95_ms",
        "traversal_file_bytes",
        "rapid_seek_settle_ms",
        "post_cancel_bytes_per_seek",
        "close_ms",
        "peak_rss_bytes",
        "total_file_bytes",
    ]
    result: dict[str, Any] = {"trial_count": len(trials)}
    for name in numeric:
        result[name] = metric_summary(
            [float(trial["metrics"][name]) for trial in trials]
        )
    misses = sum(trial["metrics"]["post_warmup_misses"] for trial in trials)
    pages = sum(trial["metrics"]["post_warmup_pages"] for trial in trials)
    result["post_warmup_misses"] = misses
    result["post_warmup_pages"] = pages
    result["post_warmup_miss_rate"] = misses / pages if pages else 0.0
    return result


def reduce_trials(trials: list[dict[str, Any]]) -> dict[str, Any]:
    conditions = [
        "hybrid_paged",
        "hybrid_resident",
        "regular_paged",
        "regular_resident",
    ]
    complete = [trial for trial in trials if trial.get("pass")]
    by_condition = {
        condition: [trial for trial in complete if trial["condition"] == condition]
        for condition in conditions
    }
    if any(len(items) != 5 for items in by_condition.values()):
        return {
            "status": "incomplete",
            "resident_strategy_gate_satisfied": False,
            "completed_trials": {
                condition: len(items) for condition, items in by_condition.items()
            },
            "failed_trials": [trial for trial in trials if not trial.get("pass")],
        }

    summaries = {
        condition: summarize_condition(items)
        for condition, items in by_condition.items()
    }
    gates: list[dict[str, Any]] = []
    all_metrics = [trial["metrics"] for trial in complete]

    gates.append(
        gate(
            "exactly_one_offset_read",
            all(metric["offset_reads"] == 1 for metric in all_metrics),
            [metric["offset_reads"] for metric in all_metrics],
            1,
        )
    )
    gates.append(
        gate(
            "no_fallback_or_failed_work",
            all(
                metric[name] == 0
                for metric in all_metrics
                for name in (
                    "fallback_metadata_reads",
                    "fallback_dtype_opens",
                    "resident_failed_chunks",
                    "failed_pages",
                    "repository_failed_reads",
                    "scheduler_failed",
                    "scheduler_work_exceptions",
                )
            ),
            "all zero",
            0,
        )
    )
    digests_equal = True
    digest_evidence: dict[str, Any] = {}
    for repetition in range(5):
        matched = [trial for trial in complete if trial["repetition"] == repetition]
        traversal = sorted({trial["metrics"]["traversal_digest"] for trial in matched})
        random_values = sorted({trial["metrics"]["random_digest"] for trial in matched})
        digest_evidence[str(repetition)] = {
            "traversal": traversal,
            "random": random_values,
        }
        digests_equal &= len(traversal) == 1 and len(random_values) == 1
    gates.append(gate("exact_decoded_digests", digests_equal, digest_evidence, 1))

    resident = [
        trial for trial in complete if trial["strategy"] == "resident"
    ]
    gates.append(
        gate(
            "one_atomic_resident_publication",
            all(
                trial["metrics"]["residency_state"] == "ready"
                and trial["metrics"]["resident_publications"] == 1
                and trial["metrics"]["resident_stale_chunks"] == 0
                and trial["metrics"]["resident_decoded_hot_bytes"] == 37_986_784
                and trial["metrics"]["resident_retained_bytes"] == 37_986_784
                for trial in resident
            ),
            [
                {
                    "condition": trial["condition"],
                    "state": trial["metrics"]["residency_state"],
                    "publications": trial["metrics"]["resident_publications"],
                    "stale_chunks": trial["metrics"]["resident_stale_chunks"],
                    "decoded_hot_bytes": trial["metrics"][
                        "resident_decoded_hot_bytes"
                    ],
                    "retained_bytes": trial["metrics"]["resident_retained_bytes"],
                }
                for trial in resident
            ],
            {
                "state": "ready",
                "publications": 1,
                "stale_chunks": 0,
                "decoded_hot_bytes": 37_986_784,
                "retained_bytes": 37_986_784,
            },
        )
    )
    gates.append(
        gate(
            "first_page_service_p95_ms",
            all(
                summaries[condition]["first_page_service_ms"]["p95_nearest_rank"]
                <= LIMITS["first_page_service_ms"]
                for condition in conditions
            ),
            {
                condition: summaries[condition]["first_page_service_ms"][
                    "p95_nearest_rank"
                ]
                for condition in conditions
            },
            LIMITS["first_page_service_ms"],
        )
    )

    first_page_regressions: dict[str, Any] = {}
    first_page_pass = True
    for layout in ("hybrid", "regular"):
        paged = summaries[f"{layout}_paged"]["first_page_presentation_ms"]["median"]
        resident_value = summaries[f"{layout}_resident"][
            "first_page_presentation_ms"
        ]["median"]
        ratio = resident_value / paged if paged else math.inf
        delta = resident_value - paged
        first_page_regressions[layout] = {
            "paged_median_ms": paged,
            "resident_median_ms": resident_value,
            "ratio": ratio,
            "delta_ms": delta,
        }
        first_page_pass &= (
            ratio <= LIMITS["resident_first_page_ratio"]
            and delta <= LIMITS["resident_first_page_delta_ms"]
        )
    gates.append(
        gate(
            "resident_first_page_regression",
            first_page_pass,
            first_page_regressions,
            {
                "ratio": LIMITS["resident_first_page_ratio"],
                "delta_ms": LIMITS["resident_first_page_delta_ms"],
            },
        )
    )

    resident_preload = [trial["metrics"]["resident_preload_ms"] for trial in resident]
    resident_bytes = [
        trial["metrics"]["resident_preload_file_bytes"] for trial in resident
    ]
    gates.append(
        gate(
            "resident_preload_p95_ms",
            percentile(resident_preload, 0.95) <= LIMITS["resident_preload_ms"],
            percentile(resident_preload, 0.95),
            LIMITS["resident_preload_ms"],
        )
    )
    gates.append(
        gate(
            "resident_preload_file_bytes_upper_bound",
            max(resident_bytes) <= LIMITS["resident_preload_file_bytes"],
            max(resident_bytes),
            LIMITS["resident_preload_file_bytes"],
        )
    )
    resident_random = [trial["metrics"]["random_p95_ms"] for trial in resident]
    gates.append(
        gate(
            "resident_random_presentation_p95_ms",
            percentile(resident_random, 0.95) <= LIMITS["resident_random_p95_ms"],
            percentile(resident_random, 0.95),
            LIMITS["resident_random_p95_ms"],
        )
    )
    gates.append(
        gate(
            "post_residency_storage_reads",
            all(
                trial["metrics"]["random_file_bytes"] == 0
                and trial["metrics"]["traversal_file_bytes"] == 0
                and trial["metrics"]["random_field_reads"] == 0
                and trial["metrics"]["traversal_field_reads"] == 0
                for trial in resident
            ),
            [
                {
                    "condition": trial["condition"],
                    "random_bytes": trial["metrics"]["random_file_bytes"],
                    "traversal_bytes": trial["metrics"]["traversal_file_bytes"],
                    "random_field_reads": trial["metrics"]["random_field_reads"],
                    "traversal_field_reads": trial["metrics"][
                        "traversal_field_reads"
                    ],
                }
                for trial in resident
            ],
            0,
        )
    )

    total_misses = sum(metric["post_warmup_misses"] for metric in all_metrics)
    total_pages = sum(metric["post_warmup_pages"] for metric in all_metrics)
    miss_rate = total_misses / total_pages if total_pages else 0.0
    gates.append(
        gate(
            "post_warmup_deadline_miss_rate",
            miss_rate <= LIMITS["post_warmup_deadline_miss_rate"],
            {"misses": total_misses, "pages": total_pages, "rate": miss_rate},
            LIMITS["post_warmup_deadline_miss_rate"],
        )
    )
    resident_rss = [
        trial["metrics"]["resident_incremental_rss_bytes"] for trial in resident
    ]
    gates.append(
        gate(
            "incremental_resident_rss_p95_bytes",
            percentile(resident_rss, 0.95)
            <= LIMITS["incremental_resident_rss_bytes"],
            percentile(resident_rss, 0.95),
            LIMITS["incremental_resident_rss_bytes"],
        )
    )
    peak_rss = [metric["peak_rss_bytes"] for metric in all_metrics]
    gates.append(
        gate(
            "detection_process_peak_rss_bytes",
            max(peak_rss) <= LIMITS["peak_rss_bytes"],
            max(peak_rss),
            LIMITS["peak_rss_bytes"],
        )
    )
    cancellation = [metric["rapid_seek_settle_ms"] for metric in all_metrics]
    closes = [metric["close_ms"] for metric in all_metrics]
    gates.append(
        gate(
            "cancellation_and_close_p95_ms",
            percentile(cancellation, 0.95)
            <= LIMITS["cancellation_and_close_ms"]
            and percentile(closes, 0.95)
            <= LIMITS["cancellation_and_close_ms"],
            {
                "cancellation_p95_ms": percentile(cancellation, 0.95),
                "close_p95_ms": percentile(closes, 0.95),
            },
            LIMITS["cancellation_and_close_ms"],
        )
    )

    passed = all(item["pass"] for item in gates)
    return {
        "status": "complete",
        "resident_strategy_gate_satisfied": passed,
        "full_archive_interference_required": passed,
        "layout_matrix_remains_paused": True,
        "condition_summaries": summaries,
        "gates": gates,
    }


def trial_plan(quick: bool) -> list[tuple[int, str]]:
    if quick:
        return [(0, "hybrid_paged"), (0, "hybrid_resident")]
    return [
        (repetition, condition)
        for repetition in range(5)
        for condition in CONDITION_ORDER[repetition]
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture-root", type=Path, default=DEFAULT_FIXTURE_ROOT)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--workload", type=Path, default=DEFAULT_WORKLOAD)
    parser.add_argument("--run", default=DEFAULT_RUN)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--timeout-seconds", type=int, default=600)
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    binary = args.binary if args.binary.is_absolute() else repo_root / args.binary
    workload = (
        args.workload if args.workload.is_absolute() else repo_root / args.workload
    )
    if args.output_dir:
        output_dir = args.output_dir
    else:
        stamp = time.strftime("%Y%m%d-%H%M%S")
        output_dir = repo_root / "docs" / "diagnostics" / f"detection_residency_{stamp}"
    output_dir.mkdir(parents=True, exist_ok=True)
    trials_dir = output_dir / "trials"
    trials_dir.mkdir(exist_ok=True)

    if not binary.is_file():
        raise SystemExit(f"Benchmark binary is unavailable: {binary}")
    if not workload.is_file():
        raise SystemExit(f"Workload is unavailable: {workload}")
    workload_hash = sha256(workload)
    if workload_hash != WORKLOAD_SHA256:
        raise SystemExit(
            f"Workload SHA-256 mismatch: {workload_hash} != {WORKLOAD_SHA256}"
        )
    for layout in ("regular", "hybrid"):
        if not (args.fixture_root / f"{layout}.zarr" / "zarr.json").is_file():
            raise SystemExit(f"Fixture is unavailable: {args.fixture_root / f'{layout}.zarr'}")
    fixture_hashes: dict[str, str] = {}
    for name, expected in MANIFEST_HASHES.items():
        path = args.fixture_root / name
        if not path.is_file():
            raise SystemExit(f"Fixture evidence is unavailable: {path}")
        observed = sha256(path)
        if observed != expected:
            raise SystemExit(f"Fixture hash mismatch for {name}: {observed}")
        fixture_hashes[name] = observed

    manifest = {
        "schema_id": "crimson.canonical_detection_residency_run_manifest",
        "schema_version": 1,
        "created_local": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "quick": args.quick,
        "fixture_root": str(args.fixture_root),
        "fixture_hashes": fixture_hashes,
        "run": args.run,
        "binary": str(binary),
        "workload": str(workload),
        "workload_sha256": workload_hash,
        "crimson_commit": command_output(["git", "rev-parse", "HEAD"], repo_root),
        "crimson_status": command_output(["git", "status", "--short"], repo_root),
        "system": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "python": sys.version,
            "macos": command_output(["sw_vers"]),
            "mount": command_output(["mount"]),
        },
        "limits": LIMITS,
        "process_order": CONDITION_ORDER,
    }
    write_json(output_dir / "run_manifest.json", manifest)

    trials: list[dict[str, Any]] = []
    for ordinal, (repetition, condition) in enumerate(trial_plan(args.quick), start=1):
        layout, strategy = condition.split("_", 1)
        evidence_path = trials_dir / f"r{repetition}_{condition}.json"
        log_path = trials_dir / f"r{repetition}_{condition}.log"
        if args.resume and evidence_path.is_file():
            document = json.loads(evidence_path.read_text())
            print(f"[{ordinal}] resume {condition} repetition={repetition}")
        else:
            command = [
                str(binary),
                str(args.fixture_root / f"{layout}.zarr"),
                args.run,
                layout,
                strategy,
                str(repetition),
                str(workload),
                str(evidence_path),
            ]
            print(f"[{ordinal}/{len(trial_plan(args.quick))}] {condition} repetition={repetition}")
            started = time.monotonic()
            timed_out = False
            try:
                completed = subprocess.run(
                    command,
                    cwd=repo_root,
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout=args.timeout_seconds,
                )
                returncode = completed.returncode
                stdout = completed.stdout
                stderr = completed.stderr
            except subprocess.TimeoutExpired as error:
                timed_out = True
                returncode = -1
                stdout = error.stdout or ""
                stderr = error.stderr or ""
                if isinstance(stdout, bytes):
                    stdout = stdout.decode(errors="replace")
                if isinstance(stderr, bytes):
                    stderr = stderr.decode(errors="replace")
            log_path.write_text(
                f"command={command!r}\nreturncode={returncode}\n"
                f"timed_out={timed_out}\n"
                f"elapsed_s={time.monotonic() - started:.6f}\n"
                f"STDOUT\n{stdout}\nSTDERR\n{stderr}\n"
            )
            if evidence_path.is_file():
                document = json.loads(evidence_path.read_text())
            else:
                document = {
                    "pass": False,
                    "condition": condition,
                    "layout": layout,
                    "strategy": strategy,
                    "repetition": repetition,
                    "error": (
                        f"process timed out after {args.timeout_seconds} seconds"
                        if timed_out
                        else f"process returned {returncode} without evidence"
                    ),
                }
        trial: dict[str, Any] = {
            "condition": condition,
            "layout": layout,
            "strategy": strategy,
            "repetition": repetition,
            "pass": bool(document.get("pass")),
            "evidence_path": str(evidence_path),
            "log_path": str(log_path),
            "error": document.get("error", ""),
        }
        if trial["pass"]:
            trial["metrics"] = extract_metrics(document)
        trials.append(trial)
        write_json(output_dir / "trials.json", trials)

    summary = reduce_trials(trials)
    summary.update(
        {
            "schema_id": "crimson.canonical_detection_residency_summary",
            "schema_version": 1,
            "trial_count": len(trials),
            "run_manifest": str(output_dir / "run_manifest.json"),
            "trials": str(output_dir / "trials.json"),
        }
    )
    write_json(output_dir / "summary.json", summary)
    print(f"summary={output_dir / 'summary.json'}")
    print(
        "resident_strategy_gate_satisfied="
        f"{summary.get('resident_strategy_gate_satisfied', False)}"
    )
    return 0 if all(trial["pass"] for trial in trials) else 1


if __name__ == "__main__":
    raise SystemExit(main())
