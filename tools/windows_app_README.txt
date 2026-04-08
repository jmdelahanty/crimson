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

Multi-GPU note:
- on machines with multiple CUDA GPUs, Crimson may prompt on first launch for
  which GPU to use
- the selection is remembered in `%LOCALAPPDATA%\Crimson\config\cuda_device.json`

What gets installed
-------------------

- `bin\redgui.exe`
- required runtime `.dll` files in `bin\`
- app resources under `share\crimson\...`

How to launch after install
---------------------------

- `<install-root>\bin\redgui.exe`

Update an existing install
--------------------------

Run the installer again from this published app drop with replace enabled.

Examples:

PowerShell:
- `powershell -ExecutionPolicy Bypass -File ".\install_crimson.ps1" -ReplaceExisting`

Double-click:
- run `install_crimson.cmd`

Desktop shortcut
----------------

- `install_crimson.cmd` does not create a Desktop shortcut by default
- use `install_crimson.ps1 -CreateDesktopShortcut` if you want one created automatically

Checks
------

- `install_crimson.ps1` runs cheap preflight and postinstall runtime checks by default
- use `check_crimson_runtime.cmd` later if you want to re-check the install or driver visibility

Notes
-----

- Keep the `.dll` files next to `redgui.exe` in `bin\`.
- Do not copy only `redgui.exe`.
- Do not move files out of this folder by hand.
- If Windows reports a missing `.dll`, report the exact filename.
- If Crimson hard-crashes, check `%LOCALAPPDATA%\Crimson\CrashDumps` for a
  `.dmp` file and matching `.txt` sidecar.
