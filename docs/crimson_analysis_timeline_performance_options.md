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

## May 2026 Follow-Up

After adding prepared trace caching, visible-range submission, and pixel-level
LOD/envelope plotting, the feeding canary no longer shows Analysis Timeline as
the dominant steady-state cost.

The latest sampled run showed:

- frame cap: `120 FPS` (`8.333 ms` budget)
- frame loop after warmup: p50 about `8.40 ms`, p99 about `8.52 ms`
- Analysis Timeline total: p50 about `0.78 ms`, p99 about `1.24 ms`
- plot drawing: p50 about `0.53 ms`, p99 about `0.97 ms`
- submitted timeline points: p50 about `6.6k`, max about `25.7k`
- frame-cap sleep: p50 about `6.0 ms`

The remaining notable timeline issue is cold-cache setup on the first visible
frame. In the sampled run, frame `0` spent about `4.7 ms` in the Analysis
Timeline and about `37 ms` in total UI build while preparing the first motion,
position, eye-angle, and tail-kinematics trace rows. Later frames reuse those
caches and are much cheaper.

This is currently acceptable. Defer heavier startup/loading architecture unless
users report visible startup hangs or the first-frame spike becomes a practical
editing/playback problem.

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

## Deferred Option: Progressive Cold-Cache Warmup

If cold-cache UI setup becomes a problem, add a progressive warmup component
instead of blocking the first frame.

The desired behavior is:

1. Open the GUI and camera view as soon as the Zarr/video are minimally usable.
2. Show a small status such as `Preparing analysis timeline...`.
3. Build timeline caches over several frames:
   - motion prepared traces
   - track position row
   - eye-angle row
   - tail-kinematics rows
   - LOD/envelope levels
4. Draw placeholders, partial rows, or decimated fallback traces until the final
   cached row is ready.
5. Keep each warmup slice bounded so event polling and rendering continue.

Do not start with a modal loading screen. A modal is only justified if the app
cannot render meaningful video yet. The better UI is progressive readiness:
video first, analysis panels shortly after.

Background threads can help, but they should be introduced carefully. The safe
threading boundary is:

- background workers prepare immutable CPU data only
- ImGui, ImPlot, OpenGL, texture upload, and UI state mutation stay on the main
  thread
- each job carries a generation key, and the main thread discards completed
  work if the user changed source/toggles meanwhile
- use a small bounded worker pool; do not spawn one thread per trace

Parallel prep is not automatically faster because these jobs are mostly memory
traversal and allocation. The first future step should be progressive/asynchronous
cache publication on the main thread; add worker parallelism only if logs still
show cold-cache stalls.

## Recommended Order

1. Keep the existing prepared-buffer, visible-range, and LOD/envelope path.
2. Continue using perf JSONL to watch p95/p99 timeline cost and submitted point
   counts.
3. If steady-state plotting regresses, optimize specific rows/traces using the
   existing instrumentation before adding new architecture.
4. If first-frame cold-cache cost becomes user-visible, implement progressive
   warmup before adding background worker parallelism.
5. Consider static-background caching only if playback remains expensive after
   cache/LOD and warmup improvements.

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
