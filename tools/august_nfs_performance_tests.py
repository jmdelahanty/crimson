#!/usr/bin/env python3
import copy
import json
from pathlib import Path
import tempfile
import unittest

from august_nfs_performance import CONTINUITY_SCHEMA, FIXTURE, compare, make_cases, mount_counters, summarize, video_continuity


class PerformanceTests(unittest.TestCase):
    def setUp(self):
        self.workload = json.loads(FIXTURE.read_text())
        self.limits = self.workload["thresholds"]
        self.case = {"kind": "playback", "start": 0, "end": 59, "warmup_s": 8,
                     "shading": True, "overlays": True, "seek_frames": []}

    def events(self):
        def event(name, details, elapsed=0, frame=0):
            return {"event": name, "details": details, "elapsed_s": elapsed,
                    "playback": {"play_video": True}, "presenter": {"presented_frame": frame},
                    "seek": {"state": "Idle"},
                    "camera_buffer": {"texture_has_valid_frame": True, "last_uploaded_frame": frame}}
        resource = {"file_counters": {"read": 10, "batch_read": 1, "bytes_read": 1000},
                    "peak_rss_kib": 10000, "mask_reads": 4, "contour_reads": 4,
                    "scheduler": {"failed": 0, "exceptions": 0, "priorities": [
                        {"priority": "current_frame", "queue_max_ms": 2}]}}
        result = [event("performance_probe_start", {"schema": "crimson.august_performance_trace.v1",
                      "shading": True, "overlays": True}),
                  event("playback_smoke_started", {}, 1),
                  event("performance_resources", resource)]
        for frame in range(60):
            result += [event("performance_frame", {"tick_ms": 16.7, "frame_loop_ms": 16.7,
                        "timeline_ms": .6, "timeline_ready": True, "overlays_ready": True,
                        "bout_bands": 3, "core_bands": 3,
                        "visible_video_frame": frame, "source_video_fps": 30.0,
                        "playback_rate": 1.0, "continuity_manual_seek": False}, 9 + frame / 30, frame),
                       event("canonical_overlay_present", {"query_frame": frame, "mask_frame": frame,
                        "contour_frame": frame, "mask_outcome": "drawn", "contour_outcome": "drawn",
                        "mask_expected_fills": 4, "mask_actual_fills": 4,
                        "contour_expected": 4, "contour_actual": 4,
                        "mask_error": "", "contour_error": "", "mask_decoded_cache_bytes": 10,
                        "mask_decoded_cache_byte_budget": 100}, 9 + frame / 30, frame)]
        result += [event("playback_smoke_pass", {"start_frame": 0, "end_frame": 59}, 11),
                   event("performance_final_resources", resource)]
        return result

    def check(self, events, case=None):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "trace.jsonl"
            path.write_text("".join(json.dumps(e) + "\n" for e in events))
            return summarize(path, case or self.case, self.limits)

    def test_positive_and_wrong_pass_range(self):
        self.assertTrue(self.check(self.events())["passed"])
        events = self.events()
        events[-2]["details"]["end_frame"] = 60
        self.assertIn("wrong playback range", self.check(events)["errors"])

    def test_missing_telemetry_fails_closed(self):
        for event_name in ("performance_probe_start", "performance_frame", "performance_resources",
                           "performance_final_resources", "playback_smoke_pass", "canonical_overlay_present"):
            with self.subTest(event=event_name):
                self.assertFalse(self.check([e for e in self.events() if e["event"] != event_name])["passed"])

    def test_skipped_frames_are_a_failure_gate(self):
        events = [e for e in self.events() if not (e["event"] == "performance_frame" and e["presenter"]["presented_frame"] == 10)]
        self.assertTrue(any("skipped_fraction" in s for s in self.check(events)["errors"]))

    def test_late_redraw_cannot_hide_first_miss(self):
        events = self.events()
        first = next(e for e in events if e["event"] == "canonical_overlay_present")
        pending = copy.deepcopy(first)
        pending["details"]["mask_outcome"] = "pending"
        events.insert(events.index(first), pending)
        self.assertTrue(any("overlay_late_fraction" in s for s in self.check(events)["errors"]))

    def test_identity_and_counts_fail(self):
        for key in ("mask_frame", "contour_actual", "mask_actual_fills"):
            events = self.events()
            next(e for e in events if e["event"] == "canonical_overlay_present")["details"][key] = 999
            self.assertFalse(self.check(events)["passed"])

    def test_render_and_memory_gates(self):
        for key, value in (("tick_ms", 80), ("timeline_ms", 10)):
            events = self.events()
            for e in events:
                if e["event"] == "performance_frame":
                    e["details"][key] = value
            self.assertFalse(self.check(events)["passed"])
        events = self.events()
        events[-1]["details"]["peak_rss_kib"] = self.limits["peak_rss_kib"] + 1
        self.assertFalse(self.check(events)["passed"])

    def test_matching_overlays_on_the_wrong_video_frame_fail(self):
        events = self.events()
        detail = next(e for e in events if e["event"] == "canonical_overlay_present")["details"]
        for key in ("query_frame", "mask_frame", "contour_frame"):
            detail[key] = 999
        self.assertIn("overlay identity mismatch", self.check(events)["errors"])

    def test_handoff_sentinel_is_accounted_separately(self):
        events = self.events()
        extra = copy.deepcopy(next(e for e in events if e["event"] == "performance_frame"))
        extra["presenter"]["presented_frame"] = -1
        extra["details"]["visible_video_frame"] = 0
        extra["elapsed_s"] += .0167
        events.insert(4, extra)
        result = self.check(events)
        self.assertTrue(result["passed"])
        self.assertEqual(result["no_presented_frame_max_ms"], 16.7)
        extra["details"]["tick_ms"] = 600
        self.assertFalse(self.check(events)["passed"])

    def test_visible_hold_counts_repeated_draws_and_terminal_endpoint(self):
        frames = [{"elapsed_s": i / 60, "playing": True, "seek_state": "Idle",
                   "manual_seek": False, "source_video_fps": 30, "playback_rate": 1,
                   "visible_video_frame": 9 if i < 20 else 10, "tick_ms": 1000 / 60}
                  for i in range(22)]
        result, errors = video_continuity(frames, 22 / 60, self.limits)
        self.assertTrue(any("max_video_hold_periods" in e for e in errors))
        self.assertAlmostEqual(result["max_video_hold_ms"], 1000 / 3)
        self.assertAlmostEqual(result["max_video_hold_periods"], 10)
        self.assertEqual(result["max_video_hold_frame"], 9)
        frames = [{**f, "visible_video_frame": 9} for f in frames[:2]]
        result, errors = video_continuity(frames, .4, self.limits)
        self.assertAlmostEqual(result["max_video_hold_ms"], 400)
        self.assertTrue(any("max_video_hold_periods" in e for e in errors))

    def test_invalid_front_is_separate_from_presenter_sentinel(self):
        events = self.events()
        frame = next(e for e in events if e["event"] == "performance_frame")
        frame["presenter"]["presented_frame"] = -1
        self.assertEqual(self.check(events)["no_visible_video_fraction"], 0)
        frame["details"]["visible_video_frame"] = None
        frame["camera_buffer"]["texture_has_valid_frame"] = False
        result = self.check(events)
        self.assertGreater(result["no_visible_video_fraction"], 0)
        self.assertEqual(result["no_visible_video_max_ms"], 16.7)
        frame["camera_buffer"]["texture_has_valid_frame"] = True
        self.assertTrue(any("unknown visible video identity" in e for e in self.check(events)["errors"]))

    def test_manual_pause_warmup_and_rate_changes_break_epochs(self):
        frames = [{"elapsed_s": i / 60, "playing": True, "seek_state": "Idle",
                   "manual_seek": False, "source_video_fps": 30, "playback_rate": 1,
                   "visible_video_frame": 8, "tick_ms": 1000 / 60} for i in range(20)]
        frames[4]["manual_seek"] = True
        frames[8]["manual_seek"] = True
        frames[12]["playing"] = False
        frames[16]["playback_rate"] = 2
        for i in range(17, 20):
            frames[i]["playback_rate"] = 2
        for i in range(18, 20):
            frames[i]["visible_video_frame"] = 9
        result, errors = video_continuity(frames, 20 / 60, self.limits)
        self.assertFalse(errors)
        self.assertLessEqual(result["max_video_hold_periods"], 2.5)
        for f in frames:
            f["manual_seek"] = False
            f["playing"] = True
            f["playback_rate"] = 1
            f["visible_video_frame"] = 8
            frame_number = round(f["elapsed_s"] * 60)
            f["seek_state"] = "WaitingMedia" if 6 <= frame_number < 10 else (
                "WaitingCameras" if 10 <= frame_number < 14 else (
                    "WaitingStimulus" if 14 <= frame_number < 18 else "Idle"))
        result, errors = video_continuity(frames, 20 / 60, self.limits)
        self.assertGreater(result["max_video_hold_periods"], 2.5)
        self.assertTrue(errors)

    def test_missing_or_malformed_continuity_telemetry_fails_closed(self):
        for key, value in (("visible_video_frame", "missing"), ("visible_video_frame", -2),
                           ("source_video_fps", 0), ("source_video_fps", float("nan")),
                           ("source_video_fps", True), ("playback_rate", None),
                           ("playback_rate", float("nan")), ("playback_rate", float("inf")),
                           ("continuity_manual_seek", "missing"),
                           ("continuity_manual_seek", 1)):
            with self.subTest(key=key, value=value):
                events = self.events()
                detail = next(e["details"] for e in events if e["event"] == "performance_frame")
                if value == "missing":
                    del detail[key]
                else:
                    detail[key] = value
                self.assertFalse(self.check(events)["passed"])

    def test_null_file_metrics_are_not_zero_reads(self):
        events = self.events()
        events[-1]["details"]["file_counters"]["bytes_read"] = None
        self.assertFalse(self.check(events)["passed"])

    def test_startup_is_explicitly_accounted(self):
        events = self.events()
        for e in events:
            if e["event"] == "performance_frame" and e["presenter"]["presented_frame"] < 20:
                e["details"]["overlays_ready"] = False
        result = self.check(events, {**self.case, "kind": "startup"})
        self.assertTrue(result["passed"])
        self.assertEqual(result["startup_pending_draws"], 20)
        self.assertGreater(result["first_ready_ms"], 8000)

    def test_seek_order_completion_and_latency(self):
        case = {**self.case, "kind": "seeks", "seek_frames": [0, 500, 0]}
        events = self.events()
        events += [{"event": "performance_seek_ready", "details": {"target": f, "ready_ms": 200}}
                   for f in case["seek_frames"]]
        events += [{"event": "performance_seeks_pass", "details": {"count": 3}}]
        self.assertTrue(self.check(events, case)["passed"])
        events[-2]["details"]["ready_ms"] = 6000
        self.assertFalse(self.check(events, case)["passed"])
        events[-2]["details"]["target"] = 501
        self.assertFalse(self.check(events, case)["passed"])

    def test_mount_scope_and_read_bytes(self):
        text = "device srv:/g mounted on /groups/johnson with fstype nfs4 statvers=1.1\n bytes: 10 0 0 0 20 0 0 0\n READ: 3 4 0 1 2 3 4 5\ndevice other mounted on /else with fstype nfs4 statvers=1.1\n bytes: 99 0 0 0 99 0 0 0"
        values = mount_counters(text, "/groups/johnson")
        self.assertEqual(values, {"normal_read_bytes": 10, "server_read_bytes": 20, "read_rpcs": 3, "read_retransmissions": 1})

    def test_baseline_compatibility_and_regressions(self):
        base = {k: "same" for k in ("workload_digest", "recording_fingerprint", "environment_key", "cache_policy")}
        base["continuity_schema"] = CONTINUITY_SCHEMA
        base["summary"] = {"a": {"tick_p99_ms": 20}}
        self.assertFalse(compare(base, base, self.limits))
        current = copy.deepcopy(base)
        current["summary"]["a"]["tick_p99_ms"] = 30
        self.assertTrue(compare(current, base, self.limits))
        current = copy.deepcopy(base)
        current["environment_key"] = "different"
        self.assertTrue(compare(current, base, self.limits))
        old = copy.deepcopy(base)
        del old["continuity_schema"]
        self.assertTrue(any("legacy results cannot qualify" in e for e in compare(base, old, self.limits)))

    def test_matrix_covers_all_variants(self):
        cases = make_cases(self.workload)
        self.assertEqual(len(cases), 9)
        self.assertEqual(len({c["name"] for c in cases}), 9)
        page = next(c for c in cases if c["name"] == "timeline_page_boundary")
        self.assertLess(page["start"], 27 * 2048)
        self.assertGreater(page["end"], 27 * 2048)


if __name__ == "__main__":
    unittest.main()
