# Crimson Main-Camera Playback Proxy Plan

Date anchored: 2026-04-04.

Status: shelved / not currently being pursued.

Reason:

- the product cost of managing duplicate camera videos is high
- the team decided to document the Windows RTX A1000 laptop ceiling first
  instead of moving forward with proxy playback immediately
- this document is kept as an archival design option, not as the active next
  implementation step

## Why This Exists

The Windows RTX A1000 laptop investigations have now ruled out most of the
smaller fixes:

- upload/presentation copies were reduced substantially
- stimulus playback was improved by moving stimulus decode to software
- the lightweight playback renderer materially improved playback relative to
  the original renderer
- forcing the app onto the discrete NVIDIA GPU did not materially improve the
  comparable playback run
- main-camera software decode was decisively worse than the current `NVDEC`
  path

What remains on this laptop is a mixed hardware/architecture ceiling:

- `camera_decode_submit_ms` stays near or above the frame budget
- `gl_draw_ms` stays near the frame budget
- full-fidelity `4512x4512 @ 60 fps` playback remains below stable `1.0x`

So the highest-confidence path forward is no longer "one more low-level tweak."
It is a deliberate playback-quality tradeoff for weaker machines.

## Product Goal

Preserve both of these user experiences:

1. smooth or near-smooth playback on weaker GPUs
2. full-fidelity inspection when playback stops

That means:

- during playback, Crimson may show a reduced-resolution proxy for the main
  camera
- when paused, stepping, or scrubbing precisely, Crimson must immediately
  return to the original full-resolution camera source

This is a product feature, not a correctness fix.

## What Must Be Preserved

- exact frame numbering and playback timing
- zarr alignment semantics
- current camera/stimulus synchronization behavior
- overlay alignment
- pause-to-inspect workflows
- zoom/pan behavior

The proxy must therefore:

- have the same frame count as the source video
- have the same FPS/time base as the source video
- map one displayed playback frame to the same logical frame index as the
  original source

## Proposed User-Facing Behavior

During playback:

- if a compatible playback proxy exists, use it for the main camera image
- overlays remain based on the original frame coordinates and are transformed
  into the proxy view
- the UI should make it clear that playback is using a proxy, not the
  full-resolution source

When paused or stepping:

- immediately switch back to the full-resolution source
- keep the same logical camera frame index
- keep the same visible zoom/pan state

This gives the user:

- smoother playback while watching
- full detail when they stop and inspect

## Recommended First Proxy Shape

For the first implementation, use a simple spatially downscaled video proxy:

- same codec family as the source where practical
- same FPS
- same frame count
- lower spatial resolution

Suggested first target:

- half-resolution linear dimensions for the main camera proxy

For a `4512x4512` source, that would be approximately:

- `2256x2256 @ 60 fps`

Why start there:

- it is a large enough reduction to plausibly matter on the laptop
- it is still high enough resolution to remain useful while playing
- it is much simpler than a more dynamic ROI-aware or adaptive proxy system

## Recommended File Layout

Keep the proxy adjacent to the main camera video so recording folders remain
portable and easy to understand.

Recommended structure:

```text
<recording-root>/
  cams/
    Cam2010093_2026-01-28T19-22-28Z_arena_1.mp4
    Cam2010093_2026-01-28T19-22-28Z_arena_1.playback-proxy.mp4
```

Alternative acceptable structure if multiple proxy variants are needed later:

```text
<recording-root>/
  cams/
    Cam2010093_2026-01-28T19-22-28Z_arena_1.mp4
  proxies/
    Cam2010093_2026-01-28T19-22-28Z_arena_1.playback-proxy.mp4
```

For the first pass, the adjacent file in `cams/` is simpler and aligns better
with the existing recording-root conventions.

## Discovery Rules

When the user opens a recording or camera video:

1. load the original full-resolution source as today
2. check for a matching playback proxy next to it
3. if found and the user allows playback proxies, mark it as available

Suggested first matching rule:

- original:
  - `Cam2010093_2026-01-28T19-22-28Z_arena_1.mp4`
- proxy candidate:
  - `Cam2010093_2026-01-28T19-22-28Z_arena_1.playback-proxy.mp4`

The proxy should be treated as optional:

- absence of a proxy must not break the current workflow
- Crimson should simply fall back to the original source

## Switching Rules

Use the proxy only when all of the following are true:

- playback is running
- the main camera is visible
- a compatible proxy exists
- the user has playback proxies enabled

Return to the original source when:

- paused
- stepping frame-by-frame
- scrubbing interactively
- performing actions that require exact full-resolution inspection

The source switch must preserve:

- logical frame index
- time position
- current camera view state

## Compatibility Requirements

Before using a proxy, validate that:

- FPS matches the original
- frame count matches the original, or is close enough to validate exact frame
  mapping reliably
- the proxy dimensions are known

If validation fails:

- ignore the proxy
- log why it was rejected
- continue on the full-resolution source

## Overlay Rules

Overlays must continue to use original frame-space coordinates.

During proxy playback:

- compute overlay placement from the current full-resolution camera-space
  coordinates
- transform into the proxy display rectangle using the same view transform

This avoids duplicating or reauthoring detection/keypoint data for the proxy.

## Generation Strategy

Proxy generation should be an offline/precompute step, not an in-app runtime
requirement in the first pass.

Why:

- runtime proxy generation does not help the laptop when the source workload is
  already too heavy
- offline generation is simpler to validate and cache
- it avoids adding more startup latency to Crimson itself

The first pass should therefore assume:

- proxies are generated outside the app
- Crimson only detects, validates, and uses them

## Telemetry Requirements

Add explicit perf/session metadata so proxy runs are easy to compare.

At minimum record:

- whether a playback proxy was available
- whether it was active on each sample
- proxy path
- proxy dimensions
- original dimensions

This makes A/B comparisons straightforward in the existing perf tooling.

## Rollout Plan

### Phase 1: Documentation

- record why proxy playback is now the highest-confidence next step
- record the intended file naming and switching rules

### Phase 2: Detection + Metadata

- detect a matching playback proxy next to the main camera source
- validate compatibility
- expose availability in the UI and perf metadata

Acceptance:

- proxy discovery works without changing the current no-proxy workflow

### Phase 3: Playback-Only Source Switching

- when playback starts, decode/display from the proxy if available
- when paused, step, or precise-scrub events occur, return to the original
  source

Acceptance:

- playback uses the proxy
- paused inspection uses the original full-resolution source
- logical frame index remains stable across the switch

### Phase 4: Overlay Validation

- ensure bounding boxes and keypoints stay aligned in proxy playback mode
- ensure zoom/pan behavior remains meaningful and consistent

Acceptance:

- overlays remain aligned in both proxy and full-resolution modes

### Phase 5: Optional Tooling

- add a helper script or documented ffmpeg recipe to generate a compatible
  proxy offline

Acceptance:

- the proxy format and naming rules are easy for users to reproduce

## Success Criteria

- playback on the Windows RTX A1000 laptop reaches or stays much closer to
  stable `1.0x`
- paused inspection still returns to full-resolution source frames
- overlays remain aligned
- no proxy is required for existing workflows
