# Crimson Phase 5L.4 Visual and Cross-Platform Acceptance

Date: 2026-07-17

Phase 5L.4 closes the visual acceptance checkpoint for the maintained
read-only workspace on macOS and Linux/NVIDIA. Windows runtime evidence remains
explicitly deferred by the user; no Linux result is represented as Windows
evidence.

## Capture Contract

Both applications capture an app-produced 1920x1080 framebuffer with no
operating-system chrome. Every loaded capture is read-only, reaches the exact
presented frame, and remains stable for at least 60 rendered frames. The shared
states are:

| State | Frame | Required content |
| --- | ---: | --- |
| workspace | 56 | maintained default window roles and camera transport |
| overlays | 56 | production keypoint, heading, subject-mask, and label data |
| crop-preview | 56 | live-geometry crop at camera frame 56 |
| analysis-eye | 56 | alternate `gaze` eye representation |
| stimulus-debug | 1024 | stimulus target and presented frame 1024 |

The Mac harness also captures the unloaded initial workspace. The exact state,
frame, dimensions, stable-frame count, write contract, semantic snapshot, and
state-specific readiness are stored beside each PNG. The Linux marker records
the actual camera media rectangle separately from the surrounding ImPlot axes.

## Comparator

`tools/phase5l_workspace_compare.py` consumes the frozen contract in
`docs/reference/phase5l/acceptance_contract.json`. It checks:

- exact state, frame, dimensions, read-only contract, and content readiness;
- exact window presence, role order, hierarchy, visible labels, and collapsed
  state;
- window bounds, camera surface anchor, and the five camera transport anchors
  within 0.5 display pixels;
- exact camera transport label order and buffered-frame identity/order; and
- the clean production camera region after source-normalized block averaging,
  with every non-antialiased channel within 3/255.

The final report passes all 46 checks. Window and control anchor deltas are
0.0 pixels. The clean camera comparison has a maximum block-channel delta of
2.109375/255 and a mean of 0.755706/255.

Phase 5 scientific thresholds remain frozen at 0.25 source pixels for source
geometry, 0.995 mask IoU, 0.995 vector coverage within one pixel, no vector
outlier beyond two pixels, and 3/255 for non-antialiased channels. The portable
`overlay_scene_contract_tests` proves source geometry. The offscreen Metal
`apple_read_only_overlay_metal_tests` renders controlled mask and vector
fixtures and enforces the raster thresholds directly.

## Masks and Exclusions

Every exclusion is machine-readable in the acceptance contract and copied into
the generated report:

- Font glyph pixels are excluded because Roboto and Fork Awesome rasterization
  differs between OpenGL and Metal. Exact visible text and anchors are compared
  through the shared ImGui semantic recorder.
- Backend diagnostics and deferred panel interiors are excluded from pixels;
  their role, order, visibility, hierarchy, labels, and outer bounds remain
  structural checks.
- Linux ImPlot decoration outside the recorded camera media viewport is
  excluded from the camera image comparison.
- Picture-in-picture regions and renderer-owned labels are excluded from the
  clean camera raster region. Their availability and geometry are asserted by
  exact-state markers and portable contracts.
- The production subject is fewer than 20 display pixels in the reference
  workspace and is covered by renderer-specific labels and insets. Its raw
  on-screen overlay pixels are therefore not claimed as equivalent; the frozen
  mask, vector, outlier, color, and source-geometry tolerances are enforced by
  the controlled fixtures above.

No operating-system chrome mask is required because both golden PNGs are
application-produced framebuffer captures.

## Validation Evidence

macOS arm64:

- `ctest --preset test-macos-arm64-headless --output-on-failure`: 36/36 passed;
- the Metal overlay fixture passed with 11 primitives and 199 triangles; and
- acquisition and live-geometry multistream production smokes passed frames
  1024 through 7024 with zero maximum camera skew; and
- the five-state comparator against the fresh NVIDIA references passed 46/46.

The acquisition smoke completed in 39.945 seconds with a maximum lag of 444
frames against the fixed 500-frame limit. The first live-geometry attempt after
remounting the network share reached the exact final frame but failed the
performance gate at 744 lag frames. An immediate full-interval retry passed in
38.117 seconds with a maximum lag of five frames and no threshold change. This
is recorded as a transient network/cache stall rather than hidden or converted
into a weaker acceptance limit.

Linux/NVIDIA, isolated checkout with CUDA architectures 80 and 86:

- `ctest --test-dir build/nvidia-validation-5l4 --output-on-failure`: 26/26
  passed; and
- authenticated `scripts/gui_smoke_playback.sh` passed frames 0 through 300,
  presenting frame 300 in 2.99474 seconds.

The final six-state Mac capture set was refreshed from executable SHA-256
`9188bd01dcb3fe93382ac84eadfebcdec0d38d5546dae4c3e897d86f101dc0b7`.
All image and marker checksum sidecars validate.

Windows source and capture tooling are present, but a real Windows build and
runtime capture were deferred by the user. This is an explicit platform
evidence exception, not a successful Windows result.

## Reproduction

```bash
scripts/capture_crimson_macos_workspace_reference.sh /tmp/crimson-phase5l4-macos

tools/phase5l_workspace_compare.py \
  --linux-dir docs/reference/phase5l/linux \
  --macos-dir docs/reference/phase5l/macos \
  --contract docs/reference/phase5l/acceptance_contract.json \
  --output docs/reference/phase5l/acceptance_report.json
```

Both capture harnesses fail closed on missing inputs, wrong frames, incomplete
state readiness, archive mutation contract changes, or fewer than 60 stable
frames.
