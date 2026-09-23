#!/usr/bin/env python3
"""Export Crimson's selected camera→stimulus mapping to portable CSV/JSON.

Supports the Zarr v3 numeric layouts used by current Palette stimulus runs
without requiring the Python zarr package. Compressed chunks are decoded with
the `zstd` executable.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import shutil
import subprocess
from pathlib import Path

import numpy as np


DTYPES = {
    "bool": np.dtype("?"),
    "int32": np.dtype("<i4"),
    "int64": np.dtype("<i8"),
    "uint8": np.dtype("u1"),
    "uint64": np.dtype("<u8"),
}


def read_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def read_array(path: Path) -> np.ndarray:
    metadata = read_json(path / "zarr.json")
    shape = metadata["shape"]
    if len(shape) != 1:
        raise ValueError(f"only 1-D arrays are supported: {path} shape={shape}")
    dtype_name = metadata["data_type"]
    if dtype_name not in DTYPES:
        raise ValueError(f"unsupported dtype {dtype_name}: {path}")
    dtype = DTYPES[dtype_name]
    chunk_size = metadata["chunk_grid"]["configuration"]["chunk_shape"][0]
    codecs = [codec["name"] for codec in metadata.get("codecs", [])]
    if codecs not in (["bytes"], ["bytes", "zstd"]):
        raise ValueError(f"unsupported codecs {codecs}: {path}")
    if "zstd" in codecs and shutil.which("zstd") is None:
        raise RuntimeError("zstd executable is required to read this fixture")

    result = np.full(shape[0], metadata.get("fill_value", 0), dtype=dtype)
    chunks = math.ceil(shape[0] / chunk_size)
    for index in range(chunks):
        chunk_path = path / "c" / str(index)
        if not chunk_path.exists():
            continue
        if "zstd" in codecs:
            payload = subprocess.run(
                ["zstd", "-q", "-d", "-c", str(chunk_path)],
                check=True,
                capture_output=True,
            ).stdout
        else:
            payload = chunk_path.read_bytes()
        values = np.frombuffer(payload, dtype=dtype)
        start = index * chunk_size
        count = min(values.size, shape[0] - start)
        result[start : start + count] = values[:count]
    return result


def choose_run(root: Path, requested: str | None) -> tuple[str, Path]:
    runs = root / "analysis" / "stimulus_runs"
    if requested:
        selected = requested
    else:
        attrs = read_json(runs / "zarr.json").get("attributes", {})
        selected = next(
            (attrs[key] for key in ("latest_complete", "latest_completed", "latest")
             if isinstance(attrs.get(key), str) and attrs[key]),
            None,
        )
        if not selected:
            candidates = sorted(
                child.name for child in runs.iterdir()
                if child.is_dir() and (child / "zarr.json").exists()
            )
            if not candidates:
                raise ValueError(f"no stimulus runs under {runs}")
            selected = candidates[-1]
    run = runs / selected
    if not run.is_dir():
        raise ValueError(f"stimulus run does not exist: {run}")
    return selected, run


def optional_array(path: Path) -> np.ndarray | None:
    return read_array(path) if (path / "zarr.json").exists() else None


def export(root: Path, output: Path, requested_run: str | None) -> None:
    run_name, run = choose_run(root, requested_run)
    alignment = run / "frame_alignment"
    metadata = run / "video_metadata" / "frame_metadata"

    direct = optional_array(alignment / "camera_to_stimulus_frame_corrected")
    direct_interpolated = optional_array(
        alignment / "camera_stimulus_frame_interpolated"
    )
    if direct is not None:
        stimulus = direct.astype(np.int64, copy=False)
        interpolated = (
            direct_interpolated.astype(bool, copy=False)
            if direct_interpolated is not None
            else np.zeros(stimulus.size, dtype=bool)
        )
        variant = "direct_corrected"
    else:
        corrected_map = optional_array(
            alignment / "camera_to_metadata_index_corrected"
        )
        legacy_map = optional_array(alignment / "camera_to_metadata_index")
        mapping = corrected_map if corrected_map is not None else legacy_map
        if mapping is None:
            raise ValueError("stimulus run has no camera-to-metadata mapping")
        corrected_frames = optional_array(metadata / "stimulus_frame_num_corrected")
        legacy_frames = optional_array(metadata / "stimulus_frame_num")
        frames = corrected_frames if corrected_frames is not None else legacy_frames
        if frames is None:
            raise ValueError("stimulus run has no metadata stimulus-frame column")
        mapping = mapping.astype(np.int64, copy=False)
        frames = frames.astype(np.int64, copy=False)
        stimulus = np.full(mapping.size, -1, dtype=np.int64)
        valid = (mapping >= 0) & (mapping < frames.size)
        stimulus[valid] = frames[mapping[valid]]
        camera_mask = optional_array(alignment / "camera_interpolation_mask")
        interpolated = (
            np.logical_not(camera_mask.astype(bool, copy=False))
            if camera_mask is not None
            else np.zeros(mapping.size, dtype=bool)
        )
        variant = (
            "legacy_corrected_metadata"
            if corrected_map is not None and corrected_frames is not None
            else "legacy_metadata"
        )

    if interpolated.size < stimulus.size:
        interpolated = np.pad(interpolated, (0, stimulus.size - interpolated.size))
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["camera_frame", "stimulus_frame", "interpolated", "valid"])
        for camera_frame, stimulus_frame in enumerate(stimulus):
            valid = int(stimulus_frame >= 0)
            writer.writerow(
                [camera_frame, int(stimulus_frame), int(interpolated[camera_frame]), valid]
            )

    attrs = read_json(run / "zarr.json").get("attributes", {})
    valid_values = stimulus[stimulus >= 0]
    manifest = {
        "schema": "crimson_macos_alignment_fixture_v1",
        "source_zarr": str(root),
        "stimulus_run": run_name,
        "mapping_variant": variant,
        "camera_frames": int(stimulus.size),
        "valid_mappings": int(valid_values.size),
        "first_stimulus_frame": int(valid_values[0]) if valid_values.size else None,
        "last_stimulus_frame": int(valid_values[-1]) if valid_values.size else None,
        "source_stimulus_video_path": attrs.get("source_stimulus_video_path"),
        "source_h5": attrs.get("source_h5"),
        "csv": output.name,
    }
    manifest_path = output.with_suffix(".json")
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2))
    print(f"Wrote {output}")
    print(f"Wrote {manifest_path}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("zarr", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--run")
    args = parser.parse_args()
    export(args.zarr.resolve(), args.output.resolve(), args.run)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
