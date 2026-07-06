Crimson Windows App
===================

This folder is a self-contained internal Crimson app drop for run-only users.

What to run
-----------

Preferred:
- run `install_crimson.cmd`

Or from PowerShell:
- run `install_crimson.ps1`
- add `-CreateDesktopShortcut` if you want the installer to create a Desktop shortcut

Quick runtime check:
- run `check_crimson_runtime.cmd`
- or run `check_crimson_runtime.ps1`

Default install location:
- `%LOCALAPPDATA%\Crimson`

How to launch after install
---------------------------

- `<install-root>\bin\redgui.exe`

Update an existing install
--------------------------

PowerShell:
- `powershell -ExecutionPolicy Bypass -File ".\install_crimson.ps1" -ReplaceExisting`

Double-click:
- run `install_crimson.cmd`

CUDA GPU preference
-------------------

Use `set_crimson_cuda_device.cmd` if you need to write or clear Crimson's saved
CUDA GPU preference. This writes:

- `%LOCALAPPDATA%\Crimson\config\cuda_device.json`

Examples:

- `powershell -ExecutionPolicy Bypass -File ".\set_crimson_cuda_device.ps1"`
- `powershell -ExecutionPolicy Bypass -File ".\set_crimson_cuda_device.ps1" -DeviceIndex 1`
- `powershell -ExecutionPolicy Bypass -File ".\set_crimson_cuda_device.ps1" -ClearSavedChoice`

Notes
-----

- Keep the `.dll` files next to `redgui.exe` in `bin\`.
- Do not copy only `redgui.exe`.
- Do not move files out of this folder by hand.
- If Windows reports a missing `.dll`, report the exact filename.
- If Crimson hard-crashes, check `%LOCALAPPDATA%\Crimson\CrashDumps` for a
  `.dmp` file and matching `.txt` sidecar.
