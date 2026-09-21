#!/usr/bin/env python3
"""Read a few raw-v2 keypoint rows independently of Crimson/TensorStore.

This read-only acceptance oracle decodes only the Zarr-v3 shard indices and
inner chunks needed by explicitly requested acquisition frames.  It compares
selected logical values (keys, acquisition frames, validity, and image-space
points) with an optional canonical-overlay probe JSON document.  It does not
verify complete array digests, every row, shard-index CRC32C values, or whole
files and must not be treated as a complete storage-integrity verifier.

The implementation intentionally supports only the concrete bytes+zstd,
end-indexed Zarr-v3 sharding layout used by the August raw-v2 publications.
Unsupported metadata fails closed instead of being guessed.
"""

from __future__ import annotations

import argparse
from collections import OrderedDict
import ctypes
import ctypes.util
import json
import math
import pathlib
import struct
from typing import Any


MAX_FRAMES = 32
MAX_OBSERVATIONS_PER_FRAME = 64
MAX_INDEX_BYTES = 1 * 1024 * 1024
MAX_COMPRESSED_INNER_BYTES = 8 * 1024 * 1024
MAX_DECODED_INNER_BYTES = 8 * 1024 * 1024
MAX_TOTAL_DECODED_BYTES = 512 * 1024 * 1024
MAX_CACHE_BYTES = 64 * 1024 * 1024
UINT64_MAX = (1 << 64) - 1

DTYPES = {
    "uint64": ("Q", 8),
    "int64": ("q", 8),
    "float32": ("f", 4),
    "bool": ("?", 1),
}

EXPECTED_ARRAYS = {
    "frame_row_offsets": ("int64", (None,)),
    "instance_key": ("uint64", (None,)),
    "source_acquisition_frame_index": ("int64", (None,)),
    "source_crop_row_ids": ("int64", (None,)),
    "pose_success": ("bool", (None,)),
    "pose_confidence": ("float32", (None,)),
    "keypoint_valid": ("bool", (None, 5)),
    "keypoint_confidences": ("float32", (None, 5)),
    "keypoints_img": ("float32", (None, 5, 2)),
}


def product(values: list[int]) -> int:
    result = 1
    for value in values:
        result *= value
    return result


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


class ZstdDecoder:
    def __init__(self) -> None:
        library_name = ctypes.util.find_library("zstd")
        require(library_name is not None, "system libzstd is unavailable")
        self._library = ctypes.CDLL(library_name)
        self._library.ZSTD_decompress.argtypes = [
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.c_void_p,
            ctypes.c_size_t,
        ]
        self._library.ZSTD_decompress.restype = ctypes.c_size_t
        self._library.ZSTD_isError.argtypes = [ctypes.c_size_t]
        self._library.ZSTD_isError.restype = ctypes.c_uint
        self._library.ZSTD_getErrorName.argtypes = [ctypes.c_size_t]
        self._library.ZSTD_getErrorName.restype = ctypes.c_char_p

    def decompress(self, encoded: bytes, expected_bytes: int) -> bytes:
        source = ctypes.create_string_buffer(encoded)
        destination = ctypes.create_string_buffer(expected_bytes)
        result = self._library.ZSTD_decompress(
            destination, expected_bytes, source, len(encoded)
        )
        if self._library.ZSTD_isError(result):
            detail = self._library.ZSTD_getErrorName(result).decode(
                "utf-8", errors="replace"
            )
            raise ValueError(f"zstd decompression failed: {detail}")
        require(result == expected_bytes, "zstd output has the wrong size")
        return destination.raw


class ChunkCache:
    def __init__(self, metrics: dict[str, int]) -> None:
        self._entries: OrderedDict[tuple[Any, ...], bytes] = OrderedDict()
        self._bytes = 0
        self._metrics = metrics

    def get(self, key: tuple[Any, ...]) -> bytes | None:
        value = self._entries.get(key)
        if value is not None:
            self._entries.move_to_end(key)
        return value

    def put(self, key: tuple[Any, ...], value: bytes) -> None:
        while self._entries and self._bytes + len(value) > MAX_CACHE_BYTES:
            _, evicted = self._entries.popitem(last=False)
            self._bytes -= len(evicted)
        require(len(value) <= MAX_CACHE_BYTES, "decoded chunk exceeds cache limit")
        self._entries[key] = value
        self._bytes += len(value)
        self._metrics["peak_cache_bytes"] = max(
            self._metrics["peak_cache_bytes"], self._bytes
        )


class BoundedShardedArray:
    def __init__(
        self,
        path: pathlib.Path,
        expected_dtype: str,
        expected_shape: tuple[int | None, ...],
        metrics: dict[str, int],
        cache: ChunkCache,
    ) -> None:
        self.path = path
        self.metadata = json.loads((path / "zarr.json").read_text())
        self.metrics = metrics
        self.cache = cache
        self._validate_metadata(expected_dtype, expected_shape)
        self.format, self.item_size = DTYPES[self.metadata["data_type"]]
        self.decoder = ZstdDecoder()

    def _validate_metadata(
        self, expected_dtype: str, expected_shape: tuple[int | None, ...]
    ) -> None:
        metadata = self.metadata
        require(metadata.get("zarr_format") == 3, f"{self.path}: not Zarr v3")
        require(metadata.get("node_type") == "array", f"{self.path}: not an array")
        require(
            metadata.get("data_type") == expected_dtype,
            f"{self.path}: unexpected dtype",
        )
        shape = metadata.get("shape")
        require(
            isinstance(shape, list)
            and len(shape) == len(expected_shape)
            and all(isinstance(value, int) and value > 0 for value in shape),
            f"{self.path}: invalid shape",
        )
        for actual, expected in zip(shape, expected_shape):
            require(expected is None or actual == expected, f"{self.path}: wrong shape")
        self.shape = shape

        grid = metadata.get("chunk_grid", {})
        require(grid.get("name") == "regular", f"{self.path}: unsupported grid")
        self.outer = grid.get("configuration", {}).get("chunk_shape")
        require(
            isinstance(self.outer, list)
            and len(self.outer) == len(shape)
            and all(isinstance(value, int) and value > 0 for value in self.outer),
            f"{self.path}: invalid outer chunk shape",
        )
        encoding = metadata.get("chunk_key_encoding", {})
        require(
            encoding.get("name") == "default"
            and encoding.get("configuration", {}).get("separator") == "/",
            f"{self.path}: unsupported chunk-key encoding",
        )
        require(
            metadata.get("storage_transformers", []) == [],
            f"{self.path}: storage transformers are unsupported",
        )

        codecs = metadata.get("codecs")
        require(
            isinstance(codecs, list)
            and len(codecs) == 1
            and codecs[0].get("name") == "sharding_indexed",
            f"{self.path}: unsupported outer codec",
        )
        configuration = codecs[0].get("configuration", {})
        require(
            configuration.get("index_location") == "end",
            f"{self.path}: only end shard indices are supported",
        )
        self.inner = configuration.get("chunk_shape")
        require(
            isinstance(self.inner, list)
            and len(self.inner) == len(shape)
            and all(isinstance(value, int) and value > 0 for value in self.inner)
            and all(
                self.outer[index] % self.inner[index] == 0
                for index in range(len(shape))
            ),
            f"{self.path}: invalid inner chunk shape",
        )
        inner_codecs = configuration.get("codecs")
        require(
            isinstance(inner_codecs, list)
            and len(inner_codecs) == 2
            and inner_codecs[0].get("name") == "bytes"
            and inner_codecs[1].get("name") == "zstd",
            f"{self.path}: only bytes+zstd inner codecs are supported",
        )
        byte_configuration = inner_codecs[0].get("configuration", {})
        if expected_dtype == "bool":
            require(
                byte_configuration == {},
                f"{self.path}: unsupported boolean bytes configuration",
            )
        else:
            require(
                byte_configuration.get("endian") == "little",
                f"{self.path}: only little-endian values are supported",
            )
        zstd_configuration = inner_codecs[1].get("configuration", {})
        require(
            isinstance(zstd_configuration.get("level"), int)
            and isinstance(zstd_configuration.get("checksum"), bool),
            f"{self.path}: malformed zstd configuration",
        )
        index_codecs = configuration.get("index_codecs")
        require(
            isinstance(index_codecs, list)
            and len(index_codecs) == 2
            and index_codecs[0].get("name") == "bytes"
            and index_codecs[0].get("configuration", {}).get("endian")
            == "little"
            and index_codecs[1].get("name") == "crc32c",
            f"{self.path}: unsupported shard-index codecs",
        )
        self.inner_grid = [
            self.outer[index] // self.inner[index]
            for index in range(len(self.outer))
        ]
        index_bytes = product(self.inner_grid) * 16 + 4
        require(index_bytes <= MAX_INDEX_BYTES, f"{self.path}: shard index too large")
        decoded_bytes = product(self.inner) * DTYPES[expected_dtype][1]
        require(
            decoded_bytes <= MAX_DECODED_INNER_BYTES,
            f"{self.path}: decoded inner chunk exceeds safety limit",
        )

    def _inner_chunk(self, row: int) -> tuple[bytes, list[int]]:
        require(0 <= row < self.shape[0], f"{self.path}: row out of range")
        shard_coordinates = [row // self.outer[0]] + [0] * (len(self.outer) - 1)
        within = [row % self.outer[0]] + [0] * (len(self.outer) - 1)
        inner_coordinates = [
            within[index] // self.inner[index]
            for index in range(len(self.inner))
        ]
        flat_chunk = 0
        for index, coordinate in enumerate(inner_coordinates):
            flat_chunk = flat_chunk * self.inner_grid[index] + coordinate
        cache_key = (str(self.path), tuple(shard_coordinates), flat_chunk)
        cached = self.cache.get(cache_key)
        if cached is not None:
            return cached, within

        shard = self.path / "c"
        for coordinate in shard_coordinates:
            shard /= str(coordinate)
        index_size = product(self.inner_grid) * 16 + 4
        shard_size = shard.stat().st_size
        require(shard_size >= index_size, f"{shard}: truncated shard index")
        with shard.open("rb") as source:
            source.seek(-index_size, 2)
            index = source.read(index_size - 4)
            require(len(index) == index_size - 4, f"{shard}: truncated index")
            offset, length = struct.unpack_from("<QQ", index, flat_chunk * 16)
            require(
                offset != UINT64_MAX and length != UINT64_MAX,
                f"{shard}: requested inner chunk is absent",
            )
            require(
                0 < length <= MAX_COMPRESSED_INNER_BYTES
                and offset <= shard_size - index_size
                and length <= shard_size - index_size - offset,
                f"{shard}: invalid inner-chunk range",
            )
            source.seek(offset)
            encoded = source.read(length)
        require(len(encoded) == length, f"{shard}: truncated inner chunk")
        expected_bytes = product(self.inner) * self.item_size
        decoded = self.decoder.decompress(encoded, expected_bytes)
        require(len(decoded) == expected_bytes, f"{shard}: wrong decoded size")
        require(
            self.metrics["decoded_inner_bytes"] + len(decoded)
            <= MAX_TOTAL_DECODED_BYTES,
            "oracle exceeded total decode safety limit",
        )
        self.metrics["compressed_inner_bytes"] += length
        self.metrics["decoded_inner_bytes"] += len(decoded)
        self.metrics["inner_chunk_reads"] += 1
        self.metrics["unverified_index_crc32c_bytes"] += 4
        self.cache.put(cache_key, decoded)
        return decoded, within

    def row(self, row: int) -> list[Any]:
        decoded, within = self._inner_chunk(row)
        inner_row = within[0] % self.inner[0]
        trailing = product(self.inner[1:])
        offset = inner_row * trailing * self.item_size
        return list(
            struct.unpack_from("<" + self.format * trailing, decoded, offset)
        )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=pathlib.Path)
    parser.add_argument("run", help="exact raw-v2 keypoints run name")
    parser.add_argument("frames", nargs="+", type=int)
    parser.add_argument(
        "--compare",
        type=pathlib.Path,
        help="canonical_overlay_repository_probe JSON to compare",
    )
    arguments = parser.parse_args()
    require(
        1 <= len(arguments.frames) <= MAX_FRAMES,
        f"request one to {MAX_FRAMES} frames",
    )
    require(
        len(arguments.frames) == len(set(arguments.frames)),
        "requested frames must be unique",
    )
    require(
        arguments.run not in ("", ".", "..")
        and "/" not in arguments.run
        and "\\" not in arguments.run,
        "run must be one safe path component",
    )
    return arguments


def compare_probe(
    samples: list[dict[str, Any]], probe_path: pathlib.Path
) -> dict[str, Any]:
    integrated = json.loads(probe_path.read_text())
    require(integrated.get("success") is True, "integrated probe did not succeed")
    integrated_samples = {
        entry["frame"]: entry for entry in integrated.get("samples", [])
    }
    for sample in samples:
        require(sample["frame"] in integrated_samples, "probe frame is missing")
        actual = integrated_samples[sample["frame"]]
        require(actual["keypoints"]["state"] == "ready", "keypoints not ready")
        require(
            len(actual["observations"]) == len(sample["observations"]),
            "observation count differs",
        )
        for expected, presented in zip(
            sample["observations"], actual["observations"]
        ):
            require(
                presented["frame"] == expected["acquisition_frame"],
                "acquisition frame differs",
            )
            require(presented["key"] == expected["instance_key"], "key differs")
            require(
                presented["keypoint_valid"]
                == [int(value) for value in expected["keypoint_valid"]],
                "keypoint validity differs",
            )
            require(
                presented["points"] == expected["keypoints_img"],
                "keypoint image coordinates differ",
            )
    return {
        "path": str(probe_path),
        "exact_selected_logical_fields_match": True,
    }


def main() -> None:
    arguments = parse_arguments()
    base = arguments.archive / "keypoints_runs" / arguments.run
    require(base.is_dir(), "raw-v2 run directory does not exist")
    metrics = {
        "compressed_inner_bytes": 0,
        "decoded_inner_bytes": 0,
        "inner_chunk_reads": 0,
        "peak_cache_bytes": 0,
        "unverified_index_crc32c_bytes": 0,
    }
    cache = ChunkCache(metrics)
    arrays = {
        name: BoundedShardedArray(
            base / name, expected_dtype, expected_shape, metrics, cache
        )
        for name, (expected_dtype, expected_shape) in EXPECTED_ARRAYS.items()
    }
    frame_count = arrays["frame_row_offsets"].shape[0] - 1
    require(frame_count > 0, "frame domain is empty")
    row_count = arrays["instance_key"].shape[0]
    for name, array in arrays.items():
        if name != "frame_row_offsets":
            require(array.shape[0] == row_count, f"{name}: row domain differs")

    samples = []
    for frame in arguments.frames:
        require(0 <= frame < frame_count, f"frame {frame} is outside the domain")
        first = arrays["frame_row_offsets"].row(frame)[0]
        last = arrays["frame_row_offsets"].row(frame + 1)[0]
        require(
            0 <= first <= last <= row_count,
            f"frame {frame} has invalid row offsets",
        )
        require(
            last - first <= MAX_OBSERVATIONS_PER_FRAME,
            f"frame {frame} exceeds {MAX_OBSERVATIONS_PER_FRAME} observations",
        )
        observations = []
        keys = set()
        for row in range(first, last):
            points = arrays["keypoints_img"].row(row)
            instance_key = arrays["instance_key"].row(row)[0]
            acquisition_frame = arrays["source_acquisition_frame_index"].row(row)[0]
            require(
                acquisition_frame == frame,
                f"row {row} carries acquisition frame {acquisition_frame}",
            )
            require(instance_key not in keys, f"frame {frame} repeats an instance key")
            keys.add(instance_key)
            observations.append(
                {
                    "row": row,
                    "instance_key": instance_key,
                    "acquisition_frame": acquisition_frame,
                    "source_crop_row_id": arrays["source_crop_row_ids"].row(row)[0],
                    "pose_success": arrays["pose_success"].row(row)[0],
                    "pose_confidence": arrays["pose_confidence"].row(row)[0],
                    "keypoint_valid": arrays["keypoint_valid"].row(row),
                    "keypoint_confidences": arrays["keypoint_confidences"].row(row),
                    "keypoints_img": [
                        points[index : index + 2] for index in range(0, 10, 2)
                    ],
                }
            )
        samples.append(
            {
                "frame": frame,
                "row_range": [first, last],
                "observations": observations,
            }
        )

    comparison = (
        compare_probe(samples, arguments.compare) if arguments.compare else None
    )
    print(
        json.dumps(
            {
                "archive": str(arguments.archive),
                "run": arguments.run,
                "method": "direct_zarr_v3_shard_index_and_exact_inner_chunks",
                "scope": {
                    "selected_logical_values_only": True,
                    "complete_payload_digest_verified": False,
                    "complete_file_integrity_verified": False,
                    "shard_index_crc32c_verified": False,
                    "max_frames": MAX_FRAMES,
                    "max_observations_per_frame": MAX_OBSERVATIONS_PER_FRAME,
                },
                "frame_count": frame_count,
                "row_count": row_count,
                "metrics": metrics,
                "samples": samples,
                "integrated_comparison": comparison,
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        raise SystemExit(f"august_keypoint_source_oracle: {error}") from error
