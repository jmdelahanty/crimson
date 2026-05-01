# Crimson Eye-Angle Exploratory Plotting Checklist

Date anchored: 2026-05-01.

## Purpose

Track the remaining work needed to turn Crimson's initial Palette eye-angle
reader into a full exploratory plotting UI.

The current implementation can load `analysis/eye_angle_runs`, read the
`eye_angle_variant_schema`, switch representations, plot default scalar traces,
show current-row values, draw gaze rays when present, and seek by basic QA
filters. This checklist covers the next layer of interaction, correctness, and
performance.

Related contracts and docs:

- `/home/delahantyj@hhmi.org/gitrepos/contracts/palette-crimson/eye_angle_read.md`
- `/home/delahantyj@hhmi.org/gitrepos/contracts/palette-crimson/zarr_alignment.md`
- `/home/delahantyj@hhmi.org/gitrepos/palette/docs/eye_angle_variants.md`
- `/home/delahantyj@hhmi.org/gitrepos/palette/src/fisheye/docs/eye_angle_conventions.md`

## Implementation Checklist

### 1. Plot Data Model

- [ ] Represent every loaded scalar field with name, display name, units,
      representation, role, ROI availability, frame availability, and source
      path.
- [ ] Represent vector fields separately from scalar fields.
- [ ] Distinguish ROI-indexed data from frame-indexed data in the plotting
      API.
- [ ] For ROI arrays, use `support/frame_indices` for frame x-values when
      available.
- [ ] For ROI arrays, use `support/time_seconds` for time x-values when the UI
      is in time mode.
- [ ] For frame arrays, use frame row index or `support/frame_time_seconds`;
      do not remap through ROI frame indices.
- [ ] Preserve the current smoothed-to-base fallback, but expose the fallback in
      the UI when it happens.

### 2. Field Browser

- [ ] Add a searchable field browser in the Eye Angles tab.
- [ ] Group fields by representation: `eye_frame`, `gaze`, `nasal_gaze`,
      `major`, `centroid`, `legacy`, and any future schema-provided
      representation.
- [ ] Show scalar and vector fields in separate groups.
- [ ] Mark unavailable schema fields as disabled rather than hiding them.
- [ ] Add selection presets: `Default`, `All in representation`, `Clear`, and
      `Valid plotted fields`.
- [ ] Keep `default_plot_fields` as the initial selection for each
      representation.
- [ ] Persist the active selection in UI state for the current session.

### 3. Scalar Plotting

- [ ] Plot multiple selected scalar traces at once.
- [ ] Group selected traces by compatible units so degrees, pixels, seconds,
      and unitless values do not share misleading axes.
- [ ] Add an x-axis mode selector: frame, time, or row.
- [ ] Draw the current frame/row cursor on each plot.
- [ ] Add hover readout for field name, x-value, y-value, row, and frame.
- [ ] Add click-to-seek from a plotted point back to the video frame.
- [ ] Add selected-row highlighting across all visible plots.
- [ ] Keep legends readable when many fields are selected.

### 4. QA And Reason Overlays

- [ ] Decode and display `qa/roi/reason_codes` and `qa/frame/reason_codes`
      consistently through the reason-code map.
- [ ] Show invalid samples directly in plots as markers or muted segments.
- [ ] Show major-axis marginal samples as a separate marker style.
- [ ] Add a plot option to hide invalid samples.
- [ ] Add a plot option to show invalid samples without connecting through
      them.
- [ ] Connect the reason substring filter to plotted sample highlighting.
- [ ] Keep existing prev/next QC seek controls, but make their target criteria
      visible in the plots.

### 5. Vector Fields And Spatial Links

- [ ] For vector ROI fields, expose derived scalar views: x, y, magnitude, and
      direction angle where meaningful.
- [ ] Keep spatial overlays separate from time-series plots.
- [ ] Let gaze-ray overlays follow the selected vector field when the selected
      representation provides one.
- [ ] Disable spatial overlays gracefully when the selected vector field is
      missing.
- [ ] Show which representation and source field are currently driving the
      camera overlay.

### 6. Representation-Aware Overlays

- [ ] Separate camera overlay settings from plot trace selection.
- [ ] Allow arcs/labels to choose their angle source explicitly instead of
      relying on hardcoded gaze/minor fields.
- [ ] Label angle overlays with the selected source representation and field.
- [ ] Keep the current eye-frame default as the conservative starting point.
- [ ] Avoid presenting a derived overlay as a different representation than its
      source field.

### 7. Performance

- [ ] Cache plot x/y buffers per field and x-axis mode.
- [ ] Rebuild plot buffers only when the field selection, x-axis mode, loaded
      run, or data source changes.
- [ ] Avoid per-frame vector allocation in ImGui/ImPlot draw code.
- [ ] Add sample-every-N decimation for dense traces.
- [ ] Consider automatic decimation based on visible pixel width.
- [ ] Keep exact data available for hover and seek even when the rendered line is
      decimated.
- [ ] Add timing instrumentation for eye-angle plot buffer rebuild and draw
      cost.

### 8. Validation

- [ ] Validate against the canary archive:
      `/nvme1/recordings/2026-01-28T23-15-10Z_arena_2_Feeding/zarr/2026-01-28T23-15-10Z_arena_2_Feeding_analysis.zarr`.
- [ ] Confirm latest resolves to
      `eye_angle_variant_schema_v7_canary_20260501`.
- [ ] Confirm representation order is `eye_frame`, `gaze`, `nasal_gaze`,
      `major`, `centroid`, `legacy`.
- [ ] Confirm the default representation is `eye_frame`.
- [ ] Confirm default plots appear for every representation with available
      fields.
- [ ] Confirm unavailable fields are disabled clearly.
- [ ] Confirm click-to-seek lands on the expected video frame.
- [ ] Confirm invalid and major-axis marginal rows can be found visually and via
      QC seek controls.
- [ ] Run `git diff --check`.
- [ ] Build `redgui`.

## Suggested First Slice

Start with:

1. Correct frame-vs-ROI x-axis handling.
2. Add a schema-driven field browser with checkboxes.
3. Cache plot buffers for selected scalar fields.

That gives Crimson a sound plotting foundation before adding richer vector
views, overlay source selection, and decimation.
