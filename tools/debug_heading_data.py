#!/usr/bin/env python3
import argparse
import json
import math
import os
from typing import Any, List, Optional

import numpy as np
import zarr

try:  # zarr >= 3
    from zarr.core.group import Group as ZarrGroup
except Exception:  # pragma: no cover
    try:  # zarr <= 2
        from zarr.hierarchy import Group as ZarrGroup  # type: ignore
    except Exception:
        ZarrGroup = object  # type: ignore


def _collect_group_keys(group: "ZarrGroup") -> List[str]:
    if hasattr(group, "group_keys"):
        return list(group.group_keys())
    try:
        return [name for name in group.keys() if _is_group(group[name])]
    except Exception:
        return []


def _is_group(obj: Any) -> bool:
    return hasattr(obj, "group_keys") or hasattr(obj, "keys")


def find_latest_run(group: "ZarrGroup") -> Optional[str]:
    """Try to locate the latest run name from group attributes or contents."""
    attrs = getattr(group, "attrs", {})
    latest = getattr(attrs, "get", lambda _key, _default=None: None)("latest_run")
    if latest:
        return latest

    # Fall back to lexicographic ordering of subgroup names.
    names: List[str] = _collect_group_keys(group)
    return sorted(names)[-1] if names else None


def find_swim_bladder_index(labels: List[str]) -> int:
    for idx, label in enumerate(labels):
        lowered = label.lower()
        if "swim" in lowered or "bladder" in lowered:
            return idx
    return 0


def load_dataset(group: "ZarrGroup", name: str) -> Optional[np.ndarray]:
    if name in group:
        return group[name][:]
    return None


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Inspect heading/keypoint data inside a palette Zarr store."
    )
    parser.add_argument(
        "zarr_path",
        help="Path to the Zarr store (directory or zip)",
    )
    parser.add_argument(
        "--frame",
        type=int,
        default=None,
        help="Filter to a specific camera frame index",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=20,
        help="Maximum number of entries to display",
    )
    parser.add_argument(
        "--detect-run",
        default=None,
        help="Detection run name (defaults to store metadata/latest run)",
    )
    parser.add_argument(
        "--keypoints-run",
        default=None,
        help="Keypoints run name (defaults to store metadata/latest run)",
    )
    parser.add_argument(
        "--crop-run",
        default=None,
        help="Crop run name for ROI offsets (defaults to source_crop_run attribute or best match)",
    )
    args = parser.parse_args()

    store_path = os.path.abspath(args.zarr_path)
    root = zarr.open(store_path, mode="r")

    if "keypoints_runs" not in root:
        raise SystemExit("No keypoints_runs group found in store")

    keypoints_group = root["keypoints_runs"]
    keypoints_run = args.keypoints_run or find_latest_run(keypoints_group)
    if not keypoints_run:
        raise SystemExit("Could not determine a keypoints run to inspect")
    kp_run = keypoints_group[keypoints_run]

    frame_indices = kp_run["frame_indices"][:]
    roi_count = frame_indices.shape[0]
    heading_values = load_dataset(kp_run, "heading")
    if heading_values is None:
        heading_values = np.zeros(roi_count, dtype=np.float32)
    detection_success = load_dataset(kp_run, "detection_success")
    if detection_success is None:
        detection_success = np.ones(roi_count, dtype=np.uint8)

    keypoints = None
    if "keypoints_roi" in kp_run:
        keypoints = kp_run["keypoints_roi"][:]
    elif "keypoints_norm" in kp_run:
        keypoints = kp_run["keypoints_norm"][:]
    else:
        raise SystemExit("Keypoints dataset (keypoints_roi/_norm) not present in run")

    labels = kp_run.attrs.get("keypoint_labels", [])
    swim_idx = find_swim_bladder_index(labels) if labels else 0

    roi_offsets = None
    crop_root = None
    if hasattr(root, "__contains__") and "crop_runs" in root:
        crop_root = root["crop_runs"]

    crop_run_name = args.crop_run or kp_run.attrs.get("source_crop_run")

    def normalize_crop_name(name: str) -> str:
        return name.split("/", 1)[1] if name.startswith("crop_runs/") else name

    if crop_root is not None:
        candidate_names: List[str] = []
        if crop_run_name:
            candidate_names.append(normalize_crop_name(crop_run_name))
        for n in _collect_group_keys(crop_root):
            if n not in candidate_names:
                candidate_names.append(n)

        for name in candidate_names:
            if name not in crop_root:
                continue
            crop_group = crop_root[name]
            roi_ds = load_dataset(crop_group, "roi_coordinates_full")
            if roi_ds is not None and roi_ds.ndim >= 2 and roi_ds.shape[0] == roi_count and roi_ds.shape[1] >= 2:
                roi_offsets = roi_ds[:, :2]
                crop_run_name = name
                break

    if roi_offsets is None:
        print("WARNING: Could not locate ROI offsets; headings will fall back to bbox centers.")
    else:
        print(f"Using crop run '{crop_run_name}' for ROI offsets")

    detect_group = None
    detect_run = args.detect_run
    if "detect_runs" in root:
        runs_group = root["detect_runs"]
        if detect_run is None:
            detect_run = find_latest_run(runs_group)
        if detect_run and detect_run in runs_group:
            detect_group = runs_group[detect_run]

    frame_offsets = None
    bbox_norm = None
    image_width = None
    image_height = None

    if detect_group is not None:
        if "frame_offsets" in detect_group:
            frame_offsets = detect_group["frame_offsets"][:]
        if "bbox_norm_coords" in detect_group:
            bbox_norm = detect_group["bbox_norm_coords"][:]
        image_width = detect_group.attrs.get("image_width")
        image_height = detect_group.attrs.get("image_height")

    det_indices = np.full(roi_count, -1, dtype=np.int64)
    if frame_offsets is not None:
        cursor = np.zeros(len(frame_offsets) - 1, dtype=np.int64)
        for roi_idx, frame in enumerate(frame_indices):
            if frame < 0 or frame >= cursor.shape[0]:
                continue
            start = frame_offsets[frame]
            end = frame_offsets[frame + 1]
            offset = cursor[frame]
            if start + offset < end:
                det_indices[roi_idx] = start + offset
            cursor[frame] += 1

    def norm_box_to_pixels(norm_box: np.ndarray) -> np.ndarray:
        if image_width is None or image_height is None:
            return norm_box
        cx = norm_box[0] * image_width
        cy = norm_box[1] * image_height
        w = norm_box[2] * image_width
        h = norm_box[3] * image_height
        x_min = cx - 0.5 * w
        y_min = cy - 0.5 * h
        x_max = cx + 0.5 * w
        y_max = cy + 0.5 * h
        return np.array([x_min, y_min, x_max, y_max], dtype=np.float32)

    matches: List[int]
    if args.frame is not None:
        matches = np.where(frame_indices == args.frame)[0].tolist()
        if not matches:
            print(f"No ROI entries found for frame {args.frame}")
            return
    else:
        matches = list(range(roi_count))

    print(f"Inspecting keypoints run '{keypoints_run}' "
          f"(swim_bladder index {swim_idx}, total ROI entries {roi_count})")
    if detect_group is not None:
        print(f"Using detection run '{detect_run}' for bounding boxes")

    shown = 0
    for roi_idx in matches:
        if shown >= args.limit:
            break
        frame = frame_indices[roi_idx]
        heading = heading_values[roi_idx]
        success = detection_success[roi_idx]
        kp = keypoints[roi_idx, swim_idx, :2]
        offset_x = offset_y = None
        if roi_offsets is not None and roi_idx < roi_offsets.shape[0]:
            offset_y = float(roi_offsets[roi_idx, 0])
            offset_x = float(roi_offsets[roi_idx, 1])

        anchor_x = float(kp[0]) if np.isfinite(kp[0]) else math.nan
        anchor_y = float(kp[1]) if np.isfinite(kp[1]) else math.nan
        if offset_x is not None and np.isfinite(anchor_x):
            anchor_x += offset_x
        if offset_y is not None and np.isfinite(anchor_y):
            anchor_y += offset_y

        det_idx = det_indices[roi_idx] if det_indices is not None else -1
        bbox_pixels = None
        bbox_norm_row = None
        if det_idx >= 0 and bbox_norm is not None and det_idx < bbox_norm.shape[0]:
            bbox_norm_row = bbox_norm[det_idx]
            bbox_pixels = norm_box_to_pixels(bbox_norm_row)

        finite_kp = np.all(np.isfinite(kp))
        finite_anchor = math.isfinite(anchor_x) and math.isfinite(anchor_y)

        print("------------------------------------------------------------")
        print(f"ROI index {roi_idx} | frame {frame} | det_success {success} "
              f"| heading {heading:.3f}")
        print(f"  swim keypoint: ({kp[0]:.4f}, {kp[1]:.4f}) finite={finite_kp}")
        if roi_offsets is not None:
            print(f"  ROI offsets (y,x): ({offset_y}, {offset_x})")
        print(f"  anchor (x,y): ({anchor_x}, {anchor_y}) finite={finite_anchor}")
        if det_idx >= 0:
            print(f"  detection index: {det_idx}")
        if bbox_norm_row is not None:
            print(f"  bbox norm (cx,cy,w,h): {bbox_norm_row}")
        if bbox_pixels is not None:
            print(f"  bbox pixel (xmin,ymin,xmax,ymax): {bbox_pixels}")

        shown += 1


if __name__ == "__main__":
    main()
