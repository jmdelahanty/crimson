#!/usr/bin/env python3
"""Batch re-encode H.264 rendered MP4 recordings with a correct GOP size.

Rendered recordings (screen captures, stimuli) stored under each recording's
raw/ directory may be missing both a sync sample table (stss box) and proper
GOP structure.  Re-encoding fixes both: ffmpeg's muxer writes a correct stss,
and the GOP flags (-g, -keyint_min, -forced-idr) ensure IDR frames at regular
intervals for fast seeking.

Usage:
    # Dry run — show what would be re-encoded
    python reencode_h264_gop.py /nvme1/recordings --gop 60

    # Actually re-encode
    python reencode_h264_gop.py /nvme1/recordings --gop 60 --apply

    # Single file, no backup
    python reencode_h264_gop.py /path/to/file.mp4 --gop 60 --apply --no-backup
"""

import argparse
import subprocess
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

from rich.console import Console
from rich.progress import (
    BarColumn,
    MofNCompleteColumn,
    Progress,
    SpinnerColumn,
    TextColumn,
    TimeElapsedColumn,
)

console = Console()

_FFMPEG = "/opt/orange/lib/ffmpeg-nvidia/bin/ffmpeg"
_FFPROBE = "/opt/orange/lib/ffmpeg-nvidia/bin/ffprobe"


# ---------------------------------------------------------------------------
# Discovery
# ---------------------------------------------------------------------------

def _iter_mp4(roots: List[Path], recursive: bool) -> Iterable[Path]:
    for root in roots:
        root = root.expanduser()
        if root.is_file() and root.suffix == ".mp4":
            if not root.name.endswith((".bak", ".recoding")):
                yield root
            continue
        if not root.exists():
            console.print(f"  [yellow]warning:[/] {root} does not exist, skipping")
            continue
        if recursive:
            candidates = sorted(root.rglob("raw/*.mp4"))
        else:
            candidates = sorted(root.glob("*/raw/*.mp4"))
        for p in candidates:
            if not p.name.endswith((".bak", ".recoding", ".fixing")):
                yield p


# ---------------------------------------------------------------------------
# Probe helpers
# ---------------------------------------------------------------------------

def _is_h264(path: Path) -> bool:
    try:
        result = subprocess.run(
            [
                _FFPROBE, "-v", "quiet", "-select_streams", "v",
                "-show_entries", "stream=codec_name",
                "-of", "csv=p=0", str(path),
            ],
            capture_output=True, text=True, timeout=30,
        )
        return "h264" in result.stdout.strip().lower()
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return False


def _has_stss(path: Path) -> bool:
    try:
        size = path.stat().st_size
        with open(path, "rb") as f:
            head = f.read(min(size, 200 * 1024 * 1024))
            if b"stss" in head:
                return True
            if size > len(head):
                f.seek(max(0, size - 500 * 1024 * 1024))
                if b"stss" in f.read():
                    return True
        return False
    except OSError:
        return True


def _probe_stream_info(path: Path) -> Optional[Dict[str, str]]:
    """Probe resolution, fps, frame count, and bitrate."""
    try:
        result = subprocess.run(
            [
                _FFPROBE, "-v", "quiet", "-select_streams", "v",
                "-show_entries", "stream=width,height,r_frame_rate,nb_frames,bit_rate",
                "-of", "csv=p=0", str(path),
            ],
            capture_output=True, text=True, timeout=30,
        )
        parts = [p.strip() for p in result.stdout.strip().split("\n")[0].split(",")]
        if len(parts) < 3:
            return None
        info: Dict[str, str] = {
            "width": parts[0],
            "height": parts[1],
        }
        rfr = parts[2]
        if "/" in rfr:
            num, den = rfr.split("/")
            info["fps"] = str(int(round(int(num) / int(den))))
        else:
            info["fps"] = rfr
        if len(parts) >= 4 and parts[3].isdigit():
            info["bit_rate"] = parts[3]
        if len(parts) >= 5 and parts[4].isdigit():
            info["nb_frames"] = parts[4]
        return info
    except (subprocess.TimeoutExpired, FileNotFoundError, ValueError):
        return None


def _probe_gop(path: Path) -> Optional[int]:
    """Detect GOP size from packet flags in the first ~5s."""
    try:
        result = subprocess.run(
            [
                _FFPROBE, "-v", "quiet", "-select_streams", "v",
                "-read_intervals", "%+5",
                "-show_packets", "-show_entries", "packet=flags",
                "-of", "csv=p=0", str(path),
            ],
            capture_output=True, text=True, timeout=30,
        )
        lines = [l.strip() for l in result.stdout.strip().split("\n") if l.strip()]
        if not lines:
            return None
        kf_indices = [i for i, l in enumerate(lines) if "K" in l]
        if len(kf_indices) < 2:
            return len(lines)
        return kf_indices[1] - kf_indices[0]
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return None


# ---------------------------------------------------------------------------
# Re-encode
# ---------------------------------------------------------------------------

def _build_ffmpeg_cmd(
    src: Path, dst: Path, gop: int, stream: Dict[str, str], gpu: int,
) -> List[str]:
    cmd = [
        _FFMPEG, "-y",
        "-hwaccel", "cuda", "-hwaccel_device", str(gpu),
        "-i", str(src),
        "-c:v", "h264_nvenc", "-gpu", str(gpu),
        "-preset", "p1", "-tune", "ll", "-rc", "vbr",
    ]

    # Match original bitrate if available
    if "bit_rate" in stream:
        cmd += ["-b:v", stream["bit_rate"]]

    # GOP
    cmd += ["-g", str(gop), "-keyint_min", str(gop), "-forced-idr", "1"]

    cmd += ["-movflags", "+faststart"]
    cmd += ["-an"]
    cmd += ["-f", "mp4"]
    cmd.append(str(dst))
    return cmd


def _probe_frame_count(path: Path) -> Optional[int]:
    try:
        result = subprocess.run(
            [
                _FFPROBE, "-v", "quiet", "-select_streams", "v",
                "-show_entries", "stream=nb_frames",
                "-of", "csv=p=0", str(path),
            ],
            capture_output=True, text=True, timeout=30,
        )
        val = result.stdout.strip().split("\n")[0].strip()
        return int(val) if val.isdigit() else None
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return None


def _probe_resolution(path: Path) -> Optional[Tuple[int, int]]:
    try:
        result = subprocess.run(
            [
                _FFPROBE, "-v", "quiet", "-select_streams", "v",
                "-show_entries", "stream=width,height",
                "-of", "csv=p=0", str(path),
            ],
            capture_output=True, text=True, timeout=30,
        )
        parts = result.stdout.strip().split(",")
        if len(parts) >= 2:
            return (int(parts[0].strip()), int(parts[1].strip()))
        return None
    except (subprocess.TimeoutExpired, FileNotFoundError, ValueError):
        return None


def _verify_output(original: Path, encoded: Path) -> Tuple[bool, str]:
    if not _has_stss(encoded):
        return False, "stss box missing in re-encoded file"

    orig_frames = _probe_frame_count(original)
    enc_frames = _probe_frame_count(encoded)
    if orig_frames is not None and enc_frames is not None:
        if orig_frames != enc_frames:
            return False, (
                f"frame count mismatch: original={orig_frames}, "
                f"encoded={enc_frames}"
            )

    orig_res = _probe_resolution(original)
    enc_res = _probe_resolution(encoded)
    if orig_res is not None and enc_res is not None:
        if orig_res != enc_res:
            return False, (
                f"resolution mismatch: original={orig_res[0]}x{orig_res[1]}, "
                f"encoded={enc_res[0]}x{enc_res[1]}"
            )

    return True, "OK"


def _reencode_file(path: Path, gop: int, gpu: int, keep_backup: bool) -> bool:
    tmp_path = path.with_suffix(".mp4.recoding")
    backup_path = path.with_suffix(".mp4.bak")
    backup_exists = backup_path.exists()

    try:
        stream = _probe_stream_info(path)
        if not stream or "width" not in stream:
            console.print(f"  [red]ERROR:[/] could not probe {path}")
            return False

        cmd = _build_ffmpeg_cmd(path, tmp_path, gop, stream, gpu)
        console.print(f"  cmd: {' '.join(cmd[:6])}...{' '.join(cmd[-4:])}")

        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=3600,
        )
        if result.returncode != 0:
            console.print(f"  [red]ERROR:[/] ffmpeg failed (exit {result.returncode})")
            for line in result.stderr.strip().split("\n")[-5:]:
                console.print(f"  stderr: {line}")
            if tmp_path.exists():
                tmp_path.unlink()
            return False

        ok, msg = _verify_output(path, tmp_path)
        if not ok:
            console.print(f"  [red]ERROR:[/] verification failed: {msg}")
            tmp_path.unlink()
            return False

        orig_size = path.stat().st_size
        new_size = tmp_path.stat().st_size
        ratio = new_size / orig_size if orig_size > 0 else 0
        console.print(f"  size: {orig_size / 1e6:.1f} MB -> {new_size / 1e6:.1f} MB "
                      f"({ratio:.2%})")

        if keep_backup and not backup_exists:
            path.rename(backup_path)
        else:
            path.unlink()

        tmp_path.rename(path)
        return True

    except subprocess.TimeoutExpired:
        console.print(f"  [red]ERROR:[/] ffmpeg timed out for {path}")
        if tmp_path.exists():
            tmp_path.unlink()
        return False
    except OSError as exc:
        console.print(f"  [red]ERROR:[/] file operation failed: {exc}")
        if tmp_path.exists():
            tmp_path.unlink()
        return False


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Batch re-encode H.264 rendered MP4 recordings with a target GOP size.",
    )
    parser.add_argument(
        "roots", nargs="*", type=Path, default=[Path("/nvme1/recordings")],
        help="Recording root directories or files to scan (default: /nvme1/recordings)",
    )
    parser.add_argument(
        "--gop", type=int, required=True,
        help="Target GOP size in frames (e.g. 60).",
    )
    parser.add_argument(
        "--apply", action="store_true",
        help="Actually re-encode. Without this flag, only a dry run is performed.",
    )
    parser.add_argument(
        "--no-backup", action="store_true",
        help="Do not keep .mp4.bak backups of original files.",
    )
    parser.add_argument(
        "--no-recursive", action="store_true",
        help="Only scan one level deep (*/raw/*.mp4) instead of full recursion.",
    )
    parser.add_argument(
        "--gpu", type=int, default=0,
        help="CUDA device index for NVENC (default: 0).",
    )
    args = parser.parse_args()

    if args.gop < 1:
        console.print("[red]ERROR:[/] --gop must be >= 1")
        sys.exit(1)

    recursive = not args.no_recursive
    keep_backup = not args.no_backup

    mp4s = list(_iter_mp4(args.roots, recursive))
    if not mp4s:
        console.print("No .mp4 files found.")
        return

    # Categorize files.  For H.264 files without stss, we can't trust packet
    # flags for GOP detection (MP4 spec: missing stss = all samples sync).
    # Since these files need re-encoding anyway (broken producer), we include
    # them in the needs_reencode list with GOP reported as "?".
    needs_reencode: List[Tuple[Path, str]] = []  # (path, current_gop_label)
    already_ok: List[Path] = []
    skipped: List[Tuple[Path, str]] = []

    with Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        BarColumn(),
        MofNCompleteColumn(),
        TimeElapsedColumn(),
        console=console,
    ) as progress:
        scan_task = progress.add_task("Scanning", total=len(mp4s))
        for path in mp4s:
            progress.update(scan_task, description=f"Scanning [cyan]{path.name}[/]")
            if not _is_h264(path):
                skipped.append((path, "not H.264"))
            elif not _has_stss(path):
                # No stss — can't detect GOP reliably, but the file is broken
                # anyway (missing keyframe flags).  Include it for re-encoding.
                needs_reencode.append((path, "no stss"))
            else:
                gop = _probe_gop(path)
                if gop is None:
                    skipped.append((path, "could not detect GOP"))
                elif gop <= args.gop:
                    already_ok.append(path)
                else:
                    needs_reencode.append((path, str(gop)))
            progress.advance(scan_task)

    console.print()
    console.print(f"  Needs re-encode: {len(needs_reencode)}")
    console.print(f"  Already OK:      {len(already_ok)}")
    console.print(f"  Skipped:         {len(skipped)}")
    console.print()

    if not needs_reencode:
        console.print("Nothing to re-encode.")
        return

    if not args.apply:
        console.print("Files that need re-encoding:")
        for path, gop_label in needs_reencode:
            console.print(f"  GOP {gop_label:>7} -> {args.gop:<4}  {path}")
        console.print(f"\nRe-run with [bold]--apply[/] to re-encode these "
                      f"{len(needs_reencode)} file(s).")
        return

    try:
        subprocess.run([_FFMPEG, "-version"], capture_output=True, timeout=10)
    except FileNotFoundError:
        console.print(f"[red]ERROR:[/] {_FFMPEG} not found.")
        sys.exit(1)

    console.print(f"Re-encoding {len(needs_reencode)} file(s) with GOP={args.gop} "
                  f"on GPU {args.gpu}...\n")
    success = 0
    failed = 0
    with Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        BarColumn(),
        MofNCompleteColumn(),
        TimeElapsedColumn(),
        console=console,
    ) as progress:
        encode_task = progress.add_task("Re-encoding", total=len(needs_reencode))
        for path, gop_label in needs_reencode:
            progress.update(
                encode_task,
                description=f"Re-encoding [cyan]{path.name}[/] (GOP {gop_label} -> {args.gop})",
            )
            if _reencode_file(path, args.gop, args.gpu, keep_backup):
                console.print(f"  [green]OK[/] {path.name}")
                success += 1
            else:
                console.print(f"  [red]FAILED[/] {path.name}")
                failed += 1
            progress.advance(encode_task)

    console.print(f"\nDone. Re-encoded: {success}, Failed: {failed}")
    if keep_backup and success > 0:
        console.print("Original files saved with .mp4.bak extension.")


if __name__ == "__main__":
    main()
