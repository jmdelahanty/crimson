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

Supported stack
---------------

This app drop was built and packaged against one supported Windows dependency
stack. It is not meant to be rebuilt or reconfigured on each user machine.

Run-only users provide:
- a supported Windows x64 machine
- a compatible NVIDIA GPU and driver

The app drop provides:
- `redgui.exe`
- the runtime DLLs matched to the build stack
- fonts, config, and helper scripts

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
- Run-only users should not need to install the CUDA Toolkit. They need a
  compatible NVIDIA driver; this app drop should contain the runtime DLLs
  Crimson needs.
- If a machine is also being used to build Crimson from source, install the
  CUDA Toolkit separately and avoid replacing the display driver unless that is
  an intentional machine-maintenance step.
- If Windows reports a missing `.dll`, report the exact filename.
- If Crimson hard-crashes, check `%LOCALAPPDATA%\Crimson\CrashDumps` for a
  `.dmp` file and matching `.txt` sidecar.
