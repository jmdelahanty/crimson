#!/usr/bin/env python3
"""Check exact-frame mask coverage, not merely video progression or late redraws."""
import argparse
import collections
import json


def check(events, start, end, grace_frames=0, minimum_frames=60):
    first = {}
    outcomes = collections.Counter()
    startup = collections.Counter()
    errors = []
    presented = set()
    peak_bytes = 0
    smoke_pass = False
    terminal_frame = -1
    for event in events:
        if event.get("event") == "playback_smoke_pass":
            detail = event.get("details", {})
            smoke_pass |= detail.get("start_frame") == start and detail.get("end_frame") == end
            if detail.get("start_frame") == start and detail.get("end_frame") == end:
                terminal_frame = detail.get("presented_frame", end)
        if event.get("event") != "canonical_overlay_present" or not event.get("playback", {}).get("play_video"):
            continue
        detail = event["details"]
        frame = event["presenter"]["presented_frame"]
        if frame < start:
            continue
        outcome = detail["mask_outcome"]
        if frame < start + grace_frames:
            startup[outcome] += 1
            continue
        presented.add(frame)
        identity = (detail["generation"], event["presenter"]["view_idx"], frame)
        first.setdefault(identity, outcome)
        outcomes[outcome] += 1
        if detail["query_frame"] != frame:
            errors.append(f"frame {frame}: query identity mismatch")
        if not detail["mask_enabled"]:
            errors.append(f"frame {frame}: masks disabled")
        if outcome not in ("drawn", "valid_absent"):
            errors.append(f"frame {frame}: {outcome}: {detail.get('mask_error', '')}")
        else:
            if detail["mask_frame"] != frame or detail["mask_state"] not in ("ready", "empty"):
                errors.append(f"frame {frame}: resolved mask identity/state mismatch")
            expected, actual = detail["mask_expected_fills"], detail["mask_actual_fills"]
            if expected != actual or (outcome == "drawn" and expected <= 0) or (outcome == "valid_absent" and expected != 0):
                errors.append(f"frame {frame}: inconsistent draw counts")
        retained = detail["mask_decoded_cache_bytes"]
        budget = detail["mask_decoded_cache_byte_budget"]
        peak_bytes = max(peak_bytes, retained)
        if retained < 0 or budget <= 0 or retained > budget:
            errors.append(f"frame {frame}: decoded cache exceeds byte budget")
    if not smoke_pass:
        errors.append("missing matching playback smoke PASS")
    if len(presented) < minimum_frames:
        errors.append("insufficient advancing frames")
    if not outcomes["drawn"]:
        errors.append("no positive mask draws observed")
    if not any(frame >= end for frame in presented) or terminal_frame < end:
        errors.append("end frame was not checked")
    last = max(presented, default=start + grace_frames - 1)
    return {"passed": not errors, "start": start, "end": end,
            "grace_frames": grace_frames, "unique_presented_frames": len(presented),
            "last_presented_frame": last,
            "unpresented_video_frames": last - (start + grace_frames) + 1 - len(presented),
            "first_draw_outcomes": dict(collections.Counter(first.values())),
            "all_draw_outcomes": dict(outcomes), "startup_draw_outcomes": dict(startup),
            "peak_decoded_cache_bytes": peak_bytes, "error_count": len(errors), "errors": errors[:20]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace")
    parser.add_argument("start", type=int)
    parser.add_argument("end", type=int)
    parser.add_argument("--grace-frames", type=int, default=0)
    parser.add_argument("--minimum-frames", type=int, default=60)
    args = parser.parse_args()
    if not 0 <= args.start <= args.end or not 0 <= args.grace_frames <= args.end - args.start or args.minimum_frames < 1:
        parser.error("invalid frame range, grace period, or minimum frame count")
    with open(args.trace, encoding="utf-8") as stream:
        result = check((json.loads(line) for line in stream if line.strip()),
                       args.start, args.end, args.grace_frames, args.minimum_frames)
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
