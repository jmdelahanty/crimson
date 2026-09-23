# Phase 5L Windows Reference Evidence

Windows runtime evidence was not available on 2026-07-16. The maintained
Windows build shares `red.cpp` and the ImGui window modules with Linux, but that
only establishes source topology. It does not establish Windows DPI scaling,
font rasterization, native frame extents, client sizing, or runtime behavior.

Do not copy Linux PNGs into this directory or label them as Windows evidence.

## Required Capture Run

Use a real Windows NVIDIA workstation with the maintained `redgui.exe` and the
same June 14 GoodCopBadCop archive (or a byte-identical local/network path).
Launch from a new empty working directory so no prior `imgui.ini` affects the
first-use layout.

The checked-in PowerShell harness performs the exact-state portion of the run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File scripts\capture_redgui_workspace_reference.ps1 `
  -Redgui C:\crimson\release\redgui.exe `
  -Zarr Z:\recordings\2026-06-14T21-12-08Z_arena_1_GoodCopBadCop\zarr\2026-06-14T21-12-08Z_arena_1_GoodCopBadCop_analysis.zarr `
  -OutputDirectory C:\temp\crimson-phase5l-reference `
  -UiState overlays `
  -Frame 56 `
  -StateName overlays_exact_f000056_1920x1080 `
  -ImguiIni docs\reference\phase5l\linux\arranged_reference_1920x1080.imgui.ini `
  -SourceRevision $(git rev-parse HEAD)
```

It creates an isolated working directory, sizes the actual Win32 client to
`1920x1080` with DPI-aware Win32 APIs, validates the atomic ready marker, and
copies the application OpenGL front buffer as the primary PNG. A separate
`.win32.png` records the visible compositor/client result. Keep the Crimson
window unobscured and the taskbar out of its client area for that companion
capture. Metadata records HWND, class,
client and outer bounds, DPI, OS, PowerShell, GPU, driver, executable hash, and
both PNG hashes. `-KeepRunDirectory` retains temporary files for diagnosis.
Add `-ValidateOnly` to compile the embedded Win32 helper and validate all paths
and parameters without launching `redgui`.

The script has been source-reviewed but cannot be counted as Windows evidence
until it executes successfully on a real Windows host and every output is
inspected. Its presence does not change the `runtime_evidence: unavailable`
manifest boundary.

Capture these client states at both `1920x1080` and `1280x800` logical pixels:

- initial empty workspace;
- initial loaded/paused workspace at frame zero;
- File menu;
- documentation-only arranged workspace using the same positions in
  `../linux/arranged_reference_1920x1080.imgui.ini`; and
- Diagnostics visible with loaded runtime state.

At `1920x1080`, also run the shared deterministic presets from a profiled
working directory:

```text
--ui-reference-state workspace      --ui-reference-frame 56
--ui-reference-state overlays       --ui-reference-frame 56
--ui-reference-state crop-preview   --ui-reference-frame 56
--ui-reference-state analysis-eye   --ui-reference-frame 56
--ui-reference-state stimulus-debug --ui-reference-frame 1024
```

The PowerShell harness supplies the required ready-file and timeout arguments.
Its marker validation matches Linux: exact camera identities, 60 stable
frames, complete camera ring, state-specific crop/overlay/analysis evidence,
and exact mapped stimulus presentation. Run `analysis-tail-stimulus` only
against an archive that actually contains both tail kinematics and stimulus
analysis.

Use Win32 `GetClientRect` to verify client dimensions and `SetWindowPos` with
the non-client delta from `AdjustWindowRectExForDpi` to size the outer window.
Capture the client rectangle, not the decorated desktop rectangle. Record:

- Crimson source revision, dirty status, executable SHA-256, and build preset;
- Windows version, GPU, NVIDIA driver, display DPI, and content scale;
- archive path, timestamp, and metadata-manifest hash;
- requested and actual client size, HWND title/class, and outer frame extents;
- requested/presented frame and every UI action used to reach the state;
- PNG SHA-256 and capture-tool version; and
- whether the state is deterministic, observational, diagnostics, or deferred.

Populate `docs/reference/phase5l/manifest.json` with the resulting paths and
replace the `runtime_evidence: unavailable` marker only after inspecting every
PNG for nonblank content, clipping, and overlap.
