# Video Repair Scripts

Quick reference for fixing seekability issues in MP4 recordings read by crimson.

## Script Summary

| Script | Codec | Fixes | Method | Directory pattern |
|--------|-------|-------|--------|-------------------|
| `fix_hevc_keyframes.py` | HEVC | Missing stss box | Remux (MP4Box) | `cams/*.mp4` |
| `reencode_hevc_gop.py` | HEVC | GOP too large | Re-encode (NVENC) | `cams/*.mp4` |
| `check_h264_keyframes.py` | H.264 | Missing stss box | Remux (MP4Box) | `raw/*.mp4` |
| `reencode_h264_gop.py` | H.264 | Missing stss + GOP | Re-encode (NVENC) | `raw/*.mp4` |

## Recommended Workflow

### HEVC camera recordings (`cams/`)

Run in order — the GOP script needs a valid stss to detect the current GOP size:

```bash
# 1. Fix missing stss (remux only, fast)
python scripts/fix_hevc_keyframes.py /nvme1/recordings --apply

# 2. Re-encode with smaller GOP
python scripts/reencode_hevc_gop.py /nvme1/recordings --gop 60 --apply
```

### H.264 rendered recordings (`raw/`)

A single re-encode pass fixes both stss and GOP:

```bash
python scripts/reencode_h264_gop.py /nvme1/recordings --gop 30 --apply
```

If you only need to fix missing stss (GOP is already acceptable), use the lighter remux:

```bash
python scripts/check_h264_keyframes.py /nvme1/recordings --apply
```

## Dependencies

| Dependency | Path / install | Used by |
|------------|----------------|---------|
| ffmpeg (NVENC) | `/opt/orange/lib/ffmpeg-nvidia/bin/ffmpeg` | `reencode_hevc_gop.py`, `reencode_h264_gop.py` |
| ffprobe (NVENC) | `/opt/orange/lib/ffmpeg-nvidia/bin/ffprobe` | All scripts except `fix_hevc_keyframes.py` |
| MP4Box | `sudo apt install gpac` | `fix_hevc_keyframes.py`, `check_h264_keyframes.py` |
| Python 3 | System | All |
| `rich` | `pip install rich` | `reencode_hevc_gop.py`, `reencode_h264_gop.py`, `check_h264_keyframes.py` |

## Common Flags

All scripts default to **dry-run mode** — they scan and report without modifying files.

| Flag | Description |
|------|-------------|
| `--apply` | Actually modify files (without this, only a dry run is performed) |
| `--no-backup` | Skip creating `.mp4.bak` backups of originals |
| `--no-recursive` | Scan one level deep (`*/cams/*.mp4` or `*/raw/*.mp4`) instead of full recursion |
| `--gop N` | Target GOP size in frames (required for re-encode scripts) |
| `--gpu N` | CUDA device index for NVENC, default 0 (re-encode scripts only) |

## Performance Notes

- **stss-only fixes** (`fix_hevc_keyframes.py`, `check_h264_keyframes.py`): Fast — MP4Box remuxes without decoding.
- **HEVC re-encode**: ~6 min per 12 GB file (4512x4512 @ 60fps).
- **H.264 re-encode**: Near-instant — files are small (344x344 @ 120fps).

## Further Reading

- [docs/acquisition_hevc_keyframe_fix_spec.md](../docs/acquisition_hevc_keyframe_fix_spec.md) — HEVC keyframe flag issue and acquisition-side fix
- [docs/citrus_h264_video_encoding_contract.md](../docs/citrus_h264_video_encoding_contract.md) — H.264 encoding requirements for rendered recordings
