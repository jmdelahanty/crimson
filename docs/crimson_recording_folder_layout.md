# Crimson Recording Folder Layout

Purpose: describe the recommended on-disk layout for a single recording on a
user machine, especially for the current Windows MVP flow where the user wants
to load:

- a zarr archive
- the affiliated raw camera video
- the stimulus video

Date anchored: 2026-04-03.

---

## Recommended Layout

Use one recording root per session:

```text
<recording-root>/
  zarr/
    <analysis-or-training>.zarr/
  cams/
    <camera-video>.mp4
  raw/
    <stimulus-video>.mp4
```

Concrete example:

```text
D:\CrimsonData\2026-01-28T19-22-28Z_arena_1_DefaultScreen\
  zarr\
    2026-01-28T19-22-28Z_arena_1_DefaultScreen_training.zarr\
  cams\
    2026-01-28T19-22-28Z_arena_1_DefaultScreen.mp4
  raw\
    stimulus.mp4
```

This is the simplest layout to support with the current auto-discovery logic.

---

## Optional Legacy H5 File

Crimson still has legacy H5 readers, but H5 is no longer the primary runtime
source for stimulus synchronization in the current zarr-driven workflow.

Today, the active stimulus-alignment path comes from the zarr archive's
`analysis/stimulus_runs/.../frame_alignment` data, not from a sidecar H5 file.
See [docs/stimulus_alignment_overview.md](/home/delahantyj@hhmi.org/gitrepos/crimson/docs/stimulus_alignment_overview.md)
and the H5 loader in [src/h5_loader.h](/home/delahantyj@hhmi.org/gitrepos/crimson/src/h5_loader.h#L92).

What H5 is still useful for:

- compatibility with older session formats
- debugging or inspecting historical `frame_metadata`
- loading legacy calibration, events, or tracking snapshots

What H5 is not required for:

- opening the zarr archive
- auto-loading the affiliated raw camera video
- auto-loading the stimulus video
- using the current stimulus-alignment path stored in zarr

If you want to keep a legacy H5 file with the recording, treat it as optional
metadata and place it outside the required `zarr/`, `cams/`, and `raw/` paths:

```text
<recording-root>/
  zarr/
    <analysis-or-training>.zarr/
  cams/
    <camera-video>.mp4
  raw/
    <stimulus-video>.mp4
  metadata/
    <session>_analysis.h5
```

One subtle leftover from the old path is that Crimson may use a zarr
`source_h5` attribute only as a filename hint to derive a stimulus `.mp4`
path. That is a path-resolution hint, not a requirement to load the H5 file at
runtime. See [src/ui_path_config.cpp](/home/delahantyj@hhmi.org/gitrepos/crimson/src/ui_path_config.cpp#L548).

---

## Why This Layout Works

When Crimson is given `--recording <recording-root>`, it:

1. searches the recording root for compatible `.zarr` / `.zr3` archives
2. if none are found there, searches `<recording-root>/zarr/`
3. loads the selected archive
4. tries to auto-resolve the affiliated camera video from zarr metadata
5. tries to auto-resolve the stimulus video from zarr metadata

The relevant code paths are:

- zarr discovery: [src/zarr_loader_internal.cpp](/home/delahantyj@hhmi.org/gitrepos/crimson/src/zarr_loader_internal.cpp#L236)
- `--recording` / `--zarr` CLI handling: [src/red.cpp](/home/delahantyj@hhmi.org/gitrepos/crimson/src/red.cpp#L164)
- affiliated video resolution: [src/ui_path_config.cpp](/home/delahantyj@hhmi.org/gitrepos/crimson/src/ui_path_config.cpp#L429)
- stimulus video resolution: [src/ui_path_config.cpp](/home/delahantyj@hhmi.org/gitrepos/crimson/src/ui_path_config.cpp#L534)

Current affiliated-video search behavior:

- if the zarr metadata provides a relative source video path, Crimson tries:
  - `<archive-root>/<hint>`
  - `<archive-parent>/<hint>`
  - `<recording-root>/<hint>`
  - `<recording-root>/cams/<hint filename>`
  - `<recording-root>/<hint filename>`

Current stimulus-video search behavior:

- if the zarr metadata provides `source_stimulus_video_path`, Crimson tries:
  - `<archive-root>/<hint>`
  - `<archive-parent>/<hint>`
  - `<recording-root>/<hint>`
  - `<recording-root>/raw/<hint filename>`
  - `<recording-root>/<hint filename>`
- if only `source_h5` is present, Crimson derives a `.mp4` path from it and
  tries the same locations
- if that still fails, Crimson scans `<recording-root>/raw/` and uses the first
  supported video file it finds

That is why `cams/` and `raw/` are the recommended folder names.

---

## Recommended Launch Commands

Preferred command:

```powershell
& .\dist\Crimson\bin\redgui.exe --recording "D:\CrimsonData\2026-01-28T19-22-28Z_arena_1_DefaultScreen"
```

Use `--recording` when you want Crimson to discover the zarr archive and try to
auto-load both videos.

Explicit archive command:

```powershell
& .\dist\Crimson\bin\redgui.exe --zarr "D:\CrimsonData\2026-01-28T19-22-28Z_arena_1_DefaultScreen\zarr\2026-01-28T19-22-28Z_arena_1_DefaultScreen_training.zarr"
```

Use `--zarr` when:

- there are multiple candidate archives under the recording root
- you want to force a specific archive
- you are debugging zarr selection behavior

If both are supplied, `--recording` wins and `--zarr` is ignored. See
[src/red.cpp](/home/delahantyj@hhmi.org/gitrepos/crimson/src/red.cpp#L191).

---

## Practical Rules

- Keep one intended recording per root directory.
- Prefer one compatible `.zarr` archive per recording root unless you plan to
  pass `--zarr` explicitly.
- Put the camera video under `cams/`.
- Put the stimulus video under `raw/`.
- Keep the zarr archive under `zarr/` unless you have a strong reason not to.

For the current Windows MVP, this is the layout with the least friction.

---

## Known Edge Cases

- If multiple equally ranked zarr archives exist in the same recording root,
  Crimson treats that as ambiguous and requires an explicit archive path. See
  [src/zarr_loader_internal.cpp](/home/delahantyj@hhmi.org/gitrepos/crimson/src/zarr_loader_internal.cpp#L283).
- If the camera video filename does not match the filename hinted by the zarr
  metadata, affiliated video auto-load may fail even if the file is nearby.
- If the stimulus metadata is incomplete, Crimson may fall back to the first
  supported video file under `raw/`, so avoid multiple unrelated `.mp4` files
  there unless you intend that behavior.

---

## Future Improvements

- Allow explicit CLI flags for camera video and stimulus video paths.
- Make the stimulus-file selection stricter when multiple candidates exist.
- Document a packaged Windows example end to end once the first release
  candidate layout is finalized.
