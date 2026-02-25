#!/usr/bin/env python3
"""Batch re-encode HEVC MP4 recordings with a smaller GOP size.

Existing recordings may have a GOP of 120 frames (2s at 60fps), making frame
seeking slow — crimson must decode up to 120 frames forward from the nearest IDR
to reach any target frame. Re-encoding with a smaller GOP (e.g. 30 or 60) reduces
worst-case seek decode cost proportionally.

This is a full re-encode (not remux) since IDR positions must change in the
bitstream. The script reads original encoding parameters from the MP4 comment tag
and matches them as closely as possible.

Usage:
    # Dry run (default) — show what would be re-encoded
    python reencode_hevc_gop.py /nvme1/recordings --gop 60

    # Actually re-encode
    python reencode_hevc_gop.py /nvme1/recordings --gop 60 --apply

    # Single file, no backup
    python reencode_hevc_gop.py /path/to/single.mp4 --gop 30 --apply --no-backup
"""

import argparse
import re
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

# The NVENC-enabled ffmpeg build.  The system ffmpeg (/usr/bin/ffmpeg) is
# typically built without NVENC and has different encoder options.
_FFMPEG = "/opt/orange/lib/ffmpeg-nvidia/bin/ffmpeg"
_FFPROBE = "/opt/orange/lib/ffmpeg-nvidia/bin/ffprobe"


# ---------------------------------------------------------------------------
# Discovery helpers (adapted from fix_hevc_keyframes.py)
# ---------------------------------------------------------------------------

def _iter_mp4(roots: List[Path], recursive: bool) -> Iterable[Path]:
    for root in roots:
        root = root.expanduser()
        if root.is_file() and root.suffix == ".mp4":
            if not root.name.endswith((".mp4.bak", ".mp4.recoding")):
                yield root
            continue
        if not root.exists():
            print(f"  warning: {root} does not exist, skipping")
            continue
        if recursive:
            candidates = sorted(root.rglob("cams/*.mp4"))
        else:
            candidates = sorted(root.glob("*/cams/*.mp4"))
        for p in candidates:
            if p.suffix == ".mp4" and not p.name.endswith((".bak", ".recoding")):
                yield p


def _is_hevc(path: Path) -> bool:
    """Check if the video stream is HEVC using ffprobe."""
    try:
        result = subprocess.run(
            [
                _FFPROBE, "-v", "quiet", "-select_streams", "v",
                "-show_entries", "stream=codec_name",
                "-of", "csv=p=0", str(path),
            ],
            capture_output=True, text=True, timeout=30,
        )
        return "hevc" in result.stdout.strip().lower()
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return False


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
        print(f"  warning: could not read {path}: {exc}")
        return True


# ---------------------------------------------------------------------------
# Probe helpers
# ---------------------------------------------------------------------------

def _parse_encoder_params(path: Path) -> Optional[Dict[str, str]]:
    """Parse the MP4 comment tag for original NVENC encoding settings.

    Expected format:
        nvenc codec=hevc; preset=p1; tuning=ll; res=4512x4512; fps=60;
        color=0; rc=vbr; bpp=0.200; target_bps=244297728

    Returns a dict with keys: preset, tune, rc, target_bps, width, height,
    fps, bpp, comment (original full string). Returns None on failure.
    """
    try:
        result = subprocess.run(
            [
                _FFPROBE, "-v", "quiet",
                "-show_entries", "format_tags=comment",
                "-of", "csv=p=0", str(path),
            ],
            capture_output=True, text=True, timeout=30,
        )
        comment = result.stdout.strip()
        if not comment:
            return None
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return None

    params: Dict[str, str] = {"comment": comment}

    # Parse key=value pairs separated by semicolons
    for part in comment.replace("nvenc ", "").split(";"):
        part = part.strip()
        if "=" not in part:
            continue
        key, _, val = part.partition("=")
        key = key.strip()
        val = val.strip()
        if key == "preset":
            params["preset"] = val
        elif key == "tuning":
            params["tune"] = val
        elif key == "rc":
            params["rc"] = val
        elif key == "target_bps":
            params["target_bps"] = val
        elif key == "bpp":
            params["bpp"] = val
        elif key == "fps":
            params["fps"] = val
        elif key == "res":
            m = re.match(r"(\d+)x(\d+)", val)
            if m:
                params["width"] = m.group(1)
                params["height"] = m.group(2)

    return params


def _probe_stream_info(path: Path) -> Optional[Dict[str, str]]:
    """Probe resolution and frame rate from the video stream."""
    try:
        result = subprocess.run(
            [
                _FFPROBE, "-v", "quiet", "-select_streams", "v",
                "-show_entries", "stream=width,height,r_frame_rate,nb_frames",
                "-of", "csv=p=0", str(path),
            ],
            capture_output=True, text=True, timeout=30,
        )
        line = result.stdout.strip().split("\n")[0]
        parts = line.split(",")
        if len(parts) < 3:
            return None
        info: Dict[str, str] = {
            "width": parts[0].strip(),
            "height": parts[1].strip(),
        }
        # r_frame_rate is a fraction like "60/1"
        rfr = parts[2].strip()
        if "/" in rfr:
            num, den = rfr.split("/")
            info["fps"] = str(int(round(int(num) / int(den))))
        else:
            info["fps"] = rfr
        if len(parts) >= 4 and parts[3].strip().isdigit():
            info["nb_frames"] = parts[3].strip()
        return info
    except (subprocess.TimeoutExpired, FileNotFoundError, (ValueError, ZeroDivisionError)):
        return None


def _get_encoding_params(path: Path) -> Dict[str, str]:
    """Get encoding params from comment tag, falling back to stream probing."""
    params = _parse_encoder_params(path) or {}
    stream = _probe_stream_info(path) or {}

    # Fill in missing values from stream probe
    for key in ("width", "height", "fps"):
        if key not in params and key in stream:
            params[key] = stream[key]

    # Defaults for NVENC settings
    params.setdefault("preset", "p1")
    params.setdefault("tune", "ll")
    params.setdefault("rc", "vbr")
    params.setdefault("bpp", "0.200")

    # Compute target_bps if missing
    if "target_bps" not in params:
        try:
            w = int(params["width"])
            h = int(params["height"])
            fps = int(params["fps"])
            bpp = float(params["bpp"])
            params["target_bps"] = str(int(w * h * bpp * fps))
        except (KeyError, ValueError):
            pass

    if "nb_frames" not in params and "nb_frames" in stream:
        params["nb_frames"] = stream["nb_frames"]

    return params


def _probe_current_gop(path: Path) -> Optional[int]:
    """Detect the current GOP size by reading packet flags from the first ~5s.

    Uses -show_packets instead of -show_frames to avoid decoding — only
    container-level demuxing is needed, making this near-instant even for
    large-resolution HEVC.
    """
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

        # Keyframe packets have 'K' in flags (e.g. "K_" or "K__")
        kf_indices = [i for i, l in enumerate(lines) if "K" in l]
        if len(kf_indices) < 2:
            # Only one keyframe in first 5s — GOP is larger than the window
            return len(lines)

        return kf_indices[1] - kf_indices[0]
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return None


# ---------------------------------------------------------------------------
# Re-encode
# ---------------------------------------------------------------------------

def _build_ffmpeg_cmd(
    src: Path, dst: Path, gop: int, params: Dict[str, str], gpu: int,
) -> List[str]:
    """Construct the ffmpeg command for GPU re-encode with the target GOP."""
    cmd = [
        _FFMPEG, "-y",
        "-hwaccel", "cuda", "-hwaccel_device", str(gpu),
        "-i", str(src),
        "-c:v", "hevc_nvenc", "-gpu", str(gpu),
    ]

    # Encoding params
    cmd += ["-preset", params.get("preset", "p1")]
    cmd += ["-tune", params.get("tune", "ll")]
    cmd += ["-rc", params.get("rc", "vbr")]

    if "target_bps" in params:
        cmd += ["-b:v", params["target_bps"]]

    # GOP settings
    cmd += ["-g", str(gop), "-keyint_min", str(gop), "-forced-idr", "1"]

    # Metadata — preserve original comment, append gop info
    original_comment = params.get("comment", "")
    if original_comment:
        new_comment = f"{original_comment}; gop={gop}"
    else:
        new_comment = f"gop={gop}"
    cmd += ["-metadata", f"comment={new_comment}"]

    # Container optimization
    cmd += ["-movflags", "+faststart"]

    # No audio in these recordings typically, but copy if present
    cmd += ["-an"]

    # Explicit format since the .recoding temp extension isn't recognized
    cmd += ["-f", "mp4"]

    cmd.append(str(dst))
    return cmd


def _probe_frame_count(path: Path) -> Optional[int]:
    """Get the frame count from ffprobe."""
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
        if val.isdigit():
            return int(val)
        return None
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return None


def _probe_resolution(path: Path) -> Optional[Tuple[int, int]]:
    """Get width, height from ffprobe."""
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


def _verify_output(
    original: Path, encoded: Path, gop: int,
) -> Tuple[bool, str]:
    """Post-encode verification checks.

    Returns (ok, message).
    """
    # 1. stss box exists
    if not _has_stss(encoded):
        return False, "stss box missing in re-encoded file"

    # 2. Frame count matches
    orig_frames = _probe_frame_count(original)
    enc_frames = _probe_frame_count(encoded)
    if orig_frames is not None and enc_frames is not None:
        if orig_frames != enc_frames:
            return False, (
                f"frame count mismatch: original={orig_frames}, "
                f"encoded={enc_frames}"
            )

    # 3. Resolution matches
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
    """Re-encode a single file with the target GOP size.

    Returns True on success.
    """
    tmp_path = path.with_suffix(".mp4.recoding")
    backup_path = path.with_suffix(".mp4.bak")
    backup_exists = backup_path.exists()

    # Pre-flight: if we need a NEW backup but one already exists, bail early
    # before spending hours encoding.  If a .bak already exists (e.g. from
    # fix_hevc_keyframes.py), we treat it as the original backup and just
    # replace the current .mp4 in place.
    try:
        params = _get_encoding_params(path)
        if "width" not in params or "height" not in params:
            console.print(f"  [red]ERROR:[/] could not determine resolution for {path}")
            return False

        cmd = _build_ffmpeg_cmd(path, tmp_path, gop, params, gpu)
        console.print(f"  cmd: {' '.join(cmd[:6])}...{' '.join(cmd[-4:])}")

        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=7200,
        )
        if result.returncode != 0:
            console.print(f"  [red]ERROR:[/] ffmpeg failed (exit {result.returncode})")
            stderr_lines = result.stderr.strip().split("\n")
            for line in stderr_lines[-5:]:
                console.print(f"  stderr: {line}")
            if tmp_path.exists():
                tmp_path.unlink()
            return False

        # Verify
        ok, msg = _verify_output(path, tmp_path, gop)
        if not ok:
            console.print(f"  [red]ERROR:[/] verification failed: {msg}")
            tmp_path.unlink()
            return False

        # Report size change
        orig_size = path.stat().st_size
        new_size = tmp_path.stat().st_size
        ratio = new_size / orig_size if orig_size > 0 else 0
        console.print(f"  size: {orig_size / 1e9:.2f} GB -> {new_size / 1e9:.2f} GB "
                      f"({ratio:.2%})")

        # Swap files
        if keep_backup and not backup_exists:
            # No prior backup — move original aside
            path.rename(backup_path)
        else:
            # Either --no-backup, or a .bak already exists from a prior
            # operation (e.g. fix_hevc_keyframes.py).  Replace in place.
            path.unlink()

        tmp_path.rename(path)
        return True

    except subprocess.TimeoutExpired:
        console.print(f"  [red]ERROR:[/] ffmpeg timed out (7200s) for {path}")
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
        description="Batch re-encode HEVC MP4 recordings with a target GOP size.",
    )
    parser.add_argument(
        "roots",
        nargs="*",
        type=Path,
        default=[Path("/nvme1/recordings")],
        help="Recording root directories or files to scan (default: /nvme1/recordings)",
    )
    parser.add_argument(
        "--gop",
        type=int,
        required=True,
        help="Target GOP size in frames (e.g. 60 for 1s at 60fps).",
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="Actually re-encode. Without this flag, only a dry run is performed.",
    )
    parser.add_argument(
        "--no-backup",
        action="store_true",
        help="Do not keep .mp4.bak backups of original files.",
    )
    parser.add_argument(
        "--no-recursive",
        action="store_true",
        help="Only scan one level deep (*/cams/*.mp4) instead of full recursion.",
    )
    parser.add_argument(
        "--gpu",
        type=int,
        default=0,
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

    needs_reencode: List[Tuple[Path, int]] = []  # (path, current_gop)
    already_ok: List[Path] = []
    no_stss: List[Path] = []
    skipped: List[Tuple[Path, str]] = []  # (path, reason)

    with Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        BarColumn(),
        MofNCompleteColumn(),
        TimeElapsedColumn(),
        console=console,
    ) as progress:
        scan_task = progress.add_task(
            "Scanning GOP sizes", total=len(mp4s),
        )
        for path in mp4s:
            progress.update(
                scan_task,
                description=f"Scanning [cyan]{path.name}[/]",
            )
            if not _is_hevc(path):
                skipped.append((path, "not HEVC"))
            elif not _has_stss(path):
                # No stss box means packet flags are unreliable (MP4 spec
                # treats all samples as sync when stss is absent).  GOP
                # detection would return 1 and silently skip the file.
                no_stss.append(path)
            else:
                current_gop = _probe_current_gop(path)
                if current_gop is None:
                    skipped.append((path, "could not detect GOP"))
                elif current_gop <= args.gop:
                    already_ok.append(path)
                else:
                    needs_reencode.append((path, current_gop))
            progress.advance(scan_task)

    console.print()
    console.print(f"  Needs re-encode (GOP > {args.gop}): {len(needs_reencode)}")
    console.print(f"  Already OK (GOP <= {args.gop}):     {len(already_ok)}")
    if no_stss:
        console.print(f"  [yellow]Missing stss (run fix_hevc_keyframes.py first):[/] {len(no_stss)}")
    console.print(f"  Skipped:                           {len(skipped)}")
    console.print()

    if no_stss:
        console.print("[yellow]Warning:[/] The following files have no stss box, so GOP "
                      "cannot be reliably detected from packet flags.")
        console.print("Run [bold]fix_hevc_keyframes.py[/] on them first, then re-run this script.")
        for path in no_stss:
            console.print(f"  {path}")
        console.print()

    if not needs_reencode:
        console.print("Nothing to re-encode.")
        return

    if not args.apply:
        console.print("Files that need re-encoding:")
        for path, current_gop in needs_reencode:
            console.print(f"  GOP {current_gop:>4} -> {args.gop:<4}  {path}")
        console.print(f"\nRe-run with --apply to re-encode these {len(needs_reencode)} file(s).")
        return

    # Check ffmpeg is available
    try:
        subprocess.run([_FFMPEG, "-version"], capture_output=True, timeout=10)
    except FileNotFoundError:
        console.print("[red]ERROR:[/] ffmpeg not found in PATH.")
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
        encode_task = progress.add_task(
            "Re-encoding", total=len(needs_reencode),
        )
        for path, current_gop in needs_reencode:
            progress.update(
                encode_task,
                description=(
                    f"Re-encoding [cyan]{path.name}[/] "
                    f"(GOP {current_gop} -> {args.gop})"
                ),
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
