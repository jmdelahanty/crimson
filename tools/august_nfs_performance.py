#!/usr/bin/env python3
"""Opt-in Linux/NVIDIA/NFS regression suite. Never flushes shared caches.

Fresh GUI processes use isolated config/cwd; seeks reuse one process and repeat
the same deterministic targets. TensorStore file bytes are application reads,
not network bytes. /proc mount counters include other processes on this client.
"""
import argparse
import collections
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[1]
FIXTURE = REPO / "tools/fixtures/august_nfs_performance_v1.json"
CONTINUITY_SCHEMA = "crimson.video_continuity.v1"


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def file_hash(path):
    result = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def command(argv, cwd=None):
    return subprocess.check_output(argv, cwd=cwd, text=True, stderr=subprocess.STDOUT).strip()


def percentile(values, fraction):
    values = sorted(values)
    if not values:
        return None
    return values[min(len(values) - 1, math.ceil(fraction * len(values)) - 1)]


def mount_counters(text, mount):
    """Linux mountstats bytes[4] is server-read bytes; READ[0] is RPC count."""
    active = False
    result = {}
    for line in text.splitlines():
        if line.startswith("device "):
            active = f" mounted on {mount} with fstype nfs" in line
        if not active:
            continue
        fields = line.split()
        if fields and fields[0] == "bytes:" and len(fields) >= 9:
            result["server_read_bytes"] = int(fields[5])
            result["normal_read_bytes"] = int(fields[1])
        if fields and fields[0] == "READ:" and len(fields) >= 9:
            result["read_rpcs"] = int(fields[1])
            result["read_retransmissions"] = int(fields[2]) - int(fields[1])
    return result


def snapshot_mount(mount):
    return mount_counters(Path("/proc/self/mountstats").read_text(), mount)


def make_cases(workload):
    cases = []
    for playback in workload["playback"]:
        for variant in workload["variants"]:
            cases.append({**playback, **variant, "name": playback["name"] + "/" + variant["name"],
                          "kind": "playback", "seek_frames": []})
    cases.append({**workload["startup"], "kind": "startup", "overlays": True,
                  "shading": True, "seek_frames": []})
    cases.append({**workload["timeline_transition"], "kind": "playback", "overlays": True,
                  "shading": True, "seek_frames": []})
    cases.append({"name": "repeated_seeks", "kind": "seeks", "overlays": True,
                  "shading": True, "seek_frames": workload["seek_frames"]})
    return cases


def video_continuity(frames, endpoint_s, thresholds):
    """Measure post-draw front-texture holds, including automatic WaitingMedia.

    The first valid sample starts an observation; it says nothing about how long
    that frame was visible beforehand. A final hold closes at the smoke PASS,
    before shutdown/resource collection. Pauses, manual seeks and rate changes
    break observation epochs. Invalid front textures are counted independently.
    """
    errors = []
    holds = []
    invalid_draws = unknown_draws = invalid_run_ms = invalid_max_ms = active_draws = 0
    held_frame = held_since = held_period_ms = None
    prior_time = prior_rate = None

    def close_hold(until_s):
        nonlocal held_frame, held_since, held_period_ms
        if held_since is not None and until_s > held_since:
            duration_ms = (until_s - held_since) * 1000
            holds.append({"frame": held_frame, "duration_ms": duration_ms,
                          "periods": duration_ms / held_period_ms})
        held_frame = held_since = held_period_ms = None

    if not isinstance(endpoint_s, (int, float)) or isinstance(endpoint_s, bool) or not math.isfinite(endpoint_s):
        errors.append("missing/nonfinite playback endpoint for video continuity")
        return {}, errors
    for frame in frames:
        t = frame["elapsed_s"]
        if not isinstance(t, (int, float)) or isinstance(t, bool) or not math.isfinite(t) or (prior_time is not None and t <= prior_time):
            errors.append("nonmonotonic/nonfinite video continuity timestamp")
            break
        prior_time = t
        if t > endpoint_s:
            break
        if not frame["playing"]:
            close_hold(t)
            invalid_run_ms = 0
            prior_rate = None
            continue
        if not isinstance(frame.get("manual_seek"), bool):
            errors.append("missing/invalid continuity_manual_seek for video continuity (legacy trace cannot qualify)")
            break
        if frame["manual_seek"]:
            close_hold(t)
            invalid_run_ms = 0
            prior_rate = None
            continue
        fps, rate = frame.get("source_video_fps"), frame.get("playback_rate")
        if any(not isinstance(v, (int, float)) or isinstance(v, bool) or not math.isfinite(v) or v <= 0 for v in (fps, rate)):
            errors.append("missing/invalid source_video_fps or playback_rate for video continuity")
            break
        if "visible_video_frame" not in frame:
            errors.append("missing post-draw visible_video_frame for video continuity (legacy trace cannot qualify)")
            break
        visible = frame["visible_video_frame"]
        if visible is not None and (not isinstance(visible, int) or isinstance(visible, bool) or visible < 0):
            errors.append("invalid visible_video_frame for video continuity")
            break
        active_draws += 1
        if prior_rate is not None and (fps, rate) != prior_rate:
            close_hold(t)
            invalid_run_ms = 0
        prior_rate = (fps, rate)
        if visible is None:
            close_hold(t)
            invalid_draws += 1
            if frame.get("front_texture_valid") is not False:
                unknown_draws += 1
            tick = frame["tick_ms"]
            if not isinstance(tick, (int, float)) or isinstance(tick, bool) or not math.isfinite(tick) or tick < 0:
                errors.append("invalid tick_ms for no-visible-video interval")
                break
            invalid_run_ms += tick
            invalid_max_ms = max(invalid_max_ms, invalid_run_ms)
            continue
        invalid_run_ms = 0
        if visible != held_frame:
            close_hold(t)
            held_frame, held_since = visible, t
            held_period_ms = 1000 / (fps * rate)
    if held_since is not None:
        if endpoint_s < held_since:
            errors.append("playback endpoint precedes active video")
        else:
            close_hold(endpoint_s)
    if not active_draws:
        errors.append("no active video continuity samples")
    result = {"max_video_hold_ms": max((h["duration_ms"] for h in holds), default=None),
              "max_video_hold_periods": max((h["periods"] for h in holds), default=None),
              "max_video_hold_frame": max(holds, key=lambda h: h["periods"])["frame"] if holds else None,
              "video_hold_count": len(holds),
              "no_visible_video_fraction": invalid_draws / active_draws if active_draws else None,
              "unknown_visible_video_draws": unknown_draws,
              "no_visible_video_max_ms": invalid_max_ms}
    if unknown_draws:
        errors.append(f"unknown visible video identity on {unknown_draws} active draws")
    if not holds:
        errors.append("no completed visible video hold")
    for key in ("max_video_hold_periods", "no_visible_video_fraction", "no_visible_video_max_ms"):
        value = result[key]
        if value is None or not math.isfinite(value):
            errors.append(f"missing/nonfinite {key}")
        elif value > thresholds[key]:
            errors.append(f"{key}: {value:.4g} > {thresholds[key]:.4g}")
    return result, errors


def summarize(trace, case, thresholds):
    errors = []
    frames, overlay_first, seek_results, resources = [], {}, [], []
    start = final = smoke = None
    seek_pass = False
    with open(trace, encoding="utf-8") as stream:
        for line in stream:
            event = json.loads(line)
            name, detail = event.get("event"), event.get("details", {})
            if name == "performance_probe_start":
                start = event
            elif name == "performance_final_resources":
                final = detail
            elif name == "performance_resources":
                resources.append({**detail, "elapsed_s": event["elapsed_s"],
                                  "playing": event["playback"]["play_video"]})
            elif name == "performance_seek_ready":
                seek_results.append(detail)
            elif name == "performance_seeks_pass":
                seek_pass = detail.get("count") == len(case["seek_frames"])
            elif name in ("performance_seek_timeout", "playback_smoke_timeout"):
                errors.append(name)
            elif name == "playback_smoke_started":
                smoke = event
            elif name == "playback_smoke_pass":
                smoke = {**(smoke or {}), "pass": detail, "pass_elapsed_s": event.get("elapsed_s")}
            elif name == "performance_frame":
                frames.append({**detail, "elapsed_s": event["elapsed_s"],
                               "playing": event["playback"]["play_video"],
                               "presented": event["presenter"]["presented_frame"],
                               "front_texture_valid": event.get("camera_buffer", {}).get("texture_has_valid_frame"),
                               "manual_seek": detail.get("continuity_manual_seek")})
            elif name == "canonical_overlay_present" and event["playback"]["play_video"]:
                frame = event["presenter"]["presented_frame"]
                overlay_first.setdefault(frame, {**detail, "presented_frame": frame})
    if not start or not final or not resources or not frames:
        return {"passed": False, "errors": ["missing required performance telemetry"]}
    if start["details"].get("schema") != "crimson.august_performance_trace.v1":
        errors.append("unsupported trace schema")
    for flag in ("shading", "overlays"):
        if start["details"].get(flag) != case[flag]:
            errors.append(f"wrong {flag} variant")
    measured = frames if case["kind"] == "seeks" else [f for f in frames if f["playing"]]
    if not measured:
        errors.append("no measured rendering frames")
    values = [f["tick_ms"] for f in measured if f["tick_ms"] > 0]
    result = {"render_samples": len(measured), "tick_p95_ms": percentile(values, .95),
              "tick_p99_ms": percentile(values, .99), "tick_max_ms": max(values, default=0),
              "timeline_p99_ms": percentile([f["timeline_ms"] for f in measured], .99),
              "peak_rss_kib": final["peak_rss_kib"], "errors": errors,
              "resources": resources[-1], "scheduler": final["scheduler"]}

    def gate(key):
        value = result.get(key)
        if not isinstance(value, (float, int)) or not math.isfinite(value):
            errors.append(f"missing/nonfinite {key}")
        elif value > thresholds[key]:
            errors.append(f"{key}: {value:.4g} > {thresholds[key]:.4g}")

    for key in ("tick_p95_ms", "tick_p99_ms", "tick_max_ms", "timeline_p99_ms", "peak_rss_kib"):
        gate(key)
    counters = final.get("file_counters", {})
    if any(not isinstance(counters.get(k), int) for k in ("read", "batch_read", "bytes_read")):
        errors.append("missing file-read instrumentation")
    result["file_bytes"] = counters.get("bytes_read")
    result["file_reads"] = (counters.get("read") or 0) + (counters.get("batch_read") or 0)
    # A sampled lower boundary: up to one second of pre-playback work may be
    # included. Whole-process totals remain separate and include initial open.
    paused = [r for r in resources if not r["playing"]]
    if paused and isinstance(counters.get("bytes_read"), int):
        before = paused[-1]["file_counters"].get("bytes_read")
        result["post_prewarm_file_bytes_upper_bound"] = counters["bytes_read"] - before if isinstance(before, int) else None
    if not result["file_bytes"] or result["file_bytes"] < 0:
        errors.append("no measured TensorStore file bytes")
    if final["scheduler"]["failed"] or final["scheduler"]["exceptions"]:
        errors.append("scheduler read/work failures")
    current = [p for p in final["scheduler"]["priorities"] if p["priority"] == "current_frame"]
    result["current_queue_max_ms"] = max((p["queue_max_ms"] for p in current), default=0)
    gate("current_queue_max_ms")
    if case["kind"] == "seeks":
        if not seek_pass or [s["target"] for s in seek_results] != case["seek_frames"]:
            errors.append("seek sequence incomplete or reordered")
        result["seeks"] = seek_results
        if seek_results:
            result["first_ready_ms"] = seek_results[0]["ready_ms"]
            gate("first_ready_ms")
        latencies = [s["ready_ms"] for s in seek_results[1:]]
        result["seek_p95_ms"] = percentile(latencies, .95)
        result["seek_max_ms"] = max(latencies, default=0)
        gate("seek_p95_ms")
        gate("seek_max_ms")
    else:
        if not smoke or "pass" not in smoke:
            errors.append("missing matching playback PASS")
        elif any(smoke["pass"].get(k + "_frame") != case[k] for k in ("start", "end")):
            errors.append("wrong playback range")
        continuity, continuity_errors = video_continuity(frames, smoke.get("pass_elapsed_s") if smoke else None, thresholds)
        result.update(continuity)
        errors.extend(continuity_errors)
        shown = {f["presented"] for f in measured if case["start"] <= f["presented"] <= case["end"]}
        result["unique_video_frames"] = len(shown)
        result["skipped_fraction"] = 1 - len(shown) / (case["end"] - case["start"] + 1)
        gate("skipped_fraction")
        result["no_presented_frame_fraction"] = sum(f["presented"] < 0 for f in measured) / max(1, len(measured))
        missing_run = longest_run = 0.0
        for frame in measured:
            missing_run = missing_run + frame["tick_ms"] if frame["presented"] < 0 else 0.0
            longest_run = max(longest_run, missing_run)
        result["no_presented_frame_max_ms"] = longest_run
        gate("no_presented_frame_fraction")
        gate("no_presented_frame_max_ms")
        if case["end"] not in shown or len(measured) < 60:
            errors.append("missing endpoint or insufficient advancing samples")
        ready = [f for f in measured if f["timeline_ready"] and f["overlays_ready"]]
        start_time = smoke.get("elapsed_s", start["elapsed_s"]) if smoke else start["elapsed_s"]
        result["first_ready_ms"] = (ready[0]["elapsed_s"] - start_time) * 1000 if ready else None
        # Unprewarmed startup is measured separately, not silently excluded.
        ready_time = ready[0]["elapsed_s"] if ready else math.inf
        assessed = measured if case["kind"] != "startup" else [f for f in measured if f["elapsed_s"] >= ready_time]
        result["startup_pending_draws"] = len(measured) - len(assessed)
        result["timeline_late_fraction"] = sum(not f["timeline_ready"] for f in assessed) / max(1, len(assessed))
        gate("timeline_late_fraction")
        if case["kind"] == "startup":
            gate("first_ready_ms")
        if case["overlays"]:
            # -1 denotes no readable presented slot during clip handoff. It is
            # counted/gated above, not mistaken for a missing overlay event.
            assessed_ids = {f["presented"] for f in assessed if f["presented"] >= 0}
            details = [overlay_first[f] for f in assessed_ids if f in overlay_first]
            if len(details) != len(assessed_ids) or not details:
                errors.append("missing overlay draw telemetry")
            late = 0
            positive = 0
            for d in details:
                exact = d["presented_frame"] == d["query_frame"] == d["mask_frame"] == d["contour_frame"]
                good = d["mask_outcome"] in ("drawn", "valid_absent") and d["contour_outcome"] in ("drawn", "valid_absent")
                late += not good
                positive += d["mask_outcome"] == "drawn" and d["contour_outcome"] == "drawn"
                if good and not exact:
                    errors.append("overlay identity mismatch")
                if good and (d["mask_expected_fills"] != d["mask_actual_fills"] or
                             d["contour_expected"] != d["contour_actual"]):
                    errors.append("overlay draw count mismatch")
                if d["mask_outcome"] in ("failed", "draw_failed", "scene_rejected") or d.get("mask_error") or d.get("contour_error"):
                    errors.append("overlay read/render error")
                if d["mask_decoded_cache_bytes"] > d["mask_decoded_cache_byte_budget"]:
                    errors.append("decoded-mask cache budget exceeded")
            result["overlay_late_fraction"] = late / max(1, len(details))
            result["positive_overlay_frames"] = positive
            gate("overlay_late_fraction")
            if not positive:
                errors.append("no positive combined mask/contour draws")
        elif any(r["mask_reads"] or r["contour_reads"] for r in resources):
            errors.append("overlay-off case scheduled mask/contour payload")
        if case["shading"] and not any(f["bout_bands"] > 0 for f in measured):
            errors.append("no positive bout shading")
        if not case["shading"] and any(f["bout_bands"] or f["core_bands"] for f in measured):
            errors.append("disabled shading drawn")
    result["passed"] = not errors
    return result


def aggregate(cases, thresholds):
    grouped = collections.defaultdict(list)
    for case in cases:
        grouped[case["name"]].append(case["result"])
    keys = list(thresholds["comparison_floors"])
    summary = {}
    for name, results in grouped.items():
        summary[name] = {k: statistics.median(r[k] for r in results if isinstance(r.get(k), (int, float)))
                         for k in keys if any(isinstance(r.get(k), (int, float)) for r in results)}
    return summary


def compare(current, baseline, thresholds):
    errors = []
    if current.get("continuity_schema") != baseline.get("continuity_schema") or baseline.get("continuity_schema") != CONTINUITY_SCHEMA:
        errors.append("baseline lacks compatible video-continuity telemetry; legacy results cannot qualify")
    for key in ("workload_digest", "recording_fingerprint", "environment_key", "cache_policy"):
        if current.get(key) != baseline.get(key):
            errors.append(f"baseline incompatible: {key}")
    if set(current["summary"]) != set(baseline["summary"]):
        errors.append("baseline case coverage differs")
    if current.get("source_bindings") != baseline.get("source_bindings"):
        errors.append("baseline selected source bindings differ")
    if errors:
        return errors
    for case, values in current["summary"].items():
        for key, floor in thresholds["comparison_floors"].items():
            if key not in values and key not in baseline["summary"][case]:
                continue
            previous, value = baseline["summary"][case].get(key), values.get(key)
            if previous is None or value is None:
                errors.append(f"baseline missing {case}/{key}")
            elif value > previous + max(floor, abs(previous) * thresholds["relative_increase"]):
                errors.append(f"regression {case}/{key}: {value:.4g} vs {previous:.4g}")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--binary", type=Path, default=REPO / "release/redgui")
    parser.add_argument("--workload", type=Path, default=FIXTURE)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--case", action="append", help="Exact case name; subset is calibration, not qualification")
    parser.add_argument("--baseline", type=Path, help="Previous result.json; requires comparable environment and complete coverage")
    args = parser.parse_args()
    if not 1 <= args.repetitions <= 10:
        parser.error("repetitions must be 1..10")
    archive, binary = args.archive.resolve(), args.binary.resolve()
    workload = json.loads(args.workload.read_text())
    if workload["schema"] != "crimson.august_nfs_performance.v1":
        parser.error("Unsupported workload schema")
    if args.output.exists():
        parser.error("Output directory already exists; use a new directory to preserve evidence")
    mount = json.loads(command(["findmnt", "-J", "-T", str(archive), "-o", "TARGET,SOURCE,FSTYPE,OPTIONS"]))["filesystems"][0]
    if mount["fstype"] not in ("nfs", "nfs4"):
        parser.error("Archive must be on NFS for this suite")
    if subprocess.run(["xdpyinfo"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
        parser.error("No authenticated DISPLAY/XAUTHORITY")
    gpu = command(["nvidia-smi", "--query-gpu=name,driver_version,memory.total", "--format=csv,noheader"])
    machine = {"host": platform.node(), "kernel": platform.release(), "gpu": gpu,
               "mount": mount, "libraries": os.environ.get("LD_LIBRARY_PATH", "")}
    source = {"commit": command(["git", "rev-parse", "HEAD"], REPO),
              "status": command(["git", "status", "--porcelain"], REPO),
              "diff_sha256": hashlib.sha256(subprocess.check_output(["git", "diff", "HEAD"], cwd=REPO)).hexdigest(),
              "untracked": {p: file_hash(REPO / p) for p in command(["git", "ls-files", "--others", "--exclude-standard"], REPO).splitlines() if (REPO / p).is_file()}}
    recording = {"archive": str(archive), "root_metadata": file_hash(archive / "zarr.json"),
                 "clip_index": file_hash(archive.parent.parent / "recording_clip_index.json")}
    recording["selector_metadata"] = {group: file_hash(archive / group / "zarr.json")
        for group in ("detect_runs", "keypoints_runs", "refined_subject_masks_runs",
                      "analysis/eye_angle_runs", "analysis/subject_shape_runs",
                      "analysis/track_kinematics_runs", "analysis/swim_bout_runs")
        if (archive / group / "zarr.json").is_file()}
    all_cases = make_cases(workload)
    cases = [c for c in all_cases if not args.case or c["name"] in args.case]
    if not cases or (args.case and set(args.case) - {c["name"] for c in all_cases}):
        parser.error("Unknown case")
    args.output.mkdir(parents=True)
    report = {"schema": "crimson.august_nfs_performance_result.v1",
              "continuity_schema": CONTINUITY_SCHEMA, "workload": workload,
              "workload_digest": digest(workload), "source": source, "binary": str(binary),
              "binary_sha256": file_hash(binary), "environment": machine, "environment_key": digest(machine),
              "recording": recording, "recording_fingerprint": digest(recording),
              "cache_policy": workload["cache_policy"], "repetitions": args.repetitions,
              "qualified": False, "cases": [], "errors": []}
    (args.output / "environment.json").write_text(json.dumps(report, indent=2))
    for repetition in range(args.repetitions):
        # Counterbalance ordering to reduce bias from warming and server load.
        ordered = cases if repetition % 2 == 0 else list(reversed(cases))
        for case in ordered:
            name = case["name"].replace("/", "_")
            case_dir = args.output / f"r{repetition}_{name}"
            case_dir.mkdir()
            config = {"schema": "crimson.august_performance_case.v1", "shading": case["shading"],
                      "overlays": case["overlays"], "seek_frames": case["seek_frames"]}
            (case_dir / "case.json").write_text(json.dumps(config))
            env = {**os.environ, "CRIMSON_PERFORMANCE_CASE": str((case_dir / "case.json").resolve()),
                   "XDG_CONFIG_HOME": str((case_dir / "config").resolve()),
                   "XDG_CACHE_HOME": str((case_dir / "cache").resolve())}
            for key in ("CRIMSON_CANONICAL_OVERLAY_MODE", "CRIMSON_CANONICAL_BOUT_SHADING"):
                env.pop(key, None)
            argv = [str(binary), "--zarr", str(archive), "--swap-interval", "0",
                    "--frame-cap-fps", str(workload["render_fps"]), "--no-mask-perf-log",
                    "--playback-trace-log", str((case_dir / "trace.jsonl").resolve())]
            if case["kind"] != "seeks":
                argv += ["--playback-smoke", f'{case["start"]}:{case["end"]}',
                         "--playback-smoke-warmup-seconds", str(case["warmup_s"]),
                         "--playback-smoke-timeout", "100"]
            before, started = snapshot_mount(mount["target"]), time.monotonic()
            (case_dir / "command.json").write_text(json.dumps(argv))
            timed_out = False
            with open(case_dir / "app.log", "w") as log:
                process = subprocess.Popen(argv, cwd=case_dir, env=env, stdout=log, stderr=subprocess.STDOUT)
                try:
                    returncode = process.wait(timeout=240)
                except subprocess.TimeoutExpired:
                    timed_out = True
                    process.terminate() # Only the exact process this runner created.
                    try:
                        returncode = process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        returncode = process.wait()
            after = snapshot_mount(mount["target"])
            try:
                result = summarize(case_dir / "trace.jsonl", case, workload["thresholds"])
            except (OSError, ValueError, KeyError, TypeError) as error:
                result = {"passed": False, "errors": [f"invalid telemetry: {error}"]}
            if returncode or timed_out:
                result["errors"].append(f"process exit={returncode}, timeout={timed_out}")
                result["passed"] = False
            result["wall_seconds"] = time.monotonic() - started
            result["mount_wide_nfs_delta"] = {k: after[k] - v for k, v in before.items() if k in after}
            if not result["mount_wide_nfs_delta"] or any(v < 0 for v in result["mount_wide_nfs_delta"].values()):
                result["errors"].append("missing/reset NFS mount counters")
                result["passed"] = False
            result["mount_counter_scope"] = "all processes on this client; not process-attributed"
            record = {"name": case["name"], "repetition": repetition, "case": case, "result": result}
            report["cases"].append(record)
            (case_dir / "result.json").write_text(json.dumps(record, indent=2))
            print(f'{repetition} {case["name"]}: {"PASS" if result["passed"] else "FAIL"} {result["errors"]}', flush=True)
    report["summary"] = aggregate(report["cases"], workload["thresholds"])
    report["source_bindings"] = sorted({json.dumps({k: c["result"].get("resources", {}).get(k)
        for k in ("motion_source", "bout_source", "keypoint_binding", "mask_binding", "shape_binding")}, sort_keys=True)
        for c in report["cases"]})
    for playback in workload["playback"]:
        on = report["summary"].get(playback["name"] + "/overlays_shading")
        off = report["summary"].get(playback["name"] + "/overlays")
        if on and off and "file_bytes" in on and "file_bytes" in off:
            allowance = max(workload["thresholds"]["shading_extra_file_bytes"], off["file_bytes"] * workload["thresholds"]["shading_extra_file_fraction"])
            if on["file_bytes"] > off["file_bytes"] + allowance:
                report["errors"].append(f'{playback["name"]}: shading-on file-read amplification')
    if args.baseline:
        baseline = json.loads(args.baseline.read_text())
        report["baseline"] = {"path": str(args.baseline), "sha256": file_hash(args.baseline)}
        if not baseline.get("qualified"):
            report["errors"].append("baseline is not a qualified full-suite result")
        report["errors"] += compare(report, baseline, workload["thresholds"])
    report["passed"] = all(c["result"]["passed"] for c in report["cases"]) and not report["errors"]
    report["qualified"] = report["passed"] and len(cases) == len(all_cases) and args.repetitions >= workload["qualification_repetitions"]
    (args.output / "result.json").write_text(json.dumps(report, indent=2))
    print(json.dumps({"passed": report["passed"], "qualified": report["qualified"], "errors": report["errors"], "output": str(args.output)}, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
