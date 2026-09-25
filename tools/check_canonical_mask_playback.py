#!/usr/bin/env python3
"""Check exact-frame mask coverage, not merely video progression or late redraws."""
import argparse
import collections
import json


def check(events, start, end, grace_frames=0, minimum_frames=60,
          require_contours=False):
    first = {}
    outcomes = collections.Counter()
    startup = collections.Counter()
    errors = []
    presented = set()
    peak_bytes = 0
    smoke_pass = False
    terminal_frame = -1
    contour_reads_observed = 0
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
        outcome = detail.get("contour_outcome") if require_contours else detail["mask_outcome"]
        if frame < start + grace_frames:
            startup[outcome] += 1
            continue
        presented.add(frame)
        identity = (detail["generation"], event["presenter"]["view_idx"], frame)
        first.setdefault(identity, outcome)
        outcomes[outcome] += 1
        if detail["query_frame"] != frame:
            errors.append(f"frame {frame}: query identity mismatch")
        if require_contours:
            if detail.get("mask_frame") != frame or detail.get("mask_state") not in ("ready", "empty"):
                errors.append(f"frame {frame}: dense mask identity/state mismatch")
            if detail.get("mask_expected_fills") != 0 or detail.get("mask_actual_fills") != 0:
                errors.append(f"frame {frame}: contour-only mode drew fills")
            if not detail.get("contour_enabled"):
                errors.append(f"frame {frame}: contours disabled")
            if outcome not in ("drawn", "valid_absent"):
                errors.append(f"frame {frame}: contour {outcome}: {detail.get('contour_error', '')}")
            else:
                if detail.get("contour_frame") != frame or detail.get("contour_state") not in ("ready", "empty"):
                    errors.append(f"frame {frame}: resolved contour identity/state mismatch")
                expected, actual = detail.get("contour_expected"), detail.get("contour_actual")
                if expected is None or expected != actual or (outcome == "drawn" and expected <= 0) or (outcome == "valid_absent" and expected != 0):
                    errors.append(f"frame {frame}: inconsistent contour draw counts")
                if detail.get("contour_error"):
                    errors.append(f"frame {frame}: contour read error: {detail['contour_error']}")
            reads = detail.get("contour_payload_reads")
            if not isinstance(reads, int) or reads < 0:
                errors.append(f"frame {frame}: missing/invalid contour payload counter")
            else:
                contour_reads_observed = max(contour_reads_observed, reads)
        else:
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
            reads = detail.get("contour_payload_reads")
            if reads is not None and reads != 0:
                errors.append(f"frame {frame}: dense-only request read contour payload")
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
        errors.append("no positive contour draws observed" if require_contours
                      else "no positive mask draws observed")
    if require_contours and contour_reads_observed <= 0:
        errors.append("no contour payload reads observed")
    if not any(frame >= end for frame in presented) or terminal_frame < end:
        errors.append("end frame was not checked")
    last = max(presented, default=start + grace_frames - 1)
    return {"passed": not errors, "start": start, "end": end,
            "require_contours": require_contours,
            "contour_payload_reads_observed": contour_reads_observed,
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
    parser.add_argument("--require-contours", action="store_true")
    args = parser.parse_args()
    if not 0 <= args.start <= args.end or not 0 <= args.grace_frames <= args.end - args.start or args.minimum_frames < 1:
        parser.error("invalid frame range, grace period, or minimum frame count")
    with open(args.trace, encoding="utf-8") as stream:
        result = check((json.loads(line) for line in stream if line.strip()),
                       args.start, args.end, args.grace_frames, args.minimum_frames,
                       args.require_contours)
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
