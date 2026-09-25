#!/usr/bin/env python3
"""Compare deterministic Phase 5L Linux and macOS workspace captures."""

from __future__ import annotations

import argparse
import binascii
import json
import math
import pathlib
import re
import struct
import sys
import zlib
from dataclasses import dataclass
from typing import Any, Iterable


@dataclass(frozen=True)
class RgbaImage:
    width: int
    height: int
    pixels: bytes


def load_png(path: pathlib.Path) -> RgbaImage:
    raw = path.read_bytes()
    if raw[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    offset = 8
    width = height = bit_depth = color_type = interlace = None
    compressed = bytearray()
    while offset < len(raw):
        length = struct.unpack_from(">I", raw, offset)[0]
        chunk_type = raw[offset + 4 : offset + 8]
        data = raw[offset + 8 : offset + 8 + length]
        expected_crc = struct.unpack_from(">I", raw, offset + 8 + length)[0]
        actual_crc = binascii.crc32(chunk_type)
        actual_crc = binascii.crc32(data, actual_crc) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise ValueError(f"{path}: invalid {chunk_type!r} CRC")
        offset += length + 12
        if chunk_type == b"IHDR":
            width, height, bit_depth, color_type, _, _, interlace = struct.unpack(
                ">IIBBBBB", data
            )
        elif chunk_type == b"IDAT":
            compressed.extend(data)
        elif chunk_type == b"IEND":
            break
    if None in (width, height, bit_depth, color_type, interlace):
        raise ValueError(f"{path}: missing IHDR")
    if bit_depth != 8 or color_type not in (2, 6) or interlace != 0:
        raise ValueError(
            f"{path}: only non-interlaced 8-bit RGB/RGBA PNGs are supported"
        )
    channels = 4 if color_type == 6 else 3
    scanlines = zlib.decompress(bytes(compressed))
    stride = width * channels
    if len(scanlines) != height * (stride + 1):
        raise ValueError(f"{path}: unexpected decompressed size")
    decoded = bytearray(height * stride)
    source_offset = 0
    for y in range(height):
        filter_type = scanlines[source_offset]
        source_offset += 1
        row = scanlines[source_offset : source_offset + stride]
        source_offset += stride
        destination = y * stride
        previous = destination - stride
        for x, value in enumerate(row):
            left = decoded[destination + x - channels] if x >= channels else 0
            above = decoded[previous + x] if y else 0
            upper_left = (
                decoded[previous + x - channels] if y and x >= channels else 0
            )
            if filter_type == 0:
                reconstructed = value
            elif filter_type == 1:
                reconstructed = value + left
            elif filter_type == 2:
                reconstructed = value + above
            elif filter_type == 3:
                reconstructed = value + ((left + above) // 2)
            elif filter_type == 4:
                predictor = left + above - upper_left
                pa = abs(predictor - left)
                pb = abs(predictor - above)
                pc = abs(predictor - upper_left)
                nearest = left if pa <= pb and pa <= pc else above if pb <= pc else upper_left
                reconstructed = value + nearest
            else:
                raise ValueError(f"{path}: unsupported PNG filter {filter_type}")
            decoded[destination + x] = reconstructed & 0xFF
    if channels == 4:
        return RgbaImage(width, height, bytes(decoded))
    rgba = bytearray(width * height * 4)
    for index in range(width * height):
        rgba[index * 4 : index * 4 + 3] = decoded[index * 3 : index * 3 + 3]
        rgba[index * 4 + 3] = 255
    return RgbaImage(width, height, bytes(rgba))


def sample_bilinear(image: RgbaImage, x: float, y: float) -> tuple[int, int, int, int]:
    x = min(max(x, 0.0), image.width - 1.0)
    y = min(max(y, 0.0), image.height - 1.0)
    x0, y0 = int(math.floor(x)), int(math.floor(y))
    x1, y1 = min(x0 + 1, image.width - 1), min(y0 + 1, image.height - 1)
    tx, ty = x - x0, y - y0
    result = []
    for channel in range(4):
        a = image.pixels[(y0 * image.width + x0) * 4 + channel]
        b = image.pixels[(y0 * image.width + x1) * 4 + channel]
        c = image.pixels[(y1 * image.width + x0) * 4 + channel]
        d = image.pixels[(y1 * image.width + x1) * 4 + channel]
        value = (a * (1.0 - tx) + b * tx) * (1.0 - ty) + (
            c * (1.0 - tx) + d * tx
        ) * ty
        result.append(int(round(value)))
    return tuple(result)  # type: ignore[return-value]


def normalized_crop(
    image: RgbaImage, bounds: dict[str, float], size: int
) -> RgbaImage:
    x0 = float(bounds["x"])
    y0 = float(bounds["y"])
    width = float(bounds["width"])
    height = float(bounds["height"])
    output = bytearray(size * size * 4)
    for y in range(size):
        source_y = y0 + (y + 0.5) * height / size - 0.5
        for x in range(size):
            source_x = x0 + (x + 0.5) * width / size - 0.5
            pixel = sample_bilinear(image, source_x, source_y)
            offset = (y * size + x) * 4
            output[offset : offset + 4] = bytes(pixel)
    return RgbaImage(size, size, bytes(output))


def normalized_window_name(name: str) -> str:
    visible = name.split("###", 1)[0]
    if visible.startswith("Cam2010093_"):
        return "camera"
    return visible


def locate_image(directory: pathlib.Path, stem: str, marker: dict[str, Any]) -> pathlib.Path:
    candidates = [
        directory / f"{stem}.png",
        directory / f"{stem}.ui-reference.json.png",
        pathlib.Path(marker.get("rendered_image", {}).get("path", "")),
    ]
    for candidate in candidates:
        if candidate and candidate.is_file():
            return candidate
    raise FileNotFoundError(f"no rendered image found for {directory / stem}")


def marker_path(directory: pathlib.Path, stem: str) -> pathlib.Path:
    path = directory / f"{stem}.ui-reference.json"
    if not path.is_file():
        raise FileNotFoundError(path)
    return path


def max_bounds_delta(left: dict[str, float], right: dict[str, float]) -> float:
    return max(abs(float(left[key]) - float(right[key])) for key in ("x", "y", "width", "height"))


def add_check(
    checks: list[dict[str, Any]], check_id: str, passed: bool, **details: Any
) -> None:
    checks.append({"id": check_id, "passed": bool(passed), **details})


def find_camera_items(marker: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        item
        for item in marker["semantic_snapshot"]["items"]
        if normalized_window_name(item["window_name"]) == "camera"
    ]


def frame_rows(marker: dict[str, Any]) -> list[int]:
    result = []
    for item in marker["semantic_snapshot"]["items"]:
        if item["window_name"] != "Frames in the buffer":
            continue
        match = re.match(r"Frame (\d+)", item["visible_label"])
        if match:
            result.append(int(match.group(1)))
    return result


def clean_region_metrics(
    left: RgbaImage,
    right: RgbaImage,
    regions: Iterable[list[float]],
    block_size: int,
) -> dict[str, Any]:
    differences: list[float] = []
    for region in regions:
        x0 = int(round(region[0] * left.width))
        y0 = int(round(region[1] * left.height))
        x1 = int(round((region[0] + region[2]) * left.width))
        y1 = int(round((region[1] + region[3]) * left.height))
        for y in range(y0, y1, block_size):
            for x in range(x0, x1, block_size):
                block_x1 = min(x + block_size, x1)
                block_y1 = min(y + block_size, y1)
                pixel_count = (block_x1 - x) * (block_y1 - y)
                for channel in range(3):
                    left_total = 0
                    right_total = 0
                    for sample_y in range(y, block_y1):
                        for sample_x in range(x, block_x1):
                            offset = (sample_y * left.width + sample_x) * 4
                            left_total += left.pixels[offset + channel]
                            right_total += right.pixels[offset + channel]
                    differences.append(
                        abs(left_total / pixel_count - right_total / pixel_count)
                    )
    ordered = sorted(differences)
    percentile_index = min(len(ordered) - 1, int(0.995 * len(ordered)))
    return {
        "block_channel_count": len(differences),
        "block_size": block_size,
        "max_channel_delta": max(differences, default=0),
        "p99_5_channel_delta": ordered[percentile_index] if ordered else 0,
        "mean_channel_delta": sum(differences) / len(differences) if differences else 0.0,
    }


def state_content_ready(state_id: str, platform: str, marker: dict[str, Any]) -> bool:
    if state_id == "overlays":
        overlays = marker.get("overlays", {})
        if platform == "linux":
            return (
                overlays.get("optional_overlay_status") == "optional overlays ready"
                and overlays.get("component_fill_count", 0) > 0
                and overlays.get("contours_drawn", 0) > 0
                and overlays.get("axes_drawn", 0) > 0
                and overlays.get("angle_labels_drawn", 0) > 0
            )
        counts = overlays.get("counts", {})
        return (
            overlays.get("ready") is True
            and counts.get("subject_mask_rasters", 0) > 0
            and counts.get("subject_mask_primitives", 0) > 0
            and counts.get("subject_mask_text", 0) > 0
        )
    if state_id == "crop-preview":
        crop = marker.get("crop", {})
        source_frame = crop.get("source_frame", crop.get("camera_frame"))
        return crop.get("ready") is True and source_frame == marker["presented_frame"]
    if state_id == "analysis-eye":
        analysis = marker.get("analysis", {})
        representation = analysis.get("eye_representation")
        if platform == "linux":
            representations = marker.get("analysis", {})
            return (
                representations.get("state_applied") is True
                and representations.get("eye_representation_key") == "gaze"
            )
        return analysis.get("ready") is True and representation == "gaze"
    if state_id == "stimulus-debug":
        stimulus = marker.get("stimulus", {})
        return (
            stimulus.get("target_frame") == stimulus.get("presented_frame")
            and stimulus.get("presented_frame", -1) >= 0
            and (stimulus.get("loaded") is True or stimulus.get("ready") is True)
        )
    return True


def compare(args: argparse.Namespace) -> dict[str, Any]:
    contract = json.loads(args.contract.read_text())
    tolerances = contract["tolerances"]
    checks: list[dict[str, Any]] = []
    captures: dict[str, dict[str, Any]] = {}
    normalized_images: dict[tuple[str, str], RgbaImage] = {}

    for state in contract["states"]:
        state_id, stem, frame = state["id"], state["stem"], state["frame"]
        markers = {}
        for platform, directory in (("linux", args.linux_dir), ("macos", args.macos_dir)):
            path = marker_path(directory, stem)
            marker = json.loads(path.read_text())
            markers[platform] = marker
            image_path = locate_image(directory, stem, marker)
            image = load_png(image_path)
            captures.setdefault(state_id, {})[platform] = {
                "marker": str(path),
                "image": str(image_path),
            }
            logical = marker.get("logical_content_size", marker["framebuffer_size"])
            add_check(
                checks,
                f"{state_id}.{platform}.capture-contract",
                marker.get("format") == "crimson_ui_reference_v1"
                and marker.get("state") == state_id
                and marker.get("write_contract") == "read-only"
                and marker.get("presented_frame") == frame
                and marker.get("stable_frames", 0) >= 60
                and logical == contract["logical_size"]
                and image.width == contract["logical_size"]["width"]
                and image.height == contract["logical_size"]["height"],
                state=marker.get("state"),
                frame=marker.get("presented_frame"),
                stable_frames=marker.get("stable_frames"),
                logical_size=logical,
                image_size={"width": image.width, "height": image.height},
            )
            add_check(
                checks,
                f"{state_id}.{platform}.state-content-ready",
                state_content_ready(state_id, platform, marker),
            )

            viewport_key = "camera_media" if platform == "linux" else "camera"
            bounds = marker.get("viewports", {}).get(viewport_key)
            if (
                state_id == "workspace"
                and bounds
                and float(bounds.get("width", 0)) > 0
            ):
                normalized_images[(state_id, platform)] = normalized_crop(
                    image, bounds, contract["image_checks"]["canonical_size"]
                )

        linux_windows = markers["linux"]["semantic_snapshot"]["windows"]
        macos_windows = markers["macos"]["semantic_snapshot"]["windows"]
        linux_roles = [normalized_window_name(window["name"]) for window in linux_windows]
        macos_roles = [normalized_window_name(window["name"]) for window in macos_windows]
        add_check(
            checks,
            f"{state_id}.window-presence-order",
            linux_roles == state["windows"] and macos_roles == state["windows"],
            expected=state["windows"],
            linux=linux_roles,
            macos=macos_roles,
        )
        if linux_roles == macos_roles and len(linux_windows) == len(macos_windows):
            bounds_delta = max(
                max_bounds_delta(left["bounds"], right["bounds"])
                for left, right in zip(linux_windows, macos_windows)
            )
            hierarchy_equal = all(
                normalized_window_name(left.get("parent_name", ""))
                == normalized_window_name(right.get("parent_name", ""))
                and left["visible_name"] == right["visible_name"]
                and left["collapsed"] == right["collapsed"]
                for left, right in zip(linux_windows, macos_windows)
            )
        else:
            bounds_delta = math.inf
            hierarchy_equal = False
        add_check(
            checks,
            f"{state_id}.window-hierarchy-labels-bounds",
            hierarchy_equal and bounds_delta <= tolerances["display_anchor_px"],
            hierarchy_labels_equal=hierarchy_equal,
            max_bounds_delta_px=bounds_delta,
            tolerance_px=tolerances["display_anchor_px"],
        )

        linux_camera_items = find_camera_items(markers["linux"])
        macos_camera_items = find_camera_items(markers["macos"])
        surface_contract = contract["shared_controls"]["camera_surface"]
        linux_surface = next(
            (item for item in linux_camera_items if item["label"].startswith(surface_contract["linux_label_prefix"])),
            None,
        )
        macos_surface = next(
            (item for item in macos_camera_items if item["label"] == surface_contract["macos_label"]),
            None,
        )
        surface_delta = (
            max_bounds_delta(
                linux_surface["window_relative_bounds"],
                macos_surface["window_relative_bounds"],
            )
            if linux_surface and macos_surface
            else math.inf
        )
        add_check(
            checks,
            f"{state_id}.camera-surface-anchor",
            surface_delta <= tolerances["display_anchor_px"],
            semantic_labels={
                "linux": linux_surface["label"] if linux_surface else None,
                "macos": macos_surface["label"] if macos_surface else None,
            },
            max_anchor_delta_px=surface_delta,
            tolerance_px=tolerances["display_anchor_px"],
        )
        transport_labels = contract["shared_controls"]["camera_transport_labels"]
        linux_transport = [item for item in linux_camera_items if item["label"] in transport_labels]
        macos_transport = [item for item in macos_camera_items if item["label"] in transport_labels]
        transport_delta = (
            max(
                max_bounds_delta(
                    left["window_relative_bounds"], right["window_relative_bounds"]
                )
                for left, right in zip(linux_transport, macos_transport)
            )
            if len(linux_transport) == len(transport_labels)
            and len(macos_transport) == len(transport_labels)
            else math.inf
        )
        add_check(
            checks,
            f"{state_id}.camera-transport-label-order-anchor",
            [item["label"] for item in linux_transport] == transport_labels
            and [item["label"] for item in macos_transport] == transport_labels
            and transport_delta <= tolerances["display_anchor_px"],
            labels=transport_labels,
            max_anchor_delta_px=transport_delta,
            tolerance_px=tolerances["display_anchor_px"],
        )
        linux_rows = frame_rows(markers["linux"])
        macos_rows = frame_rows(markers["macos"])
        expected_count = min(
            contract["shared_controls"]["buffered_frame_count"],
            len(linux_rows),
            len(macos_rows),
        )
        expected_rows = list(range(frame, frame + expected_count))
        add_check(
            checks,
            f"{state_id}.buffered-frame-identity-order",
            expected_count > 0
            and linux_rows[:expected_count] == expected_rows
            and macos_rows[:expected_count] == expected_rows,
            expected=expected_rows,
            linux=linux_rows[:expected_count],
            macos=macos_rows[:expected_count],
        )

    workspace_linux = normalized_images[("workspace", "linux")]
    workspace_macos = normalized_images[("workspace", "macos")]
    pixel_metrics = clean_region_metrics(
        workspace_linux,
        workspace_macos,
        contract["image_checks"]["camera_clean_source_regions"],
        contract["image_checks"]["camera_block_size"],
    )
    add_check(
        checks,
        "workspace.camera-clean-raster",
        pixel_metrics["max_channel_delta"]
        <= tolerances["non_antialiased_channel_8bit"],
        tolerance_channel_8bit=tolerances["non_antialiased_channel_8bit"],
        **pixel_metrics,
    )

    return {
        "schema": "crimson.phase5l.visual-acceptance-report.v1",
        "passed": all(check["passed"] for check in checks),
        "contract": str(args.contract),
        "linux_directory": str(args.linux_dir),
        "macos_directory": str(args.macos_dir),
        "masks": contract["masks"],
        "required_threshold_fixtures": contract["required_threshold_fixtures"],
        "captures": captures,
        "checks": checks,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--linux-dir", type=pathlib.Path, required=True)
    parser.add_argument("--macos-dir", type=pathlib.Path, required=True)
    parser.add_argument("--contract", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        report = compare(args)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"phase5l_workspace_compare: {error}", file=sys.stderr)
        return 2
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    failed = [check["id"] for check in report["checks"] if not check["passed"]]
    if failed:
        print("phase5l_workspace_compare: FAIL")
        for check_id in failed:
            print(f"  {check_id}")
        return 1
    print(f"phase5l_workspace_compare: PASS ({len(report['checks'])} checks)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
