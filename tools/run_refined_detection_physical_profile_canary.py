#!/usr/bin/env python3
"""Run and reduce the frozen refined-detection physical-profile canary."""

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


DEFAULT_ROOT = Path(
    "/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/"
    "refined_detection_storage/profile_canary/"
    "sleepyfish_accept_all_regular_vs_access_aware_20260727_v1"
)
DEFAULT_BINARY = Path(
    "build/macos-arm64-release/"
    "refined_detection_physical_profile_benchmark"
)
EXPECTED_MANIFEST_SHA256 = (
    "8d9215aa29bf4b0787e50114e9fb429f959194ee3a7f1bdea6fd1d04ae1424b6"
)
EXPECTED_PAYLOAD_DIGEST = (
    "2c00649c378c7a33f5621c4cd91ad787db46dcd0a512d6391ece92b383cd5609"
)

LIMITS = {
    "ready_ms": 180_000.0,
    "access_ready_regression_ratio": 1.10,
    "access_ready_regression_ms": 5_000.0,
    "current_frame_p95_ms": 1_000.0,
    "access_current_frame_regression_ratio": 1.10,
    "access_current_frame_regression_ms": 250.0,
    "post_warmup_deadline_misses": 0,
    "seek_settle_p95_ms": 250.0,
    "peak_rss_bytes": 768 * 1024 * 1024,
    "access_peak_rss_regression_bytes": 64 * 1024 * 1024,
    "access_total_file_bytes_ratio": 1.05,
    "access_traversal_file_bytes_ratio": 0.80,
    "shutdown_ms": 2_000.0,
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
    index = max(0, math.ceil(fraction * len(ordered)) - 1)
    return ordered[index]


def command_output(command: list[str]) -> str:
    try:
        completed = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=20,
        )
        return (completed.stdout or completed.stderr).strip()
    except (OSError, subprocess.TimeoutExpired) as error:
        return f"unavailable: {error}"


def nested(document: dict[str, Any], *keys: str) -> Any:
    value: Any = document
    for key in keys:
        value = value[key]
    return value


def trial_metrics(document: dict[str, Any]) -> dict[str, Any]:
    forward = nested(document, "traversal", "forward")
    reverse = nested(document, "traversal", "reverse")
    return {
        "ready_ms": float(nested(document, "readiness", "total_ms")),
        "first_page_ms": float(
            nested(document, "readiness", "first_page_request_to_publish_ms")
        ),
        "current_frame_p95_ms": float(
            nested(document, "current_frames", "p95_ms")
        ),
        "current_frame_max_ms": float(
            nested(document, "current_frames", "maximum_ms")
        ),
        "offset_ms": float(nested(document, "refined_open", "offset_read_ms")),
        "offset_reads": int(
            nested(document, "refined_open", "offset_read_calls")
        ),
        "retained_offset_bytes": int(
            nested(document, "refined_open", "retained_offset_bytes")
        ),
        "total_file_bytes": int(
            nested(document, "physical_total", "file_bytes")
        ),
        "traversal_file_bytes": int(
            nested(forward, "physical", "file_bytes")
            + nested(reverse, "physical", "file_bytes")
        ),
        "traversal_file_reads": int(
            nested(forward, "physical", "file_reads")
            + nested(reverse, "physical", "file_reads")
        ),
        "post_warmup_misses": int(forward["post_warmup_misses"])
        + int(reverse["post_warmup_misses"]),
        "post_warmup_pages": int(forward["post_warmup_pages"])
        + int(reverse["post_warmup_pages"]),
        "forward_digest": forward["logical_digest_fnv1a64"],
        "reverse_digest": reverse["logical_digest_fnv1a64"],
        "seek_settle_ms": float(nested(document, "seek_burst", "settle_ms")),
        "stale_publications": int(
            nested(document, "seek_burst", "stale_publications")
        ),
        "peak_rss_bytes": int(document["peak_rss_bytes"]),
        "shutdown_ms": float(document["shutdown_ms"]),
        "failed_pages": int(nested(document, "buffer", "failed_pages")),
        "failed_reads": int(nested(document, "repository", "failed_reads")),
        "source_audit_handle_opens": int(
            nested(document, "refined_open", "source_audit_handle_opens")
        ),
        "exact_handle_opens": int(
            nested(document, "refined_open", "exact_handle_opens")
        ),
        "peak_concurrent_fields": int(
            nested(document, "repository", "peak_concurrent_ui_field_reads")
        ),
        "scheduler_failed": int(nested(document, "scheduler", "failed")),
        "scheduler_work_exceptions": int(
            nested(document, "scheduler", "work_exceptions")
        ),
    }


def describe(values: list[float]) -> dict[str, Any]:
    return {
        "values": values,
        "median": statistics.median(values),
        "p95_nearest_rank": percentile(values, 0.95),
        "minimum": min(values),
        "maximum": max(values),
    }


def summarize(trials: list[dict[str, Any]]) -> dict[str, Any]:
    fields = [
        "ready_ms",
        "first_page_ms",
        "current_frame_p95_ms",
        "current_frame_max_ms",
        "offset_ms",
        "total_file_bytes",
        "traversal_file_bytes",
        "traversal_file_reads",
        "seek_settle_ms",
        "peak_rss_bytes",
        "shutdown_ms",
    ]
    return {
        "trial_count": len(trials),
        **{
            field: describe([float(trial["metrics"][field]) for trial in trials])
            for field in fields
        },
        "post_warmup_misses": sum(
            trial["metrics"]["post_warmup_misses"] for trial in trials
        ),
        "post_warmup_pages": sum(
            trial["metrics"]["post_warmup_pages"] for trial in trials
        ),
    }


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
        for layout in ("regular", "access_aware")
    }
    if any(len(layout_trials) != 5 for layout_trials in by_layout.values()):
        return {
            "status": "incomplete",
            "consumer_gate_satisfied": False,
            "promotion_recommended": False,
            "profile_promoted": False,
            "completed_trials": {
                key: len(value) for key, value in by_layout.items()
            },
            "failures": [
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
        layout: summarize(layout_trials)
        for layout, layout_trials in by_layout.items()
    }
    regular = summaries["regular"]
    access = summaries["access_aware"]

    ready_ratio = access["ready_ms"]["median"] / regular["ready_ms"]["median"]
    ready_delta = access["ready_ms"]["median"] - regular["ready_ms"]["median"]
    current_ratio = (
        access["current_frame_p95_ms"]["median"]
        / regular["current_frame_p95_ms"]["median"]
    )
    current_delta = (
        access["current_frame_p95_ms"]["median"]
        - regular["current_frame_p95_ms"]["median"]
    )
    rss_delta = (
        access["peak_rss_bytes"]["median"]
        - regular["peak_rss_bytes"]["median"]
    )
    total_bytes_ratio = (
        access["total_file_bytes"]["median"]
        / regular["total_file_bytes"]["median"]
    )
    traversal_bytes_ratio = (
        access["traversal_file_bytes"]["median"]
        / regular["traversal_file_bytes"]["median"]
    )

    correctness_failures: list[str] = []
    by_pair: dict[int, dict[str, dict[str, Any]]] = {}
    for trial in complete:
        metrics = trial["metrics"]
        by_pair.setdefault(trial["repetition"], {})[trial["layout"]] = metrics
        checks = {
            "clean_immutable_binary": trial["immutable_crimson_revision_bound"],
            "offset_reads": metrics["offset_reads"] == 1,
            "retained_offsets": metrics["retained_offset_bytes"] == 9_504_008,
            "exact_handles": metrics["exact_handle_opens"] == 11,
            "lazy_source_audit": metrics["source_audit_handle_opens"] == 0,
            "failed_pages": metrics["failed_pages"] == 0,
            "failed_reads": metrics["failed_reads"] == 0,
            "stale_publications": metrics["stale_publications"] == 0,
            "scheduler_failed": metrics["scheduler_failed"] == 0,
            "scheduler_work_exceptions": metrics["scheduler_work_exceptions"]
            == 0,
            "concurrent_fields": metrics["peak_concurrent_fields"] >= 2,
        }
        for check, passed in checks.items():
            if not passed:
                correctness_failures.append(
                    f"{trial['layout']} repetition {trial['repetition']}: {check}"
                )
    for repetition, pair in sorted(by_pair.items()):
        if set(pair) != {"regular", "access_aware"}:
            correctness_failures.append(f"repetition {repetition}: incomplete pair")
            continue
        if pair["regular"]["forward_digest"] != pair["access_aware"][
            "forward_digest"
        ]:
            correctness_failures.append(
                f"repetition {repetition}: forward digest mismatch"
            )
        if pair["regular"]["reverse_digest"] != pair["access_aware"][
            "reverse_digest"
        ]:
            correctness_failures.append(
                f"repetition {repetition}: reverse digest mismatch"
            )

    gates = [
        gate(
            "correctness",
            not correctness_failures,
            correctness_failures,
            "exact contract and paired logical digests",
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
            "access_aware_ready_regression",
            ready_ratio <= LIMITS["access_ready_regression_ratio"]
            and ready_delta <= LIMITS["access_ready_regression_ms"],
            {"ratio": ready_ratio, "milliseconds": ready_delta},
            {
                "ratio": LIMITS["access_ready_regression_ratio"],
                "milliseconds": LIMITS["access_ready_regression_ms"],
            },
        ),
        gate(
            "current_frame_p95",
            max(
                regular["current_frame_p95_ms"]["p95_nearest_rank"],
                access["current_frame_p95_ms"]["p95_nearest_rank"],
            )
            <= LIMITS["current_frame_p95_ms"],
            {
                "regular": regular["current_frame_p95_ms"]["p95_nearest_rank"],
                "access_aware": access["current_frame_p95_ms"][
                    "p95_nearest_rank"
                ],
            },
            LIMITS["current_frame_p95_ms"],
        ),
        gate(
            "access_aware_current_frame_regression",
            current_ratio <= LIMITS["access_current_frame_regression_ratio"]
            and current_delta <= LIMITS["access_current_frame_regression_ms"],
            {"ratio": current_ratio, "milliseconds": current_delta},
            {
                "ratio": LIMITS["access_current_frame_regression_ratio"],
                "milliseconds": LIMITS["access_current_frame_regression_ms"],
            },
        ),
        gate(
            "zero_post_warmup_deadline_misses",
            regular["post_warmup_misses"] == 0
            and access["post_warmup_misses"] == 0,
            {
                "regular": regular["post_warmup_misses"],
                "access_aware": access["post_warmup_misses"],
            },
            LIMITS["post_warmup_deadline_misses"],
        ),
        gate(
            "seek_settle_p95",
            max(
                regular["seek_settle_ms"]["p95_nearest_rank"],
                access["seek_settle_ms"]["p95_nearest_rank"],
            )
            <= LIMITS["seek_settle_p95_ms"],
            {
                "regular": regular["seek_settle_ms"]["p95_nearest_rank"],
                "access_aware": access["seek_settle_ms"]["p95_nearest_rank"],
            },
            LIMITS["seek_settle_p95_ms"],
        ),
        gate(
            "every_peak_rss",
            all(
                trial["metrics"]["peak_rss_bytes"]
                <= LIMITS["peak_rss_bytes"]
                for trial in complete
            ),
            max(trial["metrics"]["peak_rss_bytes"] for trial in complete),
            LIMITS["peak_rss_bytes"],
        ),
        gate(
            "access_aware_peak_rss_regression",
            rss_delta <= LIMITS["access_peak_rss_regression_bytes"],
            rss_delta,
            LIMITS["access_peak_rss_regression_bytes"],
        ),
        gate(
            "access_aware_total_file_bytes_ratio",
            total_bytes_ratio <= LIMITS["access_total_file_bytes_ratio"],
            total_bytes_ratio,
            LIMITS["access_total_file_bytes_ratio"],
        ),
        gate(
            "access_aware_traversal_file_bytes_ratio",
            traversal_bytes_ratio
            <= LIMITS["access_traversal_file_bytes_ratio"],
            traversal_bytes_ratio,
            LIMITS["access_traversal_file_bytes_ratio"],
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
        "consumer_gate_satisfied": passed,
        "promotion_recommended": passed,
        "profile_promoted": False,
        "promotion_scope": "Palette versioned storage-profile decision",
        "summaries": summaries,
        "comparisons": {
            "access_aware_ready_ratio": ready_ratio,
            "access_aware_ready_regression_ms": ready_delta,
            "access_aware_current_frame_p95_ratio": current_ratio,
            "access_aware_current_frame_p95_regression_ms": current_delta,
            "access_aware_peak_rss_median_regression_bytes": rss_delta,
            "access_aware_total_file_bytes_ratio": total_bytes_ratio,
            "access_aware_traversal_file_bytes_ratio": traversal_bytes_ratio,
        },
        "gates": gates,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--canary-root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=float, default=240.0)
    parser.add_argument("--resume", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo = Path.cwd().resolve()
    binary = args.binary if args.binary.is_absolute() else repo / args.binary
    manifest = args.canary_root / "canary_manifest.json"
    if not binary.is_file():
        raise RuntimeError(f"Benchmark binary is unavailable: {binary}")
    if sha256(manifest) != EXPECTED_MANIFEST_SHA256:
        raise RuntimeError("Canary manifest file SHA-256 is not frozen value")
    args.output_dir.mkdir(parents=True, exist_ok=True)

    order = [
        (repetition, layout)
        for repetition in range(5)
        for layout in (
            ("regular", "access_aware")
            if repetition % 2 == 0
            else ("access_aware", "regular")
        )
    ]
    trials: list[dict[str, Any]] = []
    for ordinal, (repetition, layout) in enumerate(order):
        output = args.output_dir / f"{ordinal:02d}_rep{repetition}_{layout}.json"
        stdout_path = output.with_suffix(".stdout.txt")
        stderr_path = output.with_suffix(".stderr.txt")
        if args.resume and output.is_file():
            document = json.loads(output.read_text())
        else:
            command = [
                str(binary),
                str(manifest),
                EXPECTED_PAYLOAD_DIGEST,
                layout,
                str(repetition),
                str(output),
            ]
            started = time.monotonic()
            try:
                completed = subprocess.run(
                    command,
                    cwd=repo,
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout=args.timeout_seconds,
                )
                stdout_path.write_text(completed.stdout)
                stderr_path.write_text(completed.stderr)
                if not output.is_file():
                    raise RuntimeError(
                        f"Trial produced no JSON ({completed.returncode}): "
                        f"{completed.stderr.strip()}"
                    )
                document = json.loads(output.read_text())
                document["runner"] = {
                    "command": command,
                    "returncode": completed.returncode,
                    "wall_seconds": time.monotonic() - started,
                }
                output.write_text(json.dumps(document, indent=2) + "\n")
            except subprocess.TimeoutExpired as error:
                document = {
                    "pass": False,
                    "layout": layout,
                    "repetition": repetition,
                    "error": f"trial timed out after {error.timeout} seconds",
                }
                output.write_text(json.dumps(document, indent=2) + "\n")
        trials.append(
            {
                "ordinal": ordinal,
                "layout": layout,
                "repetition": repetition,
                "pass": bool(document.get("pass")),
                "error": document.get("error", ""),
                "path": str(output),
                "sha256": sha256(output),
                "immutable_crimson_revision_bound": bool(
                    document.get("immutable_crimson_revision_bound")
                ),
                "crimson_commit": document.get("crimson_commit", ""),
                "metrics": trial_metrics(document)
                if document.get("pass")
                else {},
            }
        )
        print(
            f"[{ordinal + 1:02d}/{len(order):02d}] repetition={repetition} "
            f"layout={layout} pass={bool(document.get('pass'))}",
            flush=True,
        )

    aggregate = {
        "schema_id": "crimson.refined_detection_physical_profile_canary",
        "schema_version": 1,
        "canary_manifest": str(manifest),
        "canary_manifest_sha256": EXPECTED_MANIFEST_SHA256,
        "canary_payload_digest": EXPECTED_PAYLOAD_DIGEST,
        "limits": LIMITS,
        "process_order": [
            {"ordinal": index, "repetition": rep, "layout": layout}
            for index, (rep, layout) in enumerate(order)
        ],
        "environment": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "python": sys.version,
            "crimson_head": command_output(["git", "rev-parse", "HEAD"]),
            "crimson_status": command_output(["git", "status", "--short"]),
            "tensorstore_cache_bytes": 64 * 1024 * 1024,
            "mount": str(args.canary_root),
            "cache_classification": "process-first; OS/SMB/server caches uncontrolled",
        },
        "trials": trials,
        "verdict": reduce_trials(trials),
    }
    aggregate_path = args.output_dir / "aggregate.json"
    aggregate_path.write_text(json.dumps(aggregate, indent=2) + "\n")
    print(aggregate_path)
    print(json.dumps(aggregate["verdict"], indent=2))
    return 0 if aggregate["verdict"]["consumer_gate_satisfied"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
