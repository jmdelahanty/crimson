#!/usr/bin/env python3
"""Run and reduce the frozen keypoint-v2 long-duration experiment."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import platform
import re
import statistics
import subprocess
import sys
import tempfile
import time
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

from plot_keypoint_v2_long_duration_benchmark import plot_aggregate


EXPERIMENT_SCHEMA = "crimson.keypoint_v2.long_duration_experiment"
EXPERIMENT_VERSION = 1
RESULT_SCHEMA = "crimson.keypoint_v2.long_duration_benchmark"
RESULT_VERSION = 1
AGGREGATE_SCHEMA = "crimson.keypoint_v2.long_duration_aggregate"
AGGREGATE_VERSION = 1
IDENTIFIER = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")

METRIC_PATHS = (
    "archive_open_ms",
    "repository_open_ms",
    "first_presentation_readiness_ms",
    "first_presentation.elapsed_ms",
    "first_presentation.physical.file_bytes",
    "random_process_first.p95_ms",
    "random_process_first.physical.file_bytes",
    "random_warm.p95_ms",
    "random_warm.physical.file_bytes",
    "forward_traversal.page_p95_ms",
    "forward_traversal.post_warmup_deadline_miss_ratio",
    "forward_traversal.physical.file_bytes",
    "reverse_traversal.page_p95_ms",
    "reverse_traversal.post_warmup_deadline_miss_ratio",
    "reverse_traversal.physical.file_bytes",
    "rapid_seeks.final_readiness_ms",
    "rapid_seeks.stale_visible_frames",
    "rapid_seeks.physical_transfer_upper_bound.file_bytes",
    "scheduler_metrics.timing_by_priority.current_frame.queue_maximum_ms",
    "scheduler_metrics.timing_by_priority.current_frame.service_maximum_ms",
    "open_metrics.retained_offset_bytes",
    "open_metrics.quality_payload_reads",
    "access_metrics.quality_payload_read_calls",
    "access_metrics.read_failures",
    "configured_cache_bytes",
    "process_physical.file_reads",
    "process_physical.file_bytes",
    "process_physical.cache_hits",
    "process_physical.cache_misses",
    "process_physical.cache_evictions",
    "peak_rss_bytes",
    "close_ms",
)

LOGICAL_PATHS = (
    "frame_count",
    "row_count",
    "keypoint_count",
    "first_presentation.logical_digest",
    "random_process_first.logical_digest",
    "random_warm.logical_digest",
    "forward_traversal.logical_digest",
    "reverse_traversal.logical_digest",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def canonical_json_sha256(document: Any) -> str:
    encoded = json.dumps(
        document, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


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


def nested(document: dict[str, Any], path: str) -> Any:
    current: Any = document
    for component in path.split("."):
        current = current[component]
    return current


def nearest_rank(values: list[float], fraction: float) -> float:
    require(bool(values), "Cannot reduce an empty sample")
    ordered = sorted(values)
    rank = max(0, math.ceil(fraction * len(ordered)) - 1)
    return ordered[rank]


def distribution(values: list[float]) -> dict[str, Any]:
    require(bool(values), "Cannot reduce an empty distribution")
    return {
        "values": values,
        "minimum": min(values),
        "median": statistics.median(values),
        "p95_nearest_rank": nearest_rank(values, 0.95),
        "maximum": max(values),
    }


def resolve_path(value: str, manifest_path: Path) -> Path:
    path = Path(value).expanduser()
    if path.is_absolute():
        return path.resolve()
    return (manifest_path.parent / path).resolve()


def validate_artifact(stage: dict[str, Any], label: str, manifest_path: Path) -> None:
    require(set(stage) == {"store", "run", "manifest_digest"},
            f"{label} must contain only store, run, and manifest_digest")
    store = resolve_path(stage["store"], manifest_path)
    require((store / "zarr.json").is_file(),
            f"{label} Zarr root is unavailable: {store}")
    require(bool(stage["run"]) and "/" not in stage["run"],
            f"{label} run must be a simple name")
    require(bool(SHA256.fullmatch(stage["manifest_digest"])),
            f"{label} manifest digest is invalid")
    stage["store"] = str(store)


def load_experiment(path: Path) -> dict[str, Any]:
    document = json.loads(path.read_text(encoding="utf-8"))
    require(document.get("schema_id") == EXPERIMENT_SCHEMA and
            document.get("schema_version") == EXPERIMENT_VERSION,
            "Unsupported keypoint experiment schema")
    allowed = {
        "schema_id", "schema_version", "description", "workload",
        "repetition_count", "timeout_seconds", "expected_crimson_commit",
        "evidence_files", "candidates", "comparisons",
    }
    require(set(document).issubset(allowed),
            "Experiment contains unsupported top-level fields")
    require(isinstance(document.get("candidates"), list) and
            bool(document["candidates"]), "Experiment has no candidates")
    require(isinstance(document.get("comparisons", []), list),
            "Experiment comparisons must be an array")
    require(isinstance(document.get("repetition_count"), int) and
            document["repetition_count"] > 0,
            "Experiment repetition_count must be positive")
    require(float(document.get("timeout_seconds", 0.0)) > 0.0,
            "Experiment timeout_seconds must be positive")
    expected_commit = document.get("expected_crimson_commit", "")
    require(not expected_commit or
            bool(re.fullmatch(r"[0-9a-f]{40}", expected_commit)),
            "expected_crimson_commit must be a full lowercase Git hash")

    workload_path = resolve_path(document["workload"], path)
    workload = json.loads(workload_path.read_text(encoding="utf-8"))
    require(workload.get("schema_id") ==
            "crimson.keypoint_v2.long_duration_workload" and
            workload.get("schema_version") == 1,
            "Experiment workload schema is incompatible")
    require(workload.get("repetition_count") == document["repetition_count"],
            "Experiment and workload repetition counts disagree")
    document["workload"] = str(workload_path)
    document["workload_file_sha256"] = sha256_file(workload_path)
    document["workload_canonical_sha256"] = canonical_json_sha256(workload)

    require(isinstance(document.get("evidence_files", []), list),
            "Experiment evidence_files must be an array")
    for evidence in document.get("evidence_files", []):
        require(isinstance(evidence, dict),
                "Evidence declarations must be objects")
        require(set(evidence) == {"path", "sha256"},
                "Evidence declarations require path and sha256")
        evidence_path = resolve_path(evidence["path"], path)
        require(evidence_path.is_file(),
                f"Evidence file is unavailable: {evidence_path}")
        require(sha256_file(evidence_path) == evidence["sha256"],
                f"Evidence digest mismatch: {evidence_path}")
        evidence["path"] = str(evidence_path)

    identifiers: set[str] = set()
    for candidate in document["candidates"]:
        require(isinstance(candidate, dict), "Candidates must be objects")
        required = {"id", "mode", "raw", "quality", "body_frame"}
        allowed_candidate = required | {
            "refined", "description", "handoff_sha256",
            "deep_validate_identity",
        }
        require(required.issubset(candidate) and
                set(candidate).issubset(allowed_candidate),
                "Candidate fields are missing or unsupported")
        identifier = candidate["id"]
        require(isinstance(identifier, str) and IDENTIFIER.fullmatch(identifier),
                f"Invalid candidate id: {identifier}")
        require(identifier not in identifiers,
                f"Duplicate candidate id: {identifier}")
        identifiers.add(identifier)
        require(candidate["mode"] in {"raw", "refined"},
                f"Candidate {identifier} has invalid mode")
        for stage_name in ("raw", "quality", "body_frame"):
            validate_artifact(candidate[stage_name],
                              f"{identifier}.{stage_name}", path)
        if candidate["mode"] == "refined":
            require("refined" in candidate,
                    f"Refined candidate {identifier} has no refined artifact")
            validate_artifact(candidate["refined"],
                              f"{identifier}.refined", path)
        else:
            require("refined" not in candidate,
                    f"Raw candidate {identifier} unexpectedly declares refined")
        handoff = candidate.get("handoff_sha256", "")
        require(not handoff or bool(SHA256.fullmatch(handoff)),
                f"Candidate {identifier} has an invalid handoff digest")

    comparison_ids: set[str] = set()
    for comparison in document.get("comparisons", []):
        require(isinstance(comparison, dict), "Comparisons must be objects")
        required = {
            "id", "baseline", "contender", "require_logical_equality",
            "protected_metrics", "primary_metrics",
            "maximum_paired_median_regression_fraction",
            "minimum_paired_median_improvement_fraction",
        }
        require(set(comparison) == required,
                "Comparison fields do not match schema v1")
        comparison_id = comparison["id"]
        require(isinstance(comparison_id, str) and
                IDENTIFIER.fullmatch(comparison_id) and
                comparison_id not in comparison_ids,
                f"Invalid or duplicate comparison id: {comparison_id}")
        comparison_ids.add(comparison_id)
        require(comparison["baseline"] in identifiers and
                comparison["contender"] in identifiers and
                comparison["baseline"] != comparison["contender"],
                f"Comparison {comparison_id} references invalid candidates")
        require(isinstance(comparison["protected_metrics"], list) and
                isinstance(comparison["primary_metrics"], list) and
                bool(comparison["primary_metrics"]),
                f"Comparison {comparison_id} metrics must be arrays and "
                "primary_metrics must be nonempty")
        for metric in comparison["protected_metrics"] + comparison["primary_metrics"]:
            require(metric in METRIC_PATHS,
                    f"Comparison {comparison_id} uses unsupported metric {metric}")
        require(0.0 <= float(
                    comparison["maximum_paired_median_regression_fraction"]) < 1.0,
                f"Comparison {comparison_id} regression fraction is invalid")
        require(0.0 < float(
                    comparison["minimum_paired_median_improvement_fraction"]) < 1.0,
                f"Comparison {comparison_id} improvement fraction is invalid")
    return document


def cyclic_order(candidate_ids: list[str], repetitions: int) -> list[dict[str, Any]]:
    order: list[dict[str, Any]] = []
    position = 0
    for repetition in range(repetitions):
        shift = repetition % len(candidate_ids)
        rotated = candidate_ids[shift:] + candidate_ids[:shift]
        for ordinal, candidate in enumerate(rotated):
            order.append({
                "position": position,
                "repetition": repetition,
                "ordinal_within_repetition": ordinal,
                "candidate": candidate,
            })
            position += 1
    return order


def build_command(binary: Path, candidate: dict[str, Any], workload: Path,
                  repetition: int, output: Path) -> list[str]:
    command = [
        str(binary), "--mode", candidate["mode"],
        "--raw-store", candidate["raw"]["store"],
        "--raw-run", candidate["raw"]["run"],
        "--raw-digest", candidate["raw"]["manifest_digest"],
        "--quality-store", candidate["quality"]["store"],
        "--quality-run", candidate["quality"]["run"],
        "--quality-digest", candidate["quality"]["manifest_digest"],
        "--body-store", candidate["body_frame"]["store"],
        "--body-run", candidate["body_frame"]["run"],
        "--body-digest", candidate["body_frame"]["manifest_digest"],
        "--workload", str(workload), "--repetition", str(repetition),
        "--output", str(output),
    ]
    if candidate["mode"] == "refined":
        command.extend([
            "--refined-store", candidate["refined"]["store"],
            "--refined-run", candidate["refined"]["run"],
            "--refined-digest", candidate["refined"]["manifest_digest"],
        ])
    if candidate.get("deep_validate_identity", False):
        command.append("--deep-validate-identity")
    return command


def validate_trial(document: dict[str, Any], candidate: dict[str, Any],
                   repetition: int, workload_sha256: str,
                   expected_commit: str) -> None:
    require(document.get("schema_id") == RESULT_SCHEMA and
            document.get("schema_version") == RESULT_VERSION,
            "Trial result schema is incompatible")
    require(document.get("status") == "pass" and
            document.get("gate_failures") == [],
            "Trial did not pass its absolute workload gates")
    require(document.get("mode") == candidate["mode"] and
            document.get("repetition") == repetition,
            "Trial mode or repetition does not match its command")
    selected = candidate.get("refined", candidate["raw"])
    require(document.get("selected_run") == selected["run"] and
            document.get("selected_manifest_digest") ==
                selected["manifest_digest"],
            "Trial selected artifact identity is incorrect")
    require(document.get("workload_sha256") == workload_sha256,
            "Trial workload digest is incorrect")
    require(not document.get("worktree_dirty", True),
            "Trial binary reports a dirty implementation worktree")
    if expected_commit:
        require(document.get("crimson_commit") == expected_commit,
                "Trial Crimson commit does not match the experiment")


def summarize_candidate(trials: list[dict[str, Any]]) -> dict[str, Any]:
    passed = [trial for trial in trials if trial["pass"]]
    summary: dict[str, Any] = {
        "trial_count": len(trials),
        "passed_trial_count": len(passed),
        "all_trials_passed": len(passed) == len(trials),
        "metrics": {},
    }
    if not passed:
        return summary
    for path in METRIC_PATHS:
        values = [float(nested(trial["result"], path)) for trial in passed]
        summary["metrics"][path] = distribution(values)
    summary["crimson_commits"] = sorted({
        trial["result"]["crimson_commit"] for trial in passed
    })
    summary["workload_sha256"] = sorted({
        trial["result"]["workload_sha256"] for trial in passed
    })
    summary["selected_manifest_digests"] = sorted({
        trial["result"]["selected_manifest_digest"] for trial in passed
    })
    return summary


def paired_metric(baseline: list[dict[str, Any]], contender: list[dict[str, Any]],
                  path: str) -> dict[str, Any]:
    baseline_by_repetition = {trial["repetition"]: trial for trial in baseline}
    contender_by_repetition = {trial["repetition"]: trial for trial in contender}
    require(set(baseline_by_repetition) == set(contender_by_repetition),
            f"Paired repetitions disagree for {path}")
    pairs: list[dict[str, Any]] = []
    ratios: list[float] = []
    for repetition in sorted(baseline_by_repetition):
        base = float(nested(baseline_by_repetition[repetition]["result"], path))
        candidate = float(nested(contender_by_repetition[repetition]["result"], path))
        ratio: float | None
        if base == 0.0:
            ratio = 1.0 if candidate == 0.0 else None
        else:
            ratio = candidate / base
            ratios.append(ratio)
        pairs.append({
            "repetition": repetition,
            "baseline": base,
            "contender": candidate,
            "ratio": ratio,
        })
    finite_ratio = statistics.median(ratios) if ratios else 1.0
    any_unbounded_regression = any(
        pair["ratio"] is None and pair["contender"] > 0 for pair in pairs
    )
    return {
        "pairs": pairs,
        "paired_median_ratio": finite_ratio,
        "paired_median_improvement_fraction": 1.0 - finite_ratio,
        "unbounded_zero_baseline_regression": any_unbounded_regression,
    }


def reduce_comparison(comparison: dict[str, Any],
                      trials_by_candidate: dict[str, list[dict[str, Any]]]) -> dict[str, Any]:
    baseline = [trial for trial in trials_by_candidate[comparison["baseline"]]
                if trial["pass"]]
    contender = [trial for trial in trials_by_candidate[comparison["contender"]]
                 if trial["pass"]]
    expected_count = len(trials_by_candidate[comparison["baseline"]])
    complete = len(baseline) == expected_count and len(contender) == expected_count
    logical_failures: list[dict[str, Any]] = []
    if complete and comparison["require_logical_equality"]:
        contender_by_repetition = {
            trial["repetition"]: trial for trial in contender
        }
        for base in baseline:
            other = contender_by_repetition[base["repetition"]]
            for path in LOGICAL_PATHS:
                if nested(base["result"], path) != nested(other["result"], path):
                    logical_failures.append({
                        "repetition": base["repetition"], "path": path,
                        "baseline": nested(base["result"], path),
                        "contender": nested(other["result"], path),
                    })
    protected: dict[str, Any] = {}
    primary: dict[str, Any] = {}
    if complete:
        protected = {
            path: paired_metric(baseline, contender, path)
            for path in comparison["protected_metrics"]
        }
        primary = {
            path: paired_metric(baseline, contender, path)
            for path in comparison["primary_metrics"]
        }
    maximum_ratio = 1.0 + float(
        comparison["maximum_paired_median_regression_fraction"])
    protected_pass = complete and all(
        not metric["unbounded_zero_baseline_regression"] and
        metric["paired_median_ratio"] <= maximum_ratio
        for metric in protected.values()
    )
    minimum_improvement = float(
        comparison["minimum_paired_median_improvement_fraction"])
    material_improvement = complete and any(
        metric["paired_median_improvement_fraction"] >= minimum_improvement
        for metric in primary.values()
    )
    selection_gate_valid = complete and not logical_failures and protected_pass
    contender_selected = selection_gate_valid and material_improvement
    failures: list[str] = []
    if not complete:
        failures.append("incomplete_paired_trials")
    if logical_failures:
        failures.append("logical_inequality")
    if not protected_pass:
        failures.append("protected_metric_regression")
    if not material_improvement:
        failures.append("no_material_primary_improvement")
    return {
        "id": comparison["id"],
        "baseline": comparison["baseline"],
        "contender": comparison["contender"],
        "complete": complete,
        "logical_equality_passed": not logical_failures,
        "logical_failures": logical_failures,
        "protected_metrics": protected,
        "primary_metrics": primary,
        "protected_metrics_passed": protected_pass,
        "material_primary_improvement": material_improvement,
        "selection_gate_valid": selection_gate_valid,
        "selected_candidate": (
            comparison["contender"] if contender_selected
            else comparison["baseline"]
        ),
        "contender_selected": contender_selected,
        "failure_reasons": failures,
        "policy": {
            "paired_statistic": "median of per-repetition contender/baseline ratios",
            "tie_rule": "retain baseline",
            "maximum_regression_fraction":
                comparison["maximum_paired_median_regression_fraction"],
            "minimum_improvement_fraction":
                comparison["minimum_paired_median_improvement_fraction"],
        },
    }


def environment(repo: Path) -> dict[str, Any]:
    return {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": sys.version,
        "repository_commit": command_output(["git", "rev-parse", "HEAD"], repo),
        "repository_status": command_output(
            ["git", "status", "--short", "--untracked-files=no"], repo
        ),
        "mount": command_output(["mount"]),
        "network": command_output(["scutil", "--nwi"]),
        "vpn": command_output(["scutil", "--nc", "list"]),
        "cache_condition": "fresh process; OS/filesystem/server cache uncontrolled",
    }


def run_experiment(args: argparse.Namespace) -> int:
    repo = Path.cwd().resolve()
    manifest_path = args.experiment.resolve()
    experiment = load_experiment(manifest_path)
    binary = (args.binary if args.binary.is_absolute()
              else repo / args.binary).resolve()
    require(binary.is_file() and binary.stat().st_mode & 0o111,
            f"Benchmark binary is unavailable or not executable: {binary}")
    output_dir = args.output_dir.resolve()
    if output_dir.exists() and any(output_dir.iterdir()) and not args.resume:
        raise RuntimeError(
            f"Output directory is not empty; use --resume: {output_dir}")
    output_dir.mkdir(parents=True, exist_ok=True)
    workload = Path(experiment["workload"])
    candidates = {candidate["id"]: candidate
                  for candidate in experiment["candidates"]}
    order = cyclic_order(list(candidates), experiment["repetition_count"])
    started = time.time()
    trials: list[dict[str, Any]] = []

    for item in order:
        candidate = candidates[item["candidate"]]
        stem = (
            f"{item['position']:02d}_repetition_{item['repetition']}_"
            f"{candidate['id']}"
        )
        result_path = output_dir / f"{stem}.json"
        stdout_path = output_dir / f"{stem}.stdout.log"
        stderr_path = output_dir / f"{stem}.stderr.log"
        command = build_command(binary, candidate, workload,
                                item["repetition"], result_path)
        print(
            f"[KeypointV2Experiment] position={item['position'] + 1}/{len(order)} "
            f"repetition={item['repetition']} candidate={candidate['id']}",
            flush=True,
        )
        returncode = 0
        timed_out = False
        elapsed_ms = 0.0
        resumed = args.resume and result_path.is_file()
        if not resumed:
            trial_started = time.monotonic()
            try:
                completed = subprocess.run(
                    command, cwd=repo, check=False, capture_output=True,
                    text=True, timeout=float(experiment["timeout_seconds"]),
                )
                returncode = completed.returncode
                stdout_path.write_text(completed.stdout, encoding="utf-8")
                stderr_path.write_text(completed.stderr, encoding="utf-8")
            except subprocess.TimeoutExpired as error:
                returncode = 124
                timed_out = True
                stdout_path.write_text(error.stdout or "", encoding="utf-8")
                stderr_path.write_text(error.stderr or "", encoding="utf-8")
            elapsed_ms = (time.monotonic() - trial_started) * 1000.0
        trial: dict[str, Any] = {
            **item,
            "command": command,
            "result_path": str(result_path),
            "stdout_path": str(stdout_path),
            "stderr_path": str(stderr_path),
            "subprocess_returncode": returncode,
            "subprocess_elapsed_ms": elapsed_ms,
            "timed_out": timed_out,
            "resumed": resumed,
            "pass": False,
        }
        if result_path.is_file():
            try:
                result = json.loads(result_path.read_text(encoding="utf-8"))
                validate_trial(
                    result, candidate, item["repetition"],
                    experiment["workload_canonical_sha256"],
                    experiment.get("expected_crimson_commit", ""),
                )
                require(returncode == 0,
                        f"Benchmark subprocess returned {returncode}")
                trial["pass"] = True
                trial["result"] = result
            except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
                trial["error"] = str(error)
        else:
            trial["error"] = "Trial produced no structured result"
        trials.append(trial)
        print(
            f"[KeypointV2Experiment] completed candidate={candidate['id']} "
            f"repetition={item['repetition']} pass={int(trial['pass'])} "
            f"elapsed_ms={elapsed_ms:.1f}",
            flush=True,
        )

    trials_by_candidate: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for trial in trials:
        trials_by_candidate[trial["candidate"]].append(trial)
    summaries = {
        candidate: summarize_candidate(candidate_trials)
        for candidate, candidate_trials in trials_by_candidate.items()
    }
    comparisons = [
        reduce_comparison(comparison, trials_by_candidate)
        for comparison in experiment.get("comparisons", [])
    ]
    all_trials_passed = all(trial["pass"] for trial in trials)
    comparisons_complete = all(item["complete"] for item in comparisons)
    comparison_gates_valid = all(
        item["selection_gate_valid"] for item in comparisons
    )
    ordinal_counts: dict[str, dict[str, int]] = {}
    for candidate in candidates:
        counts = Counter(
            str(item["ordinal_within_repetition"])
            for item in order if item["candidate"] == candidate
        )
        ordinal_counts[candidate] = dict(sorted(counts.items()))
    aggregate = {
        "schema_id": AGGREGATE_SCHEMA,
        "schema_version": AGGREGATE_VERSION,
        "status": (
            "pass" if all_trials_passed and comparison_gates_valid else "fail"
        ),
        "classification": "keypoint_v2_fresh_process_experiment",
        "profile_selection_verdict": (
            "not_applicable" if not comparisons else "see_comparisons"
        ),
        "created_unix_seconds": time.time(),
        "wall_elapsed_seconds": time.time() - started,
        "experiment_path": str(manifest_path),
        "experiment_file_sha256": sha256_file(manifest_path),
        "experiment_canonical_sha256": canonical_json_sha256(
            json.loads(manifest_path.read_text(encoding="utf-8"))
        ),
        "workload_path": str(workload),
        "workload_file_sha256": experiment["workload_file_sha256"],
        "workload_canonical_sha256": experiment["workload_canonical_sha256"],
        "binary": str(binary),
        "binary_sha256": sha256_file(binary),
        "environment": environment(repo),
        "candidate_order": order,
        "candidate_ordinal_counts": ordinal_counts,
        "candidate_declarations": experiment["candidates"],
        "trials": trials,
        "candidate_summaries": summaries,
        "comparisons": comparisons,
        "all_trials_passed": all_trials_passed,
        "comparison_trials_complete": comparisons_complete,
        "comparison_gates_valid": comparison_gates_valid,
    }
    aggregate_path = output_dir / "aggregate.json"
    aggregate_path.write_text(
        json.dumps(aggregate, indent=2, allow_nan=False) + "\n",
        encoding="utf-8",
    )
    plot_path = output_dir / "summary.svg"
    plot_aggregate(aggregate, plot_path)
    print(
        f"[KeypointV2Experiment] status={aggregate['status']} "
        f"aggregate={aggregate_path} plot={plot_path}", flush=True,
    )
    return 0 if aggregate["status"] == "pass" else 1


def synthetic_result(candidate: str, repetition: int, factor: float) -> dict[str, Any]:
    physical = {
        "file_reads": int(100 * factor), "file_batch_reads": int(80 * factor),
        "file_bytes": int(10_000_000 * factor), "cache_hits": 500,
        "cache_misses": int(100 * factor), "cache_evictions": 0,
    }
    latency = {
        "p95_ms": 20.0 * factor, "logical_digest": f"digest-{repetition}",
        "physical": physical,
    }
    traversal = {
        **latency, "page_p95_ms": 50.0 * factor,
        "post_warmup_deadline_miss_ratio": 0.0,
    }
    return {
        "schema_id": RESULT_SCHEMA, "schema_version": RESULT_VERSION,
        "status": "pass", "gate_failures": [], "mode": "raw",
        "repetition": repetition, "crimson_commit": "a" * 40,
        "worktree_dirty": False, "workload_sha256": "b" * 64,
        "selected_run": candidate, "selected_manifest_digest": "c" * 64,
        "frame_count": 1000, "row_count": 900, "keypoint_count": 3,
        "archive_open_ms": 10.0 * factor,
        "repository_open_ms": 100.0 * factor,
        "first_presentation_readiness_ms": 200.0 * factor,
        "first_presentation": {
            "elapsed_ms": 50.0 * factor,
            "logical_digest": f"digest-{repetition}", "physical": physical,
        },
        "random_process_first": latency, "random_warm": latency,
        "forward_traversal": traversal, "reverse_traversal": traversal,
        "rapid_seeks": {
            "final_readiness_ms": 20.0 * factor, "stale_visible_frames": 0,
            "physical_transfer_upper_bound": physical,
        },
        "scheduler_metrics": {"timing_by_priority": {"current_frame": {
            "queue_maximum_ms": factor, "service_maximum_ms": 10.0 * factor,
        }}},
        "open_metrics": {"retained_offset_bytes": 8008,
                         "quality_payload_reads": 0},
        "access_metrics": {"quality_payload_read_calls": 0,
                           "read_failures": 0},
        "configured_cache_bytes": 64 * 1024 * 1024,
        "process_physical": physical, "peak_rss_bytes": 100_000_000,
        "close_ms": factor,
    }


def self_test() -> None:
    order = cyclic_order(["a", "b", "c"], 5)
    require(len(order) == 15, "Cyclic order has the wrong size")
    for candidate in ("a", "b", "c"):
        counts = Counter(item["ordinal_within_repetition"]
                         for item in order if item["candidate"] == candidate)
        require(max(counts.values()) - min(counts.values()) <= 1,
                "Cyclic order is not position-balanced")
    trials: dict[str, list[dict[str, Any]]] = {"regular": [], "hybrid": []}
    for repetition in range(5):
        for candidate, factor in (("regular", 1.0), ("hybrid", 0.75)):
            trials[candidate].append({
                "candidate": candidate, "repetition": repetition, "pass": True,
                "result": synthetic_result(candidate, repetition, factor),
            })
    comparison = {
        "id": "regular_vs_hybrid", "baseline": "regular",
        "contender": "hybrid", "require_logical_equality": True,
        "protected_metrics": ["first_presentation_readiness_ms"],
        "primary_metrics": ["process_physical.file_bytes"],
        "maximum_paired_median_regression_fraction": 0.10,
        "minimum_paired_median_improvement_fraction": 0.20,
    }
    reduced = reduce_comparison(comparison, trials)
    require(reduced["contender_selected"],
            "Material synthetic improvement was not selected")
    trials["hybrid"][2]["result"]["forward_traversal"]["logical_digest"] = "bad"
    reduced = reduce_comparison(comparison, trials)
    require(not reduced["contender_selected"] and
            not reduced["logical_equality_passed"] and
            not reduced["selection_gate_valid"],
            "Logical inequality did not fail closed")
    with tempfile.TemporaryDirectory(prefix="crimson-keypoint-runner-") as root:
        aggregate = {
            "status": "pass", "candidate_summaries": {
                candidate: summarize_candidate(values)
                for candidate, values in trials.items()
            },
        }
        output = Path(root) / "plot.svg"
        plot_aggregate(aggregate, output)
        require(output.read_text(encoding="utf-8").startswith("<svg"),
                "Synthetic aggregate plot was not produced")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the keypoint-v2 fresh-process experiment")
    parser.add_argument("--experiment", type=Path)
    parser.add_argument("--binary", type=Path,
                        default=Path("build/macos-arm64-release/") /
                        "keypoint_v2_long_duration_benchmark")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.self_test:
        self_test()
        print("run_keypoint_v2_long_duration_benchmark self-test: PASS")
        return 0
    require(args.experiment is not None and args.output_dir is not None,
            "--experiment and --output-dir are required")
    return run_experiment(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"run_keypoint_v2_long_duration_benchmark: ERROR: {error}",
              file=sys.stderr)
        raise SystemExit(2)
