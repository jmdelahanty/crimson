# Video Repair Scripts

## macOS Keypoint Quality Timeline

Launch the full-duration keypoint-v2 fixture paused at frame 1000:

```bash
scripts/launch_macos_keypoint_quality_timeline.sh
```

Pass another starting frame as the first argument. Validate the build and
mounted inputs without opening Crimson with `--check`:

```bash
scripts/launch_macos_keypoint_quality_timeline.sh 250000
scripts/launch_macos_keypoint_quality_timeline.sh --check
```

The launcher keeps Crimson attached to the terminal so repository and timeline
diagnostics remain visible. Use the Keypoints tab in Frame Inspect, then select
`Keypoint Quality Timeline`.

## macOS Playback Smoke

Build the native app, then run the representative AVFoundation-to-Metal viewer
smoke:

```bash
cmake --build --preset build-macos-arm64-release
scripts/macos_gui_smoke_playback.sh /Volumes/recordings/cams/camera.mp4
```

Supply another camera video as the first argument or through
`CRIMSON_MACOS_PLAYBACK_SMOKE_VIDEO`. Supply the inclusive validation range as
the second argument or through `CRIMSON_MACOS_PLAYBACK_SMOKE_RANGE`:

```bash
scripts/macos_gui_smoke_playback.sh \
    /Volumes/recordings/cams/camera.mp4 \
    70000:70300
```

The app owns the logical clock. A passing record reports requested and presented
frame identity, PTS error, bounded native-buffer depth, intentional source-frame
skips, late presentations, process memory, and thermal state.

Camera-aligned stimulus presentation has a separate production smoke. It opens
the representative main camera and analysis Zarr by default, or accepts video,
Zarr, and inclusive camera range arguments:

```bash
scripts/macos_gui_smoke_stimulus.sh

scripts/macos_gui_smoke_stimulus.sh \
  /Volumes/recordings/cams/camera.mp4 \
  /Volumes/recordings/zarr/analysis.zarr \
  1024:1324
```

The smoke requires both `[AppleVideoSmoke] PASS` and
`[AppleStimulusSmoke] PASS`. The latter rejects mapping/decoded identity
mismatches and nonzero camera-frame presentation skew.

Crop-only and combined camera/stimulus/crop smokes accept either the derived
acquisition video or live geometry route:

```bash
scripts/macos_gui_smoke_crop.sh VIDEO ZARR 1024:1324 acquisition
scripts/macos_gui_smoke_crop.sh VIDEO ZARR 1024:1324 geometry

scripts/macos_gui_smoke_multistream.sh VIDEO ZARR 1024:1324 acquisition
scripts/macos_gui_smoke_multistream.sh VIDEO ZARR 1024:1324 geometry
```

The multistream smoke includes exact pause, one-frame step, backward seek,
forward seek, and end-frame settlements. Queue bounds are checked against the
configured capacities. Camera and stimulus default to six frames and can be
changed with `--video-buffer-size` and `--stimulus-buffer-size`; the acquisition
crop decoder uses 32 frames.

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

## Maintained Workspace Reference Capture

`capture_redgui_workspace_reference.sh` captures the undecorated maintained
Linux `redgui` client from a fresh ImGui profile and writes PNG, geometry,
X11, log, ini, and JSON metadata. It is read-only with respect to the loaded
archive.

```bash
export DISPLAY=:1
export XAUTHORITY=/run/user/$(id -u)/.mutter-Xwaylandauth.<current>
export LD_LIBRARY_PATH=/opt/orange/lib/ffmpeg-nvidia/lib:${LD_LIBRARY_PATH:-}

scripts/capture_redgui_workspace_reference.sh \
  /absolute/path/to/redgui \
  /absolute/path/to/analysis.zarr \
  /tmp/crimson-phase5l-reference \
  initial_loaded_1920x1080 \
  1920 1080
```

Pass `-` as the Zarr argument for an empty-workspace capture. See
`docs/crimson_macos_phase5l_workspace_inventory.md` for classification and
`docs/reference/phase5l/manifest.json` for the audited capture set.

For an exact app-side state, set the state, frame, and an optional layout
profile together:

```bash
CRIMSON_REFERENCE_UI_STATE=overlays \
CRIMSON_REFERENCE_FRAME=56 \
CRIMSON_REFERENCE_UI_TIMEOUT=60 \
CRIMSON_REFERENCE_IMGUI_INI=docs/reference/phase5l/linux/arranged_reference_1920x1080.imgui.ini \
scripts/capture_redgui_workspace_reference.sh \
  /absolute/path/to/redgui \
  /absolute/path/to/analysis.zarr \
  /tmp/crimson-phase5l-overlays \
  overlays_exact_f000056_1920x1080 \
  1920 1080
```

Exact bundles use the application-produced OpenGL front-buffer PNG as the
primary image and retain `.x11.png` separately for window-system diagnostics.
The ready marker must prove the exact presented frame, complete camera ring,
state-specific evidence, and 60 stable frames. Set
`CRIMSON_REFERENCE_KEEP_RUN_DIR=1` to preserve temporary logs after failure.
For an overlay readiness failure, also set
`CRIMSON_REFERENCE_MASK_PERF_LOG=1`; the harness then preserves per-frame mask
and eye-geometry counters as `<state>.mask-perf.jsonl` on success or in the
retained run directory on failure.

On Windows, use `capture_redgui_workspace_reference.ps1`. It applies the same
exact marker checks and creates both the application front-buffer PNG and a
separate Win32 client capture:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File scripts\capture_redgui_workspace_reference.ps1 `
  -Redgui C:\crimson\release\redgui.exe `
  -Zarr Z:\recordings\analysis.zarr `
  -OutputDirectory C:\temp\crimson-phase5l-reference `
  -UiState overlays `
  -Frame 56 `
  -StateName overlays_exact_f000056_1920x1080 `
  -ImguiIni docs\reference\phase5l\linux\arranged_reference_1920x1080.imgui.ini `
  -SourceRevision $(git rev-parse HEAD)
```

The Win32 companion uses a foreground screen copy, so keep the client
unobscured and outside the taskbar. The application-produced front buffer
remains the pixel authority. Add `-ValidateOnly` to verify paths, parameters,
and Win32 helper compilation without launching the GUI.
See `docs/reference/phase5l/windows/README.md` for the evidence boundary and
the complete Windows matrix.

### macOS Capture and Cross-Platform Comparison

With the reference recording mounted at `/Volumes/johnsonlab`, capture all six
Mac states through the app-produced Metal framebuffer:

```bash
scripts/capture_crimson_macos_workspace_reference.sh \
  /tmp/crimson-phase5l4-macos
```

The harness validates the exact frame, 1920x1080 logical/framebuffer size,
read-only contract, 60 stable frames, semantic snapshot, overlays, live crop,
alternate `gaze` representation, and stimulus alignment. Inputs and the app
path can be overridden with the `CRIMSON_MACOS_REFERENCE_*` environment
variables declared at the top of the script.

Compare the equivalent loaded states to the maintained Linux references:

```bash
tools/phase5l_workspace_compare.py \
  --linux-dir docs/reference/phase5l/linux \
  --macos-dir /tmp/crimson-phase5l4-macos \
  --contract docs/reference/phase5l/acceptance_contract.json \
  --output /tmp/crimson-phase5l4-acceptance.json
```

The comparator has no third-party Python dependency. It decodes PNGs with the
standard library, validates semantic structure and anchors, applies only the
documented regions/masks, and exits nonzero if any acceptance check fails.

### Phase 5M Polar Capture and Comparison

Capture the exact frame-1024 maintained polar state on the authenticated Linux
display using the frozen Phase 5L workspace arrangement:

```bash
scripts/capture_redgui_polar_reference.sh \
  /absolute/path/to/redgui \
  /absolute/path/to/analysis.zarr \
  /tmp/crimson-phase5m-linux
```

Capture the equivalent native Mac/Metal state from the mounted reference
recording:

```bash
scripts/capture_crimson_macos_polar_reference.sh \
  /tmp/crimson-phase5m-macos
```

Both harnesses require a 1920x1080 framebuffer, exact camera frame 1024, a
read-only marker, 60 stable frames, a ready two-point polar scene, and a
canonical semantic signature. Compare the captures with the checked-in
contract:

```bash
tools/phase5m_polar_compare.py \
  --linux-dir /tmp/crimson-phase5m-linux \
  --macos-dir /tmp/crimson-phase5m-macos \
  --contract docs/reference/phase5m/acceptance_contract.json \
  --output /tmp/crimson-phase5m-acceptance.json
```

The standard-library comparator validates provenance, exact-frame state,
logical scene geometry, ordered text, semantic signatures, opaque marker
channels, vector coverage/outliers, and raster marker anchors. Its only masks
and exclusions are declared in the acceptance contract.

### Phase 5N Stimulus Overlay Capture and Comparison

Capture exact camera frame 1024 from the maintained NVIDIA/OpenGL client on
its authenticated display:

```bash
scripts/capture_redgui_stimulus_overlay_reference.sh \
  /absolute/path/to/redgui \
  /absolute/path/to/analysis.zarr \
  /tmp/crimson-phase5n-linux
```

Capture the equivalent native Mac/Metal state from the mounted reference
recording:

```bash
scripts/capture_crimson_macos_stimulus_overlay_reference.sh \
  /tmp/crimson-phase5n-macos
```

Both wrappers require a read-only exact-frame marker, the current production
stimulus descriptor, 60 stable frames, one event panel, ordered event text,
and the canonical stimulus-camera-overlay signature. Compare the artifacts:

```bash
tools/phase5n_stimulus_overlay_compare.py \
  --linux-dir /tmp/crimson-phase5n-linux \
  --macos-dir /tmp/crimson-phase5n-macos \
  --contract docs/reference/phase5n/acceptance_contract.json \
  --output /tmp/crimson-phase5n-acceptance.json
```

The standard-library comparator enforces exact frame, descriptor, geometry,
primitive, color, text, anchor, layer-order, and semantic-signature equality.
It also requires visible panel-border and text rasters within the documented
tolerances and masks.
