Crimson Linux App Drop
======================

This folder is a staged Crimson Linux install tree produced by CMake install
rules.

Expected layout:

  bin/redgui
  bin/crimson
  share/crimson/fonts/
  share/crimson/config/
  check_crimson_runtime.sh
  release.json

Recommended launch path:

  ./bin/crimson --zarr /path/to/archive.zarr

The launcher resolves the install root and prepends app-local private library
directories to LD_LIBRARY_PATH before starting bin/redgui. Set
CRIMSON_LINUX_STRICT_RUNTIME=1 to avoid inheriting an existing LD_LIBRARY_PATH,
or set CRIMSON_EXTRA_LD_LIBRARY_PATH to append a managed module/runtime root.

Before handing this app drop to a user, run:

  ./check_crimson_runtime.sh --require-nvidia-smi

If a real GUI display is available, add:

  ./check_crimson_runtime.sh --require-nvidia-smi --require-gl

To write a dependency audit manifest:

  ./check_crimson_runtime.sh --write-dependency-manifest dependency_manifest.json

Developer checks allow absolute managed-workstation dependency roots as
warnings. Release checks are stricter:

  ./check_crimson_runtime.sh --mode release --write-dependency-manifest dependency_manifest.json

If release mode should allow a managed module/runtime root, set:

  CRIMSON_ALLOWED_RUNTIME_ROOTS=/path/to/runtime/root1:/path/to/runtime/root2

Run-only users need a compatible NVIDIA display driver and access to the data
they want to open. They should not need the CUDA Toolkit, TensorRT SDK, OpenCV
development tree, FFmpeg headers, or a compiler just to launch a complete app
drop.
