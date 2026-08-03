# Crimson Main-Camera Playback Renderer Plan

Date anchored: 2026-04-04.

Lifecycle: **deferred active performance plan**. This remains the current
weak-GPU renderer experiment; it is not required for functional playback on
accepted workstation-class systems.

## Why This Exists

The zoom/view-state telemetry clarified an important point on the Windows RTX
A1000 laptop:

- the user can be tightly zoomed in during playback
- the visible source-image fraction can drop dramatically
- but `gl_draw_ms` still stays essentially flat

In the representative zoomed-playback capture:

- camera viewport stayed about `1039 x 678`
- full-view samples had mean `gl_draw_ms` about `16.244 ms`
- tight-zoom samples had mean `gl_draw_ms` about `16.242 ms`
- playback still stayed below real time in both cases

So the next limiting factor is not "how much of the source frame is visible."
It is the cost of presenting the main camera image into the playback viewport.

## What The New Evidence Means

The earlier zoom-aware plan assumed this might be the next highest-yield path:

- zoomed out: cheaper whole-frame playback image
- zoomed in: full-resolution ROI only

That plan is still useful architectural context, but the latest capture showed
that **ROI alone is not likely to materially reduce draw cost**.

Why:

- the on-screen playback viewport stays the same size while zooming
- the GPU still shades roughly the same number of output pixels
- the current playback render path still pays for presenting the camera image
  through the same expensive final draw path

So the next optimization should target the playback camera renderer itself.

## Why We Are Doing This Change

This is now a targeted low-end GPU optimization, not a general correctness fix.

That matches observed behavior:

- the low-VRAM Windows laptop is still render-limited on
  `4512x4512 @ 60 fps` playback
- the user does not see this problem on a stronger desktop with an RTX A6000

The goal is therefore:

- keep full playback functionality
- keep zoom during playback
- keep overlays visible
- make the main camera presentation path cheaper on weaker GPUs

## What Must Be Preserved

- zoom during playback
- pan during playback
- overlay alignment
- paused full-fidelity inspection
- current stimulus improvements

This means the next change cannot simply remove interactivity or hide the main
camera overlays.

## Revised Direction

Use a cheaper playback-specific camera presentation path while preserving the
existing interaction semantics.

In practice:

- paused or editing mode keeps the current full ImPlot-based camera path
- playback mode uses a lighter image renderer for the main camera
- the lighter path still honors the current view transform so zoom/pan remain
  meaningful
- overlays remain visible, but should be drawn through the cheapest path that
  still preserves alignment

## Why This Is More Promising Than ROI Alone

The profiler now says:

- upload is negligible
- steady decode/write cost is negligible
- `gl_draw_ms` remains near the frame budget

And the zoomed-playback capture says:

- tight zoom did not reduce draw cost

So the bottleneck is now dominated by:

- the final playback presentation of the main camera image

That is why the next design target is the renderer, not just the visible ROI.

## Target Shape

### Playback Path

During playback:

- present the main camera image through a lighter render path than the current
  full ImPlot image path
- keep the same camera view bounds / transform semantics
- preserve bounding boxes and essential overlays

### Paused Path

When paused:

- keep the current high-fidelity interactive view
- keep the current edit-friendly behavior

This avoids regressing the workflows that matter most when the user is
inspecting a frame carefully.

## Candidate Implementation Shapes

### Option 1: Playback-Specific Camera Image Pass

- compute the camera view bounds exactly as today
- render the playback camera image through a lighter GL image pass
- composite it into the current viewport rectangle

This is the most direct path if the main cost is the current camera image draw.

### Option 2: Playback Image Plus Lightweight Overlay Layer

- use the lighter image pass for the camera image
- keep overlays, but draw them through a cheaper overlay path
- keep paused mode on the existing full path

This is likely the best tradeoff if the user must still see boxes during play.

### Option 3: Hybrid Path

- preserve the existing ImPlot path for paused / active-edit states
- switch to the cheaper playback renderer only while running normally

This is the safest rollout.

## Rollout Plan

### Phase 1: Documented Pivot

- record that ROI-only is no longer the highest-priority render experiment
- record the zoomed-playback evidence that motivated the pivot

### Phase 2: Playback Renderer Prototype

- add a playback-only cheaper camera presentation path
- preserve current paused behavior
- keep view-state telemetry in place

Acceptance:

- playback speed improves materially on the Windows laptop
- `gl_draw_ms` drops materially
- zoom/pan semantics remain usable during playback

### Phase 3: Overlay Triage

- keep essential overlays in playback
- simplify only the parts that prove expensive

Acceptance:

- overlays remain aligned
- user-visible playback behavior remains acceptable

### Phase 4: Revisit ROI-Aware Conversion

Once the cheaper playback renderer is in place:

- reconsider whether ROI-aware conversion is still worth adding
- use the existing view-state telemetry to guide that decision

## Success Criteria

- playback on the Windows laptop moves materially closer to `1.0x`
- `gl_draw_ms` drops below the current near-budget plateau
- zoom remains usable during playback
- bounding boxes remain visible and aligned
- paused inspection remains full fidelity
