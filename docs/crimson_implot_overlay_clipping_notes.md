# Crimson ImPlot Overlay Clipping Notes

This note documents a subtle GUI issue seen in the Analysis Timeline after
custom overlays were added to linked ImPlot subplot rows.

## Symptom

Custom shaded overlays can extend into subplot margins instead of stopping at
the inner plot area. This is easiest to see when the x-axis is zoomed or
scrolling:

- stimulus step highlights may extend beyond the plotted lane
- swim-bout highlights may extend into the y-axis label/tick margin

The x-axis can still be logically correct; the bug is visual clipping.

## Cause

ImPlot clips normal plotted items, but custom `ImDrawList` primitives are not
automatically clipped to the plot area. If a long interval starts before the
visible x-limits or ends after them, `ImPlot::PlotToPixels(...)` can return
pixel coordinates outside the visible plot body. Drawing those rectangles
directly lets them spill into margins or adjacent UI.

## Required Pattern

For custom rectangle or line overlays inside an ImPlot plot:

1. Get the authoritative visible x range from `ImPlot::GetPlotLimits()`.
2. Clamp interval endpoints to `limits.X.Min` and `limits.X.Max` before calling
   `ImPlot::PlotToPixels(...)`.
3. Push a clip rect matching the inner plot area:

```cpp
const ImVec2 plot_pos = ImPlot::GetPlotPos();
const ImVec2 plot_size = ImPlot::GetPlotSize();
draw_list->PushClipRect(
    plot_pos,
    ImVec2(plot_pos.x + plot_size.x, plot_pos.y + plot_size.y),
    true);
// draw custom primitives here
draw_list->PopClipRect();
```

4. Use plot limits, not a separate scroll-window variable, when deciding if a
   custom shape is visible. Linked subplot axes and user pan/zoom can make the
   current plot limits differ from the controller’s initial requested range.

## Current Examples

Known fixed examples:

- `src/gui/analysis_timeline_stimulus_context.cpp`
  - stimulus step bands clamp `x0/x1` to the visible x-limits
  - step/event draw-list primitives are clipped to the plot area
- `src/gui/analysis_timeline_motion_plot.cpp`
  - swim-bout and core-bout rectangles clamp to `ImPlot::GetPlotLimits()`
  - speed-row custom rectangles are clipped to the plot area

When adding new timeline rows, prefer normal ImPlot plot calls where possible.
When direct draw-list primitives are needed, use this clipping pattern.
