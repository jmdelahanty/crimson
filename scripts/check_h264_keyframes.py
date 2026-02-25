#!/usr/bin/env python3
"""Check and fix H.264 keyframe flags in rendered MP4 recordings.

Rendered recordings (screen captures, stimuli) are H.264 files stored under
each recording's raw/ directory.  Like the HEVC camera recordings, these may
be missing a sync sample table (stss box) which causes unreliable seeking.

This script scans for H.264 MP4 files, reports their status (codec, stss,
GOP, resolution, fps, frame count), and optionally fixes missing stss boxes
using MP4Box.

Usage:
    # Dry run — report status of all rendered recordings
    python check_h264_keyframes.py /nvme1/recordings

    # Fix missing stss boxes
    python check_h264_keyframes.py /nvme1/recordings --apply

    # Single file
    python check_h264_keyframes.py /path/to/file.mp4

    # No backups
    python check_h264_keyframes.py /nvme1/recordings --apply --no-backup
"""

import argparse
import shutil
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
from rich.table import Table

console = Console()

_FFPROBE = "/opt/orange/lib/ffmpeg-nvidia/bin/ffprobe"


# ---------------------------------------------------------------------------
# Discovery helpers
# ---------------------------------------------------------------------------

def _iter_mp4(roots: List[Path], recursive: bool) -> Iterable[Path]:
    for root in roots:
        root = root.expanduser()
        if root.is_file() and root.suffix == ".mp4":
            if not root.name.endswith((".bak", ".fixing")):
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
            if not p.name.endswith((".bak", ".fixing")):
                yield p


# ---------------------------------------------------------------------------
# Probe helpers
# ---------------------------------------------------------------------------

def _has_stss(path: Path) -> bool:
    """Check if an MP4 file has a sync sample table (stss box)."""
    try:
        size = path.stat().st_size
        with open(path, "rb") as f:
            head = f.read(min(size, 200 * 1024 * 1024))
            if b"stss" in head:
                return True
            if size > len(head):
                tail_start = max(0, size - 500 * 1024 * 1024)
                f.seek(tail_start)
                tail = f.read()
                if b"stss" in tail:
                    return True
        return False
    except OSError as exc:
        console.print(f"  [yellow]warning:[/] could not read {path}: {exc}")
        return True  # assume OK if unreadable


def _probe_stream(path: Path) -> Optional[Dict[str, str]]:
    """Probe video stream properties."""
    try:
        result = subprocess.run(
            [
                _FFPROBE, "-v", "quiet", "-select_streams", "v",
                "-show_entries", "stream=codec_name,width,height,nb_frames,r_frame_rate",
                "-of", "csv=p=0", str(path),
            ],
            capture_output=True, text=True, timeout=30,
        )
        line = result.stdout.strip().split("\n")[0]
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 4:
            return None
        info: Dict[str, str] = {
            "codec": parts[0],
            "width": parts[1],
            "height": parts[2],
        }
        # r_frame_rate is a fraction like "120/1"
        rfr = parts[3]
        if "/" in rfr:
            num, den = rfr.split("/")
            info["fps"] = str(int(round(int(num) / int(den))))
        else:
            info["fps"] = rfr
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
# Fix
# ---------------------------------------------------------------------------

def _fix_file(path: Path, keep_backup: bool) -> bool:
    """Re-import an MP4 with MP4Box to build a proper stss box."""
    tmp_path = path.with_suffix(".mp4.fixing")
    backup_path = path.with_suffix(".mp4.bak")
    backup_exists = backup_path.exists()

    try:
        result = subprocess.run(
            [
                "MP4Box",
                "-add", f"{path}#video",
                "-new", str(tmp_path),
            ],
            capture_output=True, text=True, timeout=3600,
        )
        if result.returncode != 0:
            console.print(f"  [red]ERROR:[/] MP4Box failed for {path}")
            console.print(f"  stderr: {result.stderr.strip()}")
            if tmp_path.exists():
                tmp_path.unlink()
            return False

        if not _has_stss(tmp_path):
            console.print(f"  [red]ERROR:[/] stss still missing after MP4Box re-import of {path}")
            tmp_path.unlink()
            return False

        # Swap files
        if keep_backup and not backup_exists:
            path.rename(backup_path)
        else:
            path.unlink()

        tmp_path.rename(path)
        return True

    except subprocess.TimeoutExpired:
        console.print(f"  [red]ERROR:[/] MP4Box timed out for {path}")
        if tmp_path.exists():
            tmp_path.unlink()
        return False
    except OSError as exc:
        console.print(f"  [red]ERROR:[/] file operation failed for {path}: {exc}")
        if tmp_path.exists():
            tmp_path.unlink()
        return False


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Check and fix H.264 keyframe flags in rendered MP4 recordings.",
    )
    parser.add_argument(
        "roots",
        nargs="*",
        type=Path,
        default=[Path("/nvme1/recordings")],
        help="Recording root directories or files to scan (default: /nvme1/recordings)",
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="Fix missing stss boxes via MP4Box. Without this, only a report is shown.",
    )
    parser.add_argument(
        "--no-backup",
        action="store_true",
        help="Do not keep .mp4.bak backups of original files.",
    )
    parser.add_argument(
        "--no-recursive",
        action="store_true",
        help="Only scan one level deep (*/raw/*.mp4) instead of full recursion.",
    )
    args = parser.parse_args()

    recursive = not args.no_recursive
    keep_backup = not args.no_backup

    mp4s = list(_iter_mp4(args.roots, recursive))
    if not mp4s:
        console.print("No .mp4 files found.")
        return

    # Scan all files
    class FileInfo:
        def __init__(self, path: Path):
            self.path = path
            self.codec: str = "?"
            self.width: str = "?"
            self.height: str = "?"
            self.fps: str = "?"
            self.nb_frames: str = "?"
            self.stss: bool = False
            self.gop: Optional[int] = None

    files: List[FileInfo] = []

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
            info = FileInfo(path)
            stream = _probe_stream(path)
            if stream:
                info.codec = stream.get("codec", "?")
                info.width = stream.get("width", "?")
                info.height = stream.get("height", "?")
                info.fps = stream.get("fps", "?")
                info.nb_frames = stream.get("nb_frames", "?")
            info.stss = _has_stss(path)
            if info.stss:
                info.gop = _probe_gop(path)
            files.append(info)
            progress.advance(scan_task)

    # Build summary table
    table = Table(title="MP4 Video Report")
    table.add_column("File", style="cyan", no_wrap=True)
    table.add_column("Codec")
    table.add_column("Resolution", justify="right")
    table.add_column("FPS", justify="right")
    table.add_column("Frames", justify="right")
    table.add_column("stss")
    table.add_column("GOP", justify="right")

    needs_fix: List[FileInfo] = []

    for f in files:
        stss_str = "[green]yes[/]" if f.stss else "[red]missing[/]"
        gop_str = str(f.gop) if f.gop is not None else ("-" if f.stss else "[dim]?[/]")
        table.add_row(
            f.path.name,
            f.codec,
            f"{f.width}x{f.height}",
            f.fps,
            f.nb_frames,
            stss_str,
            gop_str,
        )
        if not f.stss:
            needs_fix.append(f)

    console.print()
    console.print(table)
    console.print()

    # Summary counts
    ok_count = sum(1 for f in files if f.stss)
    console.print(f"  OK (stss present):    {ok_count}")
    console.print(f"  Missing stss:         {len(needs_fix)}")
    console.print()

    if not needs_fix:
        console.print("All files look good.")
        return

    if not args.apply:
        console.print("Files missing stss:")
        for f in needs_fix:
            console.print(f"  {f.path}")
        console.print(f"\nRe-run with [bold]--apply[/] to fix these {len(needs_fix)} file(s) via MP4Box.")
        return

    # Check MP4Box is available
    if not shutil.which("MP4Box"):
        console.print("[red]ERROR:[/] MP4Box not found. Install with: sudo apt install gpac")
        sys.exit(1)

    console.print(f"Fixing {len(needs_fix)} file(s)...\n")
    fixed = 0
    failed = 0
    with Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        BarColumn(),
        MofNCompleteColumn(),
        TimeElapsedColumn(),
        console=console,
    ) as progress:
        fix_task = progress.add_task("Fixing", total=len(needs_fix))
        for f in needs_fix:
            progress.update(fix_task, description=f"Fixing [cyan]{f.path.name}[/]")
            if _fix_file(f.path, keep_backup):
                console.print(f"  [green]OK[/] {f.path.name}")
                fixed += 1
            else:
                console.print(f"  [red]FAILED[/] {f.path.name}")
                failed += 1
            progress.advance(fix_task)

    console.print(f"\nDone. Fixed: {fixed}, Failed: {failed}")
    if keep_backup and fixed > 0:
        console.print("Original files saved with .mp4.bak extension.")


if __name__ == "__main__":
    main()
