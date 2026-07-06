# Crimson Ubuntu and macOS Platform Strategy

Purpose: summarize the practical build and distribution path for Ubuntu/Linux
and macOS, based on Crimson's current CUDA/TensorRT/OpenGL architecture.

Date anchored: 2026-07-06.

Related docs:

- [docs/crimson_supported_dependency_stack_matrix.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_supported_dependency_stack_matrix.md)
- [docs/crimson_cuda_driver_toolkit_and_presets.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_cuda_driver_toolkit_and_presets.md)
- [docs/crimson_packaging_and_distribution_plan.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_packaging_and_distribution_plan.md)
- [docs/crimson_windows_installation_procedures.md](/home/delahantyj@hhmi.org/gitrepos/crimson-ui-monolith/docs/crimson_windows_installation_procedures.md)

## Current Reality

Crimson is currently an NVIDIA-first desktop application:

- `CMakeLists.txt` declares `project(redgui LANGUAGES CXX CUDA)`.
- The maintained presets target CUDA `12.4`, TensorRT `10.0.1.6`, and OpenCV
  `4.10.0`.
- The video path links FFmpeg plus NVIDIA decode libraries such as `nvcuvid`.
- The inference path links TensorRT.
- The UI/rendering path uses GLFW, GLEW, ImGui, ImPlot, and OpenGL.
- CUDA/NPP code is compiled directly into the app.

This means "cross-platform" is not just a packaging question. Ubuntu and
Windows can share the NVIDIA dependency family. macOS cannot, because modern
macOS has no CUDA or TensorRT support.

## Ubuntu / Linux

Ubuntu is the tractable platform path. It is mostly a reproducibility,
packaging, and validation problem.

The baseline Linux stack already exists in shared presets:

```text
linux-trt10-cuda12.4-release
linux-trt10-cuda12.4-debug
```

Those presets should remain aligned with:

```text
CUDA Toolkit 12.4
TensorRT 10.0.1.6
OpenCV 4.10.0
FFmpeg/NVIDIA codec development root
```

The near-term Ubuntu work should be:

1. Pick supported Ubuntu versions, likely Ubuntu 22.04 and/or 24.04.
2. Add a Linux equivalent of the Windows dependency path checker.
3. Verify clean configure/build/install from the shared Linux preset.
4. Make `cmake --install` produce a runnable staged tree.
5. Fix runtime resource lookup so fonts/config/models are executable-relative,
   not current-working-directory-relative.
6. Set Linux RPATH or launcher behavior so required private libraries are found
   from the install tree.
7. Add a GUI smoke command for the staged install.
8. Only after the staged tree is reliable, decide between a tarball, AppImage,
   `.deb`, or internal module-style deployment.

Ubuntu should not need a renderer rewrite. It should use the existing
CUDA/OpenGL/TensorRT architecture.

## macOS

macOS is a different product slice, not a simple port of the current build.

The blockers are structural:

- CUDA is unavailable on modern macOS.
- TensorRT is unavailable on macOS.
- NVDEC/CUVID and NVENC are unavailable on macOS.
- OpenGL is deprecated on macOS and should not be the long-term rendering
  foundation.
- Current CMake requires CUDA as a project language, so a macOS build would
  need a no-CUDA build variant before it can even configure.

MLX is not a rendering replacement. MLX is relevant to ML/tensor execution on
Apple Silicon. Rendering would need Metal or a Metal-backed UI/rendering layer.

A realistic macOS stack would likely require:

- Metal for image presentation and overlay rendering.
- VideoToolbox for hardware video decode.
- CoreML, MPS, MLX, or ONNX Runtime with a Metal/CoreML execution provider for
  inference, replacing TensorRT.
- A no-CUDA build variant.
- Backend abstractions for decode, inference, GPU buffers, and texture upload.

## Recommended macOS Slices

Do not target full parity first. Use staged scope.

### Slice 1: Archive Viewer

Goal: open useful Crimson/Palette data on macOS without live inference.

Scope:

- build with no CUDA language enabled
- load Zarr metadata and overlays
- display existing videos through CPU decode or VideoToolbox
- render overlays through a macOS-compatible UI/render path
- no TensorRT inference
- no CUDA/NPP processing
- no NVDEC/NVENC

This is still a meaningful project, but it avoids replacing every GPU path at
once.

### Slice 2: Accelerated Viewer

Goal: make viewing and random seeking feel native on Apple hardware.

Scope:

- VideoToolbox decode
- Metal texture presentation
- efficient CPU/GPU upload path for overlay data
- platform-independent frame and texture interfaces

### Slice 3: Inference Parity

Goal: replace the TensorRT-specific model execution path.

Possible backends:

- CoreML for packaged model deployment
- MPS or MLX for Apple Silicon tensor execution
- ONNX Runtime with an Apple backend if model conversion and performance are
  acceptable

This slice should only start after the viewer slice proves the app structure can
run without CUDA.

## Architecture Work Needed Before macOS

The work that helps macOS also improves testability on Linux and Windows:

- Make CUDA an optional build feature instead of a required project language.
- Split decode behind an interface:
  - FFmpeg/NVDEC on NVIDIA platforms
  - VideoToolbox or software decode on macOS
- Split inference behind an interface:
  - TensorRT on NVIDIA platforms
  - placeholder/no-op initially on macOS
  - later CoreML/MPS/MLX/ONNX backend
- Split presentation/upload behind an interface:
  - OpenGL on current platforms
  - Metal on macOS
- Keep Zarr readers and overlay preparation independent of GPU backend.

## Policy

Ubuntu/Linux should stay on the current NVIDIA baseline stack unless the team
explicitly promotes a new stack.

macOS should not be described as "supported" until a no-CUDA viewer build can:

- configure on macOS
- build on macOS
- open a real Zarr/video session
- display key overlays
- seek reliably
- package into a runnable app bundle or staged folder

Until then, macOS is exploratory. The first macOS milestone should be a viewer,
not full inference/editing parity.
