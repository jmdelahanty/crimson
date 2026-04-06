Crimson Windows App
===================

This folder is a self-contained internal Crimson app drop for run-only users.

What to run
-----------

Preferred:
- run `install_crimson.cmd`

Or from PowerShell:
- run `install_crimson.ps1`

Default install location:
- `%LOCALAPPDATA%\Crimson`

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

Notes
-----

- Keep the `.dll` files next to `redgui.exe` in `bin\`.
- Do not copy only `redgui.exe`.
- Do not move files out of this folder by hand.
- If Windows reports a missing `.dll`, report the exact filename.
