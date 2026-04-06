# Crimson Main-Camera Playback Renderer Plan

Date anchored: 2026-04-04.

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

## Additional April 2026 Findings

The later Windows laptop captures added two important clarifications:

- a runtime `VSync` toggle proved that some of the old `gl_draw_ms` cost was
  really driver/compositor pacing charged to the render call instead of to
  `SwapBuffers`
- but turning `VSync` off did **not** reach stable `1.0x` playback, so VSync
  was only part of the story

The newer decoder instrumentation then exposed the missing decoder-side cost:

- `camera_decode_demux_ms` stayed small
- `camera_decode_submit_ms` was consistently near or above the `16.67 ms`
  frame budget on the Windows RTX A1000 laptop

This means the laptop is not only render-limited. It is also paying a
meaningful per-frame hardware decode submit cost on the same weak GPU.

### Why Buffer Size Matters More Than Expected

The main-camera `GPU Buffer` size now looks like a real throughput/VRAM tradeoff
on this machine:

- a very large `GPU Buffer` ring (`100`) produced much better playback headroom
  than expected, but at the cost of a very large GPU allocation
- a small `GPU Buffer` ring (`8`) made playback substantially worse and exposed
  larger decode gaps and occasional decoder wait spikes

Interpretation:

- the large ring appears to give the decoder enough runway to hide some decode
  and render jitter
- the small ring does not leave enough headroom for this laptop's combined
  decode/render path

So the current evidence no longer points to "just make the renderer cheaper."
It points to a mixed bottleneck:

- final playback presentation is still expensive
- hardware decode submit is also expensive
- the two are likely competing for the same limited GPU capacity

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

As of the latest captures, that renderer work is still the right primary
follow-up.

A main-camera software-decode experiment was run after these notes were first
written, and it failed decisively on the Windows laptop:

- software main-camera decode collapsed to about `0.30x` playback speed
- the same workload on the `GPU Decode` path stayed near `0.89x`
- decoder-side submit/convert/write costs all became much worse on the software
  path

So the current guidance is:

- keep the main camera on `GPU Decode`
- continue treating the playback renderer as the main optimization frontier
- do not spend more time on main-camera software decode unless the decode stack
  changes substantially

As of the latest Windows laptop findings, the practical product conclusion is
more conservative than this document originally assumed:

- on the validated Windows RTX A1000 laptop, full-fidelity
  `4512x4512 @ 60 fps HEVC` playback is likely to remain somewhat slower than
  real time on the current path
- reducing playback fidelity may still help, but that is now a deliberate
  product tradeoff, not a near-term renderer fix
- for current guidance, prefer documenting the hardware ceiling clearly instead
  of assuming a proxy-based playback path will be adopted immediately

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

### Phase 5: Main-Camera Software Decode Experiment

Status: completed and rejected on the Windows RTX A1000 laptop.

Observed result:

- the software path regressed to roughly `0.30x` playback
- `GPU Decode` remained much better at roughly `0.89x`
- the experiment did not reduce the real bottleneck enough to justify keeping
  it as a recommended path

Follow-up:

- retain the documented result for future reference
- do not prioritize more work on this experiment ahead of playback-render
  optimization

## Success Criteria

- playback on the Windows laptop moves materially closer to `1.0x`
- `gl_draw_ms` drops below the current near-budget plateau
- zoom remains usable during playback
- bounding boxes remain visible and aligned
- paused inspection remains full fidelity
