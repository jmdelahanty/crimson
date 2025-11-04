# Distance Timeline Implementation - Complete Index

This index provides quick access to all information about the distance-to-chaser plotting feature.

## Quick Links to Source Code

### Main Visualization Window
- **File**: `/src/red.cpp`
- **Lines**: 3347-3472
- **Component**: ImGui "Distance Timeline" window
- **What it does**: Renders the main plot showing distance over time

### Dataset Selection UI
- **File**: `/src/red.cpp`
- **Lines**: 3113-3190
- **Component**: `renderMovementDatasetUI` lambda function
- **What it does**: Creates combo box for selecting between movement datasets

### Distance Calculation
- **File**: `/src/red.cpp`
- **Lines**: 1440-1472 - `selectBoundingBoxForTarget`
- **Lines**: 1475-1519 - `projectStimulusToCamera`
- **What it does**: Calculates Euclidean distances and coordinate transformations

### Data Structures
- **File**: `/src/zarr_loader.h`
- **Lines**: 175-191 - `MovementSeries` struct (contains distance_to_target_mm)
- **Lines**: 213-232 - `ChaserStateRecord` struct (contains distance_to_target_px)

### Data Loading
- **File**: `/src/zarr_loader.cpp`
- **Functions**: `loadMovementData()`, `loadMovementTrack()`, `loadLegacyMovementData()`
- **What it does**: Loads distance measurements from Zarr files

### Accessor Methods
- **File**: `/src/zarr_loader.h`
- **Lines**: 359-402
- **Key Method**: `getMovementDistanceToTargetMm()`
- **What it does**: Provides access to distance data for UI rendering

---

## Generated Documentation

### DISTANCE_TIMELINE_IMPLEMENTATION.md (11 KB)
Comprehensive technical documentation covering:
- Overview and architecture
- Data structures with field explanations
- Function descriptions and purpose
- Distance data flow (Zarr -> Loader -> UI)
- Pixel-to-MM conversion logic
- Current frame synchronization algorithm
- Recent changes and related commits
- Accessor method signatures
- Visualization features
- Error handling strategies

**Start here for**: Understanding the overall system design and data flow

### DISTANCE_TIMELINE_CODE_REFERENCE.md (17 KB)
Complete code implementation with:
- Full code snippets from all key functions
- Line-by-line explanations with comments
- Data structure definitions with annotations
- Accessor method implementations
- Usage examples
- Algorithm explanations

**Start here for**: Examining actual code implementation and making modifications

---

## What the Distance Timeline Does

The Distance Timeline is a visualization that:

1. **Loads movement data** from Zarr files containing chaser/target tracking
2. **Plots distance over time** using ImPlot (green line on white background)
3. **Shows current position** with a yellow vertical line that follows playback
4. **Displays statistics**: Average distance, maximum distance, minimum distance
5. **Supports multiple datasets** via dropdown selection
6. **Validates data** by filtering NaN and infinite values
7. **Synchronizes with playback** using frame-to-time mapping with interpolation

---

## Data Flow

```
User loads Zarr file
        |
        v
ZarrDetectionLoader::loadMovementData()
        |
        +---> loadSpeedRunMovement() OR loadLegacyMovementData()
        |
        v
loadMovementTrack() for each track
        |
        +---> Reads distance_to_target_mm (or distance_to_target_px)
        +---> Converts pixels to mm if needed
        +---> Maps camera frames to time values
        +---> Filters offline samples
        |
        v
Populates MovementSeries.distance_to_target_mm
        |
        v
red.cpp main render loop
        |
        +---> Distance Timeline window (lines 3347-3472)
        |
        +---> Retrieves selected series
        +---> Gets distance data via getMovementDistanceToTargetMm()
        +---> Filters NaN/Inf values
        +---> Computes statistics
        +---> Renders plot with ImPlot
        |
        v
User sees interactive plot with current frame indicator
```

---

## Key Algorithms

### Current Frame Synchronization
Located in Distance Timeline window (red.cpp, lines 3403-3448):

```
If current_frame_num exists in frame_indices:
    Use exact time from time_data at that index
Else:
    Find neighboring frames using lower_bound
    Interpolate time linearly between them
If no match and video_fps available:
    Estimate time from FPS and clamp to data range
```

### Distance Calculation
Located in `selectBoundingBoxForTarget` (red.cpp, lines 1440-1472):

```
For each bounding box:
    If bbox passes filter predicate:
        dx = cam_x - box.centroid_x
        dy = cam_y - box.centroid_y
        dist_sq = dx^2 + dy^2
        If dist_sq < best_distance:
            Update best_index and best_distance
Return best_index

Filter priority:
1. Target bounding box (is_target == true)
2. Unknown fish (fish_id < 0)
3. Any bounding box
```

### Pixel-to-MM Conversion
Located in `loadMovementTrack` (zarr_loader.cpp):

```
If distance_to_target_mm not available:
    Try to load distance_to_target_px
    If loaded and pixels_per_mm available:
        For each distance_px value:
            distance_mm = distance_px / pixels_per_mm
```

---

## Recent Development History

### Latest Commit (a37ad69)
**"distance to chaser plot working kind of"**
- Date: Mon Nov 3 21:03:05 2025
- Added Distance Timeline window (main feature)
- Added movement dataset selection UI
- Added distance data loading
- Added statistics calculation
- 168 lines in red.cpp, 128 lines in zarr_loader.cpp, 14 lines in zarr_loader.h

### Related Commits (chronological order)
- `baef725` - Plotting chaser and target plus bounding box from online detections
- `97bde0e` - Timeline back and functional
- `aa4a587` - Timeline failing again
- `76d2903` - Refactored to remove h5
- `47a5c75` - Trying to add timeline support
- `a010b88` - Debugging multiple chaser bbox 0 labels
- `25b3f20` - Interpolation and dataset selection for plotting
- `cf3b692` - Full data provenance now plots purely from zarr
- `0e6244c` - Cleaned up loading from zarr kind of

---

## File Organization

```
crimson/
├── src/
│   ├── red.cpp                          [3633 lines] - Main UI and visualization
│   ├── zarr_loader.cpp                  [6372 lines] - Data loading
│   ├── zarr_loader.h                    [623 lines]  - Data structures & API
│   └── inspect_data_structures.py       [254 lines] - Debug utility
├── DISTANCE_TIMELINE_INDEX.md           [This file]
├── DISTANCE_TIMELINE_IMPLEMENTATION.md  [Architecture & Design]
└── DISTANCE_TIMELINE_CODE_REFERENCE.md  [Full Code Snippets]
```

---

## Configuration & Customization

### Plot Appearance
To modify plot colors, line widths, etc., edit in red.cpp around line 3397:
```cpp
ImPlot::SetNextLineStyle(ImVec4(0.3f, 0.85f, 0.4f, 1.0f), 2.0f);  // Green, width 2.0
```

### Plot Size
Edit around line 3388:
```cpp
ImVec2 plot_size = ImVec2(-1, 300);  // -1 = full width, 300 = height
```

### Current Frame Indicator
Edit around line 3454:
```cpp
ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), 3.0f);  // Yellow, width 3.0
```

### Y-Axis Scaling
Edit around line 3392:
```cpp
double y_max = (max_distance > 0.0) ? max_distance * 1.1 : 1.0;  // 10% margin above max
```

---

## Error Handling & Robustness

### Data Validation Checks
- `std::isfinite()` - Filter NaN and infinity values
- Bounds checking - Verify array indices before access
- Null pointer checks - Validate returned series pointers
- Size synchronization - Ensure time and distance arrays match

### User-Facing Messages
- "No distance data available." - No selected series or empty data
- "No valid distance samples available." - All values are NaN or Inf
- Shows "Valid points: X / Y" - Indicates how many samples were used

### Graceful Fallbacks
1. If exact frame match not found, interpolate between neighbors
2. If interpolation not possible, estimate time from FPS
3. If FPS not available, clamp to available time range
4. If distance_to_target_mm missing, try loading distance_to_target_px and convert

---

## Dependencies

### Libraries Used
- **ImGui** - UI framework
- **ImPlot** - Plotting library (built on ImGui)
- **TensorStore** - Zarr file access
- **OpenCV** - Coordinate transformations (cv::perspectiveTransform)

### Data Dependencies
- Zarr file with structure: `analysis/movement_runs/[run_name]/`
- Required arrays: `time_seconds`, `distance_to_target_mm` (or `distance_to_target_px`)
- Optional arrays: `camera_frame_ids`, `frame_indices`, `has_offline`

---

## Testing & Debugging

### Python Utility for Data Inspection
File: `src/inspect_data_structures.py`
- Lists all arrays in Zarr file
- Shows first element of each array
- Helps verify data structure matches code expectations

### Common Issues & Solutions

| Issue | Likely Cause | Solution |
|-------|--------------|----------|
| "No distance data available" | Missing distance_to_target arrays | Check Zarr file structure with inspect_data_structures.py |
| All points invalid | NaN/Inf in all samples | Verify zarr_loader pixel_per_mm conversion |
| Yellow line not visible | Frame indices don't match movement data | Check frame alignment in loadMovementTrack |
| Plot empty | size_t sample_count = 0 | Ensure time and distance arrays are populated |
| Statistics show NaN | All samples filtered out | Check data quality in Zarr file |

---

## Future Enhancement Ideas

1. **Clickable events** - Click on plot to jump to that frame
2. **Event markers** - Show behavior events overlaid on plot
3. **Multiple series** - Plot multiple animals/runs simultaneously
4. **Filtering options** - Hide offline/invalid samples
5. **Export data** - Save plot as image or distance data as CSV
6. **Annotations** - Add user notes at specific times
7. **Statistics panel** - Show percentiles, histograms
8. **Smoothing slider** - Adjust distance smoothing on-the-fly

---

## Summary

The Distance Timeline is a comprehensive implementation for visualizing chaser-target distance over time. It:

- Integrates tightly with the Zarr-based data loading system
- Provides robust data validation and error handling
- Offers an interactive plot synchronized with video playback
- Supports multiple movement datasets with dataset selection UI
- Includes comprehensive statistics and current position tracking
- Is implemented with clean, well-commented code

For detailed information on any component, refer to the dedicated documentation files listed at the top of this index.

