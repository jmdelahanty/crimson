#!/usr/bin/env python3
"""Fix HEVC keyframe flags in MP4 recordings.

The acquisition agent's FFmpegWriter does not set AV_PKT_FLAG_KEY for HEVC
streams, so the MP4 container has no sync sample table (stss box). This causes
unreliable seeking in crimson and other players.

This script uses MP4Box (from GPAC) to re-import each MP4, which parses the
HEVC bitstream and builds a proper stss box with correct IDR keyframe entries.
The video data is not re-encoded — only container metadata changes.

Usage:
    # Dry run (default) — show what would be fixed
    python fix_hevc_keyframes.py /nvme1/recordings

    # Actually fix the files (replaces originals, keeps .bak backup)
    python fix_hevc_keyframes.py /nvme1/recordings --apply

    # Fix without keeping backups
    python fix_hevc_keyframes.py /nvme1/recordings --apply --no-backup

    # Non-recursive (only top-level cams/ dirs)
    python fix_hevc_keyframes.py /nvme1/recordings --no-recursive
"""

import argparse
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Iterable, List


def _iter_mp4(roots: List[Path], recursive: bool) -> Iterable[Path]:
    for root in roots:
        root = root.expanduser()
        if root.is_file() and root.suffix == ".mp4":
            yield root
            continue
        if not root.exists():
            print(f"  warning: {root} does not exist, skipping")
            continue
        if recursive:
            yield from sorted(root.rglob("cams/*.mp4"))
        else:
            yield from sorted(root.glob("*/cams/*.mp4"))


def _has_stss(path: Path) -> bool:
    """Check if an MP4 file has a sync sample table (stss box)."""
    # The moov atom can be at the start or end of the file. Read enough from
    # both ends to find it.
    try:
        size = path.stat().st_size
        with open(path, "rb") as f:
            # Check start of file (faststart / progressive)
            head = f.read(min(size, 200 * 1024 * 1024))
            if b"stss" in head:
                return True
            # Check end of file (moov at end)
            if size > len(head):
                tail_start = max(0, size - 500 * 1024 * 1024)
                f.seek(tail_start)
                tail = f.read()
                if b"stss" in tail:
                    return True
        return False
    except OSError as exc:
        print(f"  warning: could not read {path}: {exc}")
        return True  # assume OK if unreadable, skip it


def _is_hevc(path: Path) -> bool:
    """Check if the video stream is HEVC using ffprobe."""
    try:
        result = subprocess.run(
            [
                "ffprobe", "-select_streams", "v",
                "-show_entries", "stream=codec_name",
                "-of", "csv=p=0", str(path),
            ],
            capture_output=True, text=True, timeout=30,
        )
        return "hevc" in result.stdout.strip().lower()
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return False


def _fix_file(path: Path, keep_backup: bool) -> bool:
    """Re-import an MP4 with MP4Box to build a proper stss box.

    Returns True on success.
    """
    tmp_path = path.with_suffix(".mp4.fixing")
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
            print(f"  ERROR: MP4Box failed for {path}")
            print(f"  stderr: {result.stderr.strip()}")
            if tmp_path.exists():
                tmp_path.unlink()
            return False

        # Verify the fix worked
        if not _has_stss(tmp_path):
            print(f"  ERROR: stss still missing after MP4Box re-import of {path}")
            tmp_path.unlink()
            return False

        # Replace original
        if keep_backup:
            backup_path = path.with_suffix(".mp4.bak")
            if backup_path.exists():
                print(f"  skipping: backup already exists at {backup_path}")
                tmp_path.unlink()
                return False
            path.rename(backup_path)
        else:
            path.unlink()

        tmp_path.rename(path)
        return True

    except subprocess.TimeoutExpired:
        print(f"  ERROR: MP4Box timed out for {path}")
        if tmp_path.exists():
            tmp_path.unlink()
        return False
    except OSError as exc:
        print(f"  ERROR: file operation failed for {path}: {exc}")
        if tmp_path.exists():
            tmp_path.unlink()
        return False


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Fix HEVC keyframe flags in MP4 recordings using MP4Box.",
    )
    parser.add_argument(
        "roots",
        nargs="*",
        type=Path,
        default=[Path("/nvme1/recordings")],
        help="Recording root directories to scan (default: /nvme1/recordings)",
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="Actually fix the files. Without this flag, only a dry run is performed.",
    )
    parser.add_argument(
        "--no-backup",
        action="store_true",
        help="Do not keep .bak backups of original files.",
    )
    parser.add_argument(
        "--no-recursive",
        action="store_true",
        help="Only scan one level deep (*/cams/*.mp4) instead of full recursion.",
    )
    args = parser.parse_args()

    # Check dependencies
    if args.apply and not shutil.which("MP4Box"):
        print("ERROR: MP4Box not found. Install with: sudo apt install gpac")
        sys.exit(1)

    recursive = not args.no_recursive
    keep_backup = not args.no_backup

    mp4s = list(_iter_mp4(args.roots, recursive))
    if not mp4s:
        print("No .mp4 files found.")
        return

    print(f"Found {len(mp4s)} MP4 file(s). Scanning for missing keyframe flags...\n")

    needs_fix: List[Path] = []
    already_ok: List[Path] = []
    skipped: List[Path] = []

    for path in mp4s:
        if not _is_hevc(path):
            skipped.append(path)
            continue
        if _has_stss(path):
            already_ok.append(path)
        else:
            needs_fix.append(path)

    print(f"  HEVC missing stss (needs fix): {len(needs_fix)}")
    print(f"  HEVC already OK:               {len(already_ok)}")
    print(f"  Skipped (not HEVC):            {len(skipped)}")
    print()

    if not needs_fix:
        print("Nothing to fix.")
        return

    if not args.apply:
        print("Files that need fixing:")
        for path in needs_fix:
            print(f"  {path}")
        print(f"\nRe-run with --apply to fix these {len(needs_fix)} file(s).")
        return

    print(f"Fixing {len(needs_fix)} file(s)...\n")
    fixed = 0
    failed = 0
    for i, path in enumerate(needs_fix, 1):
        print(f"[{i}/{len(needs_fix)}] {path}")
        if _fix_file(path, keep_backup):
            print(f"  OK")
            fixed += 1
        else:
            failed += 1

    print(f"\nDone. Fixed: {fixed}, Failed: {failed}")
    if keep_backup and fixed > 0:
        print("Original files saved with .mp4.bak extension.")


if __name__ == "__main__":
    main()
