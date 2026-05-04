# Crimson Analysis Timeline Performance Options

This note documents performance options for the Analysis Timeline after the
May 2026 profiling pass on the feeding canary. The goal is to keep ImPlot
interactivity while reducing per-frame CPU work and submitted plot geometry.

## Current Observation

The latest perf logs point to Analysis Timeline as a major steady frame cost:

- `movement_timeline_ms` is often around 4 ms per frame.
- At a 120 FPS render cap, the full frame budget is about 8.33 ms.
- Occasional timeline spikes around 12-13 ms are enough to cause visible frame
  pacing inconsistency.
- Camera upload, playback texture prewarm, and mask overlay work were not the
  dominant costs in the sampled run.

The current shape is roughly:

1. Palette/Zarr arrays are loaded into memory.
2. Each UI frame prepares plot-friendly vectors for the currently visible
   timeline sections.
3. Visible timeline rows are submitted to ImPlot.
4. Even when the x-axis is a small scrolling window, full traces are usually
   submitted and clipped by ImPlot.

This keeps implementation simple, but the cost scales with total trace length
and number of visible rows, not with the number of pixels that can actually be
drawn.

## Data Layout

Game/rendering-style data layout thinking is useful here.

An array-of-structs layout is convenient for per-sample logic:

```cpp
struct MotionSample {
    float time_s;
    float speed_mm_s;
    float heading_deg;
    float x_mm;
    float y_mm;
};
std::vector<MotionSample> samples;
```

Timeline plotting usually wants a struct-of-arrays layout:

```cpp
std::vector<float> time_s;
std::vector<float> speed_mm_s;
std::vector<float> heading_deg;
std::vector<float> x_mm;
std::vector<float> y_mm;
```

SoA is better for traces because each plot walks one x array and one y array
linearly. Crimson already stores most source arrays this way. The current
performance issue is less "wrong layout" and more "too much transient plot
buffer construction and too many submitted points per frame."

## Option 1: Cache Prepared Plot Buffers

Build plot-ready buffers when the selected dataset or trace options change,
then reuse them every frame.

Examples of cache invalidation keys:

- selected track-kinematics run, track, speed level
- selected swim-bout candidate and detector trace toggle
- eye-angle run, representation, left/right/vergence toggles
- tail-kinematics run and selected tail trace toggles
- x/y position trace toggles

Benefits:

- Preserves normal ImPlot interactivity.
- Avoids repeated `float -> double` conversion and vector allocation.
- Makes later visible-range slicing and LOD easier.

Tradeoffs:

- Needs cache ownership and invalidation discipline.
- Uses additional memory for prepared plot buffers.

This should be the first optimization.

## Option 2: Submit Only the Visible X Range

When the timeline has a known x-axis range, find the visible slice with
`std::lower_bound` on the sorted x array and submit only that slice to ImPlot.

For example:

```cpp
auto first = std::lower_bound(xs.begin(), xs.end(), x_min);
auto last = std::upper_bound(xs.begin(), xs.end(), x_max);
```

Then pass `first..last` to `ImPlot::PlotLine`.

Benefits:

- Preserves ImPlot pan, zoom, legends, and axis linking.
- Cost scales with visible time window rather than total recording duration.
- Especially useful for scrolling playback.

Tradeoffs:

- Need a small pad around the visible range so line segments entering/leaving
  the view do not visually break at the plot edge.
- If the user zooms all the way out, this still submits all samples.

This should be the second optimization.

## Option 3: Pixel-Level LOD

LOD means level of detail. For plots, it means drawing less detail when the
screen cannot show every sample anyway.

If a plot is 1,200 pixels wide and 20,000 samples fall within the visible
window, many samples map to the same x pixel. Instead of drawing all of them,
we can aggregate per pixel column:

- draw one representative point,
- draw min/max per pixel column,
- or preserve extrema plus endpoints for each pixel bucket.

Benefits:

- Cost scales with screen width rather than sample count.
- Preserves peaks better than naive "every Nth sample" decimation.
- Useful when zoomed out or when future recordings have much denser traces.

Tradeoffs:

- Hover/tooltips may need to query original data separately.
- Needs careful handling for lines with gaps or non-finite values.
- More code and more validation than visible-range slicing.

This should come after cache + visible slicing unless profiling shows a trace
still submits too many points inside the visible range.

## Option 4: Cache Ranges And Derived Annotations

Avoid recomputing values that only change when data or toggles change:

- full-trace y min/max
- visible-window y min/max if needed
- speed maxima
- heading axis ranges
- bout start/end/core times
- detector trace time conversion

Benefits:

- Simple and low risk.
- Helps remove repeated full-array scans during draw.

Tradeoffs:

- Smaller win than visible slicing if draw submission dominates.
- Needs invalidation tied to the same cache keys as plot buffers.

This can be bundled with Option 1.

## Option 5: Playback Static-Background Cache

During active playback, the timeline plot geometry often does not change except
for the current-time marker. A more aggressive optimization is to render the
static timeline background to a texture and update only the marker each frame.

Benefits:

- Best possible playback frame cost.
- Particularly useful for dense, zoomed-out timelines.

Tradeoffs:

- More complex.
- Less natural while panning, zooming, toggling traces, or inspecting hover
  values.
- Requires invalidation whenever plot state changes.

This is a later optimization, not the first step.

## Recommended Order

1. Add a cache layer for prepared plot buffers and derived ranges.
2. Submit only visible x-range slices when a scrolling or linked x range is
   active.
3. Precompute swim-bout window times and detector trace times.
4. Add targeted instrumentation for point counts submitted per row and cache
   hit/miss status.
5. Add pixel-level LOD for traces that still submit far more points than the
   plot width.
6. Consider static-background caching only if playback remains expensive after
   the above.

## Validation

For each optimization, compare before/after logs from the same Zarr and UI
state:

- `frame_perf.ui.movement_timeline_ms`
- total ImGui vertex/index counts
- per-row submitted point counts, once instrumented
- visible interactivity: pan, zoom, hover, legend toggles, x-axis linking
- playback frame pacing at `--frame-cap-fps 120`

The target is not just lower average time. The important user-facing metric is
lower p95/p99 timeline cost during active playback and paused inspection.
