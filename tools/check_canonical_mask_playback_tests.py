#!/usr/bin/env python3
import copy
import unittest
from check_canonical_mask_playback import check


def sample(frame, outcome="drawn"):
    count = 4 if outcome == "drawn" else 0
    return {"event": "canonical_overlay_present", "playback": {"play_video": True},
            "presenter": {"presented_frame": frame, "view_idx": 0},
            "details": {"generation": 1, "query_frame": frame, "mask_frame": frame,
                        "mask_state": "ready", "mask_outcome": outcome, "mask_error": "",
                        "mask_enabled": True, "mask_expected_fills": count, "mask_actual_fills": count,
                        "mask_decoded_cache_bytes": 16, "mask_decoded_cache_byte_budget": 32}}


class CoverageTests(unittest.TestCase):
    def events(self):
        return [sample(0), sample(1, "valid_absent"), sample(2),
                {"event": "playback_smoke_pass", "details": {"start_frame": 0, "end_frame": 2}}]

    def run_check(self, events, grace=0):
        return check(events, 0, 2, grace_frames=grace, minimum_frames=2)

    def test_exact_draw_and_valid_absence(self):
        result = self.run_check(self.events())
        self.assertTrue(result["passed"])
        self.assertEqual(result["first_draw_outcomes"], {"drawn": 2, "valid_absent": 1})

    def test_late_redraw_does_not_hide_first_miss(self):
        events = self.events()
        events.insert(0, sample(0, "pending"))
        result = self.run_check(events)
        self.assertFalse(result["passed"])
        self.assertEqual(result["first_draw_outcomes"]["pending"], 1)
        self.assertTrue(self.run_check(events, grace=1)["passed"])

    def test_clock_may_skip_a_video_frame_at_endpoint(self):
        events = self.events()
        events[2] = sample(3)
        events[-1]["details"]["presented_frame"] = 3
        result = self.run_check(events)
        self.assertTrue(result["passed"])
        self.assertEqual(result["unpresented_video_frames"], 1)

    def test_fail_closed_on_identity_disabled_budget_draw_errors(self):
        for field, value in (("mask_frame", 9), ("query_frame", 9), ("mask_enabled", False),
                             ("mask_state", "failed"), ("mask_actual_fills", 0),
                             ("mask_decoded_cache_bytes", 33), ("mask_outcome", "draw_failed")):
            with self.subTest(field=field):
                events = copy.deepcopy(self.events())
                events[0]["details"][field] = value
                self.assertFalse(self.run_check(events)["passed"])

    def test_cannot_pass_without_advancing_video_or_positive_masks(self):
        events = self.events()
        self.assertFalse(self.run_check(events[:-1])["passed"])
        self.assertFalse(self.run_check([events[0], events[0], events[-1]])["passed"])
        for event in events[:-1]:
            event["playback"]["play_video"] = False
        self.assertFalse(self.run_check(events)["passed"])
        events = [sample(i, "valid_absent") for i in range(3)] + [events[-1]]
        self.assertFalse(self.run_check(events)["passed"])


if __name__ == "__main__":
    unittest.main()
