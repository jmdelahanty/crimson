#!/usr/bin/env python3
"""Compare exact-frame Phase 5M polar captures from Linux and macOS."""

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


def image_path(directory: pathlib.Path, stem: str) -> pathlib.Path:
    for candidate in (
        directory / f"{stem}.png",
        directory / f"{stem}.ui-reference.json.png",
    ):
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(f"no polar capture image in {directory}")


def marker_path(directory: pathlib.Path, stem: str) -> pathlib.Path:
    candidate = directory / f"{stem}.ui-reference.json"
    if not candidate.is_file():
        raise FileNotFoundError(candidate)
    return candidate


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
        raise ValueError(f"pixel ({x}, {y}) outside {image.width}x{image.height}")
    offset = (y * image.width + x) * 4
    return tuple(image.pixels[offset:offset + 4])  # type: ignore[return-value]


def expected_rgb(point: dict[str, Any]) -> tuple[int, int, int]:
    return tuple(int(round(float(value) * 255.0))
                 for value in point["fill"][:3])  # type: ignore[return-value]


def point_raster(image: RgbaImage, polar: dict[str, Any],
                 point: dict[str, Any], channel_tolerance: int
                 ) -> tuple[set[tuple[int, int]], int, tuple[float, float]]:
    screen = point["screen_center"]
    box = polar["screen_box"]
    radius = float(point["radius_px"])
    target = expected_rgb(point)
    minimum_x = max(0, int(math.floor(float(screen["x"]) - radius - 2)))
    maximum_x = min(image.width - 1,
                    int(math.ceil(float(screen["x"]) + radius + 2)))
    minimum_y = max(0, int(math.floor(float(screen["y"]) - radius - 2)))
    maximum_y = min(image.height - 1,
                    int(math.ceil(float(screen["y"]) + radius + 2)))
    mask: set[tuple[int, int]] = set()
    best_delta = 255
    positions: list[tuple[float, float]] = []
    dominant_channel = max(range(3), key=lambda index: target[index])
    for y in range(minimum_y, maximum_y + 1):
        for x in range(minimum_x, maximum_x + 1):
            if math.hypot((x + 0.5) - float(screen["x"]),
                          (y + 0.5) - float(screen["y"])) > radius + 1.0:
                continue
            actual = pixel(image, x, y)
            delta = max(abs(actual[channel] - target[channel])
                        for channel in range(3))
            best_delta = min(best_delta, delta)
            marker_colored = (
                actual[dominant_channel] >= 64 and
                all(actual[dominant_channel] >= actual[channel] + 40
                    for channel in range(3)
                    if channel != dominant_channel)
            )
            if marker_colored:
                local = (x - int(math.floor(float(box["x"]))),
                         y - int(math.floor(float(box["y"]))))
                mask.add(local)
                positions.append(((x + 0.5) - float(box["x"]),
                                  (y + 0.5) - float(box["y"])))
    if not positions:
        centroid = (math.inf, math.inf)
    else:
        centroid = (sum(value[0] for value in positions) / len(positions),
                    sum(value[1] for value in positions) / len(positions))
    return mask, best_delta, centroid


def nearest_distance(point: tuple[int, int], candidates: set[tuple[int, int]]) -> float:
    if not candidates:
        return math.inf
    return min(max(abs(point[0] - other[0]), abs(point[1] - other[1]))
               for other in candidates)


def vector_agreement(left: set[tuple[int, int]], right: set[tuple[int, int]],
                     radius: int) -> tuple[float, float]:
    if not left or not right:
        return 0.0, math.inf
    left_distances = [nearest_distance(point, right) for point in left]
    right_distances = [nearest_distance(point, left) for point in right]
    matched = (sum(distance <= radius for distance in left_distances) +
               sum(distance <= radius for distance in right_distances))
    coverage = matched / (len(left_distances) + len(right_distances))
    return coverage, max(left_distances + right_distances)


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
    tolerances = contract["tolerances"]
    expected_size = contract["logical_size"]
    expected_scene = contract["expected_scene"]
    checks: list[dict[str, Any]] = []

    for platform, expected_platform in (("linux", "linux-opengl"),
                                         ("macos", "macos-metal")):
        marker = markers[platform]
        polar = marker["polar"]
        image = images[platform]
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
            frame=marker.get("presented_frame"),
            stable_frames=marker.get("stable_frames"),
            image_size={"width": image.width, "height": image.height},
        )
        add_check(
            checks, f"{platform}.polar-ready-exact",
            polar.get("ready") is True and polar.get("status") == "ready" and
            polar.get("availability") == "ready" and
            polar.get("requested_frame") == contract["frame"] and
            polar.get("source_frame") == contract["frame"] and
            polar.get("point_count") == expected_scene["point_count"] and
            polar.get("primitive_count") == expected_scene["primitive_count"] and
            polar.get("text_count") == expected_scene["text_count"],
            polar_status=polar.get("status"),
            requested_frame=polar.get("requested_frame"),
            source_frame=polar.get("source_frame"),
            counts={key: polar.get(key) for key in
                    ("point_count", "primitive_count", "text_count")},
        )
        descriptor = polar["descriptor"]
        expected_descriptor = contract["expected_descriptor"]
        descriptor_matches = all(
            descriptor.get(key) == value
            for key, value in expected_descriptor.items()
        ) and descriptor.get("availability") == "ready"
        add_check(checks, f"{platform}.production-descriptor",
                  descriptor_matches, descriptor=descriptor,
                  expected=expected_descriptor)
        add_check(
            checks, f"{platform}.controlled-logical-size",
            abs(float(polar["graph"]["width"]) -
                expected_scene["graph_width"]) <=
            tolerances["source_geometry_px"] and
            abs(float(polar["graph"]["height"]) -
                expected_scene["graph_height"]) <=
            tolerances["source_geometry_px"] and
            all(abs(float(point["radius_px"]) -
                    expected_scene["marker_radius"]) <=
                tolerances["source_geometry_px"]
                for point in polar["points"]),
            graph=polar["graph"], box=polar["box"],
        )
        text_contents = [entry["content"] for entry in polar["text"]]
        add_check(checks, f"{platform}.semantic-text",
                  text_contents == expected_scene["required_text"],
                  contents=text_contents)

    linux_polar = markers["linux"]["polar"]
    macos_polar = markers["macos"]["polar"]
    add_check(
        checks, "cross-platform.scene-semantic-signature",
        linux_polar["semantic_signature"] == macos_polar["semantic_signature"],
        signature_equal=(linux_polar["semantic_signature"] ==
                         macos_polar["semantic_signature"]),
    )
    descriptor_delta = maximum_numeric_delta(
        linux_polar["descriptor"], macos_polar["descriptor"])
    add_check(checks, "cross-platform.descriptor-agreement",
              descriptor_delta <= tolerances["scientific_value"],
              maximum_numeric_delta=descriptor_delta,
              tolerance=tolerances["scientific_value"])

    def local_points(polar: dict[str, Any]) -> list[dict[str, Any]]:
        return [{key: value for key, value in point.items()
                 if key != "screen_center"}
                for point in polar["points"]]

    scene_delta = max(
        maximum_numeric_delta(linux_polar["graph"], macos_polar["graph"]),
        maximum_numeric_delta(linux_polar["center"], macos_polar["center"]),
        maximum_numeric_delta(linux_polar["radius_px"],
                              macos_polar["radius_px"]),
        maximum_numeric_delta(linux_polar["display_max_distance_mm"],
                              macos_polar["display_max_distance_mm"]),
        maximum_numeric_delta(local_points(linux_polar),
                              local_points(macos_polar)),
        maximum_numeric_delta(linux_polar["text"], macos_polar["text"]),
    )
    add_check(checks, "cross-platform.scene-values-and-anchors",
              scene_delta <= tolerances["source_geometry_px"],
              maximum_numeric_delta=scene_delta,
              tolerance=tolerances["source_geometry_px"])
    box_delta = maximum_numeric_delta(
        {key: linux_polar["box"][key] for key in ("width", "height")},
        {key: macos_polar["box"][key] for key in ("width", "height")})
    add_check(checks, "cross-platform.inset-logical-size",
              box_delta <= tolerances["display_anchor_px"],
              maximum_size_delta_px=box_delta,
              linux_box=linux_polar["box"], macos_box=macos_polar["box"])

    for index, (linux_point, macos_point) in enumerate(
            zip(linux_polar["points"], macos_polar["points"])):
        chaser = linux_point["chaser_index"]
        linux_mask, linux_channel_delta, linux_centroid = point_raster(
            images["linux"], linux_polar, linux_point,
            tolerances["non_antialiased_channel_8bit"])
        macos_mask, macos_channel_delta, macos_centroid = point_raster(
            images["macos"], macos_polar, macos_point,
            tolerances["non_antialiased_channel_8bit"])
        add_check(
            checks, f"raster.chaser-{chaser}.opaque-channel",
            max(linux_channel_delta, macos_channel_delta) <=
            tolerances["non_antialiased_channel_8bit"],
            linux_best_channel_delta=linux_channel_delta,
            macos_best_channel_delta=macos_channel_delta,
            tolerance=tolerances["non_antialiased_channel_8bit"],
        )
        coverage, outlier = vector_agreement(
            linux_mask, macos_mask,
            tolerances["vector_coverage_radius_px"])
        add_check(
            checks, f"raster.chaser-{chaser}.vector-coverage",
            coverage >= tolerances["vector_coverage"] and
            outlier <= tolerances["vector_max_outlier_px"],
            coverage=coverage, minimum_coverage=tolerances["vector_coverage"],
            maximum_outlier_px=outlier,
            outlier_limit_px=tolerances["vector_max_outlier_px"],
            linux_pixels=len(linux_mask), macos_pixels=len(macos_mask),
        )
        expected_center = linux_point["center"]
        centroid_delta = max(
            math.hypot(linux_centroid[0] - float(expected_center["x"]),
                       linux_centroid[1] - float(expected_center["y"])),
            math.hypot(macos_centroid[0] - float(expected_center["x"]),
                       macos_centroid[1] - float(expected_center["y"])),
        )
        add_check(
            checks, f"raster.chaser-{chaser}.display-anchor",
            centroid_delta <= tolerances["display_anchor_px"],
            maximum_centroid_delta_px=centroid_delta,
            tolerance_px=tolerances["display_anchor_px"],
            linux_centroid=linux_centroid, macos_centroid=macos_centroid,
        )

    report = {
        "schema": "crimson.phase5m.polar-acceptance-report.v1",
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
        print("phase5m_polar_compare: FAIL " + ", ".join(failed),
              file=sys.stderr)
        return 1
    print(f"phase5m_polar_compare: PASS ({len(checks)} checks)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
