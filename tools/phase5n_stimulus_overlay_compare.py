#!/usr/bin/env python3
"""Compare exact-frame Phase 5N stimulus overlays on Linux and macOS."""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import sys
from typing import Any

from phase5l_workspace_compare import RgbaImage, load_png


def add_check(checks: list[dict[str, Any]], check_id: str, passed: bool,
              **details: Any) -> None:
    checks.append({"id": check_id, "passed": bool(passed), **details})


def marker_path(directory: pathlib.Path, stem: str) -> pathlib.Path:
    candidate = directory / f"{stem}.ui-reference.json"
    if not candidate.is_file():
        raise FileNotFoundError(candidate)
    return candidate


def image_path(directory: pathlib.Path, stem: str) -> pathlib.Path:
    for candidate in (
        directory / f"{stem}.png",
        directory / f"{stem}.ui-reference.json.png",
    ):
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(f"no stimulus-overlay image in {directory}")


def maximum_numeric_delta(left: Any, right: Any) -> float:
    if isinstance(left, dict) and isinstance(right, dict):
        if set(left) != set(right):
            return math.inf
        return max((maximum_numeric_delta(left[key], right[key])
                    for key in left), default=0.0)
    if isinstance(left, list) and isinstance(right, list):
        if len(left) != len(right):
            return math.inf
        return max((maximum_numeric_delta(a, b)
                    for a, b in zip(left, right)), default=0.0)
    if isinstance(left, (int, float)) and isinstance(right, (int, float)):
        return abs(float(left) - float(right))
    return 0.0 if left == right else math.inf


def pixel(image: RgbaImage, x: int, y: int) -> tuple[int, int, int, int]:
    if not (0 <= x < image.width and 0 <= y < image.height):
        return (0, 0, 0, 0)
    offset = (y * image.width + x) * 4
    return tuple(image.pixels[offset:offset + 4])  # type: ignore[return-value]


def cyan_like(value: tuple[int, int, int, int]) -> bool:
    red, green, blue, _ = value
    return blue >= 120 and green >= 80 and blue >= red + 35 and green >= red + 15


def expected_border_samples(box: dict[str, Any]) -> list[tuple[float, float]]:
    width = float(box["width"])
    height = float(box["height"])
    inset = 6
    samples: list[tuple[float, float]] = []
    for x in range(inset, max(inset, int(math.floor(width)) - inset + 1)):
        samples.extend(((float(x), 0.0), (float(x), height)))
    for y in range(inset, max(inset, int(math.floor(height)) - inset + 1)):
        samples.extend(((0.0, float(y)), (width, float(y))))
    return samples


def border_metrics(image: RgbaImage, box: dict[str, Any], radius: int
                   ) -> tuple[float, set[tuple[int, int]]]:
    origin_x = float(box["x"])
    origin_y = float(box["y"])
    samples = expected_border_samples(box)
    matches = 0
    raster: set[tuple[int, int]] = set()
    for sample_x, sample_y in samples:
        center_x = int(round(origin_x + sample_x))
        center_y = int(round(origin_y + sample_y))
        found = False
        for y in range(center_y - radius, center_y + radius + 1):
            for x in range(center_x - radius, center_x + radius + 1):
                if cyan_like(pixel(image, x, y)):
                    found = True
                    raster.add((int(round(x - origin_x)),
                                int(round(y - origin_y))))
        matches += int(found)
    return (matches / len(samples) if samples else 0.0), raster


def text_pixel_count(image: RgbaImage, box: dict[str, Any]) -> int:
    minimum_x = int(math.floor(float(box["x"]))) + 4
    maximum_x = int(math.ceil(float(box["x"]) + float(box["width"]))) - 4
    minimum_y = int(math.floor(float(box["y"]))) + 4
    maximum_y = int(math.ceil(float(box["y"]) + float(box["height"]))) - 4
    count = 0
    for y in range(minimum_y, maximum_y + 1):
        for x in range(minimum_x, maximum_x + 1):
            # Exclude the two-pixel perimeter where the panel border may land.
            if (x <= minimum_x + 2 or x >= maximum_x - 2 or
                    y <= minimum_y + 2 or y >= maximum_y - 2):
                continue
            count += int(cyan_like(pixel(image, x, y)))
    return count


def nearest_distance(point: tuple[int, int], candidates: set[tuple[int, int]]) -> float:
    if not candidates:
        return math.inf
    return min(max(abs(point[0] - other[0]), abs(point[1] - other[1]))
               for other in candidates)


def raster_agreement(left: set[tuple[int, int]], right: set[tuple[int, int]],
                     radius: int) -> float:
    if not left or not right:
        return 0.0
    distances = ([nearest_distance(point, right) for point in left] +
                 [nearest_distance(point, left) for point in right])
    return sum(distance <= radius for distance in distances) / len(distances)


def scene_values(overlay: dict[str, Any]) -> dict[str, Any]:
    return {
        "requested_frame": overlay["requested_frame"],
        "source_frame": overlay["source_frame"],
        "event_source_frame": overlay["event_source_frame"],
        "step_index": overlay["step_index"],
        "grating_direction_camera_deg": overlay["grating_direction_camera_deg"],
        "event_box": overlay["event_box"],
        "step_box": overlay["step_box"],
        "primitives": overlay["primitives"],
        "text": overlay["text"],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--linux-dir", type=pathlib.Path, required=True)
    parser.add_argument("--macos-dir", type=pathlib.Path, required=True)
    parser.add_argument("--contract", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()

    contract = json.loads(args.contract.read_text())
    stem = contract["stem"]
    markers = {
        "linux": json.loads(marker_path(args.linux_dir, stem).read_text()),
        "macos": json.loads(marker_path(args.macos_dir, stem).read_text()),
    }
    images = {
        "linux": load_png(image_path(args.linux_dir, stem)),
        "macos": load_png(image_path(args.macos_dir, stem)),
    }
    expected_size = contract["logical_size"]
    expected_scene = contract["expected_scene"]
    tolerances = contract["tolerances"]
    checks: list[dict[str, Any]] = []
    border_rasters: dict[str, set[tuple[int, int]]] = {}

    for platform, expected_platform in (("linux", "linux-opengl"),
                                         ("macos", "macos-metal")):
        marker = markers[platform]
        image = images[platform]
        overlay = marker["stimulus_camera_overlay"]
        add_check(
            checks, f"{platform}.capture-contract",
            marker.get("format") == "crimson_ui_reference_v1" and
            marker.get("platform") == expected_platform and
            marker.get("state") == contract["state"] and
            marker.get("write_contract") == "read-only" and
            marker.get("target_frame") == contract["frame"] and
            marker.get("presented_frame") == contract["frame"] and
            marker.get("stable_frames", 0) >= 60 and
            image.width == expected_size["width"] and
            image.height == expected_size["height"],
            platform_marker=marker.get("platform"),
            presented_frame=marker.get("presented_frame"),
            stable_frames=marker.get("stable_frames"),
            image_size={"width": image.width, "height": image.height},
        )
        add_check(
            checks, f"{platform}.overlay-ready-exact",
            overlay.get("ready") is True and
            overlay.get("status") == "ready" and
            overlay.get("availability") == "ready" and
            overlay.get("requested_frame") == contract["frame"] and
            overlay.get("source_frame") == contract["frame"] and
            overlay.get("event_source_frame") == contract["frame"] and
            overlay.get("step_index") is None and
            overlay.get("grating_direction_camera_deg") is None and
            overlay.get("primitive_count") == expected_scene["primitive_count"] and
            overlay.get("text_count") == expected_scene["text_count"],
            status=overlay.get("status"),
            requested_frame=overlay.get("requested_frame"),
            source_frame=overlay.get("source_frame"),
            event_source_frame=overlay.get("event_source_frame"),
        )
        descriptor_delta = maximum_numeric_delta(
            overlay.get("descriptor"), contract["expected_descriptor"])
        add_check(checks, f"{platform}.production-descriptor",
                  descriptor_delta <= tolerances["scientific_value"],
                  descriptor=overlay.get("descriptor"),
                  expected=contract["expected_descriptor"])
        geometry_delta = max(
            maximum_numeric_delta(overlay.get("event_box"),
                                  expected_scene["event_box"]),
            maximum_numeric_delta(overlay.get("step_box"),
                                  expected_scene["step_box"]),
        )
        add_check(checks, f"{platform}.scene-geometry",
                  geometry_delta <= tolerances["source_geometry_px"],
                  maximum_numeric_delta=geometry_delta,
                  event_box=overlay.get("event_box"),
                  step_box=overlay.get("step_box"))
        layer_order = ([entry["layer"] for entry in overlay["primitives"]] +
                       [entry["layer"] for entry in overlay["text"]])
        text = overlay["text"]
        add_check(
            checks, f"{platform}.content-colors-and-layer-order",
            layer_order == expected_scene["layer_order"] and
            len(text) == 1 and
            text[0].get("content") == expected_scene["required_text"] and
            overlay["primitives"][0].get("has_fill") is True and
            overlay["primitives"][0].get("has_stroke") is True,
            layer_order=layer_order,
            text=[entry.get("content") for entry in text],
            primitive=overlay["primitives"][0],
        )
        coverage, raster = border_metrics(
            image, overlay["screen_event_box"],
            tolerances["raster_search_radius_px"])
        border_rasters[platform] = raster
        add_check(checks, f"{platform}.panel-border-raster",
                  coverage >= tolerances["minimum_border_coverage"],
                  coverage=coverage,
                  minimum=tolerances["minimum_border_coverage"],
                  matched_pixels=len(raster),
                  screen_event_box=overlay["screen_event_box"])
        text_pixels = text_pixel_count(image, overlay["screen_event_box"])
        add_check(checks, f"{platform}.event-text-raster",
                  text_pixels >= tolerances["minimum_text_pixels"],
                  cyan_like_text_pixels=text_pixels,
                  minimum=tolerances["minimum_text_pixels"])

    linux_overlay = markers["linux"]["stimulus_camera_overlay"]
    macos_overlay = markers["macos"]["stimulus_camera_overlay"]
    signature_equal = (linux_overlay["semantic_signature"] ==
                       macos_overlay["semantic_signature"])
    add_check(checks, "cross-platform.semantic-signature", signature_equal,
              equal=signature_equal)
    scene_delta = maximum_numeric_delta(scene_values(linux_overlay),
                                        scene_values(macos_overlay))
    add_check(checks, "cross-platform.scene-values",
              scene_delta <= tolerances["source_geometry_px"],
              maximum_numeric_delta=scene_delta,
              tolerance=tolerances["source_geometry_px"])
    raster_coverage = raster_agreement(
        border_rasters["linux"], border_rasters["macos"],
        tolerances["raster_search_radius_px"])
    add_check(
        checks, "cross-platform.panel-border-raster",
        raster_coverage >= tolerances["minimum_cross_platform_border_coverage"],
        coverage=raster_coverage,
        minimum=tolerances["minimum_cross_platform_border_coverage"],
        linux_pixels=len(border_rasters["linux"]),
        macos_pixels=len(border_rasters["macos"]),
    )

    report = {
        "schema": "crimson.phase5n.stimulus-overlay-acceptance-report.v1",
        "passed": all(check["passed"] for check in checks),
        "contract": str(args.contract),
        "inputs": {
            "linux_marker": str(marker_path(args.linux_dir, stem)),
            "linux_image": str(image_path(args.linux_dir, stem)),
            "macos_marker": str(marker_path(args.macos_dir, stem)),
            "macos_image": str(image_path(args.macos_dir, stem)),
        },
        "tolerances": tolerances,
        "masks": contract["masks"],
        "required_threshold_fixtures": contract["required_threshold_fixtures"],
        "checks": checks,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    failed = [check["id"] for check in checks if not check["passed"]]
    if failed:
        print("phase5n_stimulus_overlay_compare: FAIL " + ", ".join(failed),
              file=sys.stderr)
        return 1
    print(f"phase5n_stimulus_overlay_compare: PASS ({len(checks)} checks)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
