# Distance to Chaser Plot - Code Structure and Implementation

## Overview
The distance timeline feature plots the distance between chaser and target fish over time. This was implemented in commit `a37ad69` with the message "distance to chaser plot working kind of".

## Files Involved

### 1. `/home/delahantyj@hhmi.org/gitrepos/crimson/src/red.cpp` (3633 lines)
Main GUI implementation for the visualization

### 2. `/home/delahantyj@hhmi.org/gitrepos/crimson/src/zarr_loader.cpp` (6372 lines)
Data loading and management from Zarr files

### 3. `/home/delahantyj@hhmi.org/gitrepos/crimson/src/zarr_loader.h` (623 lines)
Header file with data structures and API declarations

---

## Data Structures

### MovementSeries (zarr_loader.h, lines 175-191)
Contains time-series data for distance measurements:

```cpp
struct MovementSeries {
    std::string category;
    std::string run_name;
    std::string track_id;
    std::string detection_variant;
    std::string source_detect_run;
    double fps = 0.0;
    double smoothing_seconds = 0.0;
    int video_width = 0;
    int video_height = 0;
    bool from_speed_runs = false;
    std::vector<float> time_seconds;
    std::vector<float> smoothed_speed_mm;
    std::vector<float> instant_speed_mm;
    std::vector<float> distance_to_target_mm;  // KEY: Distance data
    std::vector<int32_t> frame_indices;
};
```

### ChaserStateRecord (zarr_loader.h, lines 213-232)
Records stimulus state with distance information:

```cpp
struct ChaserStateRecord {
    int32_t stimulus_frame_num = -1;
    int32_t camera_frame_id = -1;
    int32_t chaser_index = -1;
    float chaser_pos_x;
    float chaser_pos_y;
    float target_pos_x;
    float target_pos_y;
    float chaser_radius_px;
    float distance_to_target_px;  // Distance in pixels
    float target_speed_px_per_s;
    // ... other fields
};
```

---

## Key Functions

### 1. Distance Timeline Window (red.cpp, lines 3347-3472)

**Location**: ImGui window drawing code in main render loop

**Purpose**: Renders the "Distance Timeline" ImGui window with plots and statistics

**Key Components**:

#### Data Retrieval
```cpp
const auto* selected_series = renderMovementDatasetUI("Dataset##distance");
const auto& time_data = zarr_loader.getMovementTimeSeconds();
const auto& distance_mm = zarr_loader.getMovementDistanceToTargetMm();
const auto& frame_indices = zarr_loader.getMovementFrameIndices();
```

#### Data Validation and Processing
- Filters out non-finite distance values
- Maintains sample count = min(time_data.size(), distance_mm.size())
- Computes statistics: sum, max, min, average distance

#### Plotting Logic
```cpp
ImPlot::BeginPlot("##distance_plot", plot_size)
ImPlot::SetupAxes("Time (s)", "Distance (mm)");
ImPlot::SetupAxisLimits(ImAxis_X1, time_plot.front(), time_plot.back(), ImGuiCond_Once);
ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, y_max, ImGuiCond_Once);
ImPlot::SetNextLineStyle(ImVec4(0.3f, 0.85f, 0.4f, 1.0f), 2.0f);  // Green color
ImPlot::PlotLine("Distance to Target", time_plot.data(), distance_plot.data(), 
                 static_cast<int>(time_plot.size()));
```

#### Current Frame Indicator
- Shows vertical yellow line at current playback position
- Uses linear interpolation for frames between data points
- Color: ImVec4(1.0f, 0.8f, 0.2f, 1.0f) - yellow

#### Statistics Display
- Average Distance: calculated from valid samples
- Max Distance: maximum value in dataset
- Min Distance: minimum value (if finite)

---

### 2. renderMovementDatasetUI Lambda (red.cpp, lines 3113-3190)

**Purpose**: Creates UI for selecting movement datasets

**Functionality**:
- Builds combo box when multiple movement series available
- Displays metadata: category, run name, track ID
- Shows variant and source detection run
- Displays FPS, smoothing parameters, and camera dimensions
- Returns selected MovementSeries pointer

---

### 3. Distance Calculation Functions in Stimulus Overlay

#### selectBoundingBoxForTarget (red.cpp, lines 1440-1472)
Finds nearest bounding box to target position using Euclidean distance:

```cpp
auto selectBoundingBoxForTarget = [&](double cam_x, double cam_y) -> size_t {
    double best_distance = std::numeric_limits<double>::infinity();
    for (size_t idx = 0; idx < chaser_bboxes.size(); ++idx) {
        const auto& box = chaser_bboxes[idx];
        double dx = cam_x - static_cast<double>(box.centroid_x);
        double dy = cam_y - static_cast<double>(box.centroid_y);
        double dist_sq = dx * dx + dy * dy;  // Squared distance
        if (dist_sq < best_distance) {
            best_distance = dist_sq;
            best_index = idx;
        }
    }
};
```

Priority order:
1. Target bounding box
2. Fish with ID < 0 (unknown)
3. Any bounding box

#### projectStimulusToCamera (red.cpp, lines 1475-1519)
Converts stimulus coordinates to camera space:

**Three projection methods**:
1. **Direct camera coords** (if has_camera_coords is true)
2. **Homography-based** (if valid_homography exists)
3. **Default projector mapping** (using 358.0 extent)

---

### 4. Zarr Loading Functions

#### loadMovementData (zarr_loader.cpp)
- Loads movement analysis from `analysis/movement_runs`
- Delegates to loadSpeedRunMovement or loadLegacyMovementData

#### loadLegacyMovementData (zarr_loader.cpp)
- Reads distance data from movement runs
- Attempts distance_to_target_mm first
- Falls back to distance_to_target_px with pixel-to-mm conversion

#### loadMovementTrack (zarr_loader.cpp)
- Loads individual movement tracks with distance series
- Parameters include:
  - `run_distance_to_target_mm`: distance measurements
  - `run_camera_frame_ids`: frame mapping
  - `run_distance_ptr`: populated with distance data

---

## Distance Data Flow

```
Zarr File
  └─ analysis/
     └─ movement_runs/
        └─ [run_name]/
           ├─ distance_to_target_mm  (preferred)
           ├─ distance_to_target_px  (fallback, with conversion)
           ├─ camera_frame_ids
           ├─ time_seconds
           ├─ smoothed_speed_mm
           └─ frame_indices

        ↓
        
ZarrDetectionLoader::loadMovementTrack()
  └─ Converts pixel distances to mm (if needed)
  └─ Maps camera frames to movement data indices
  └─ Filters offline samples (has_offline flags)
  └─ Populates MovementSeries.distance_to_target_mm

        ↓
        
red.cpp - Distance Timeline Window
  └─ Retrieves selected series
  └─ Validates and filters data
  └─ Plots time vs distance
  └─ Shows statistics
  └─ Updates with current frame position
```

---

## Pixel-to-MM Conversion

In `loadMovementTrack` (zarr_loader.cpp):

```cpp
if (pixels_per_mm > 1e-6f) {
    run_distance_to_target_mm.resize(run_distance_to_target_px.size());
    for (size_t i = 0; i < run_distance_to_target_px.size(); ++i) {
        run_distance_to_target_mm[i] = run_distance_to_target_px[i] / pixels_per_mm;
    }
}
```

This converts from pixel-space distances to millimeters for display.

---

## Current Frame Synchronization

The timeline shows a vertical indicator at the current playback frame:

**Algorithm**:
1. If current_frame_num exists in frame_indices: use exact time
2. Otherwise: use binary search (lower_bound) and linear interpolation
3. Fallback: estimate from FPS if no exact match
4. Clamp to available time range

```cpp
if (std::isfinite(current_time)) {
    double current_line_x[2] = {current_time, current_time};
    double current_line_y[2] = {limits.Y.Min, limits.Y.Max};
    ImPlot::PlotLine("##current_time", current_line_x, current_line_y, 2);
}
```

---

## Recent Changes (Commit a37ad69)

### Files Modified
- `imgui.ini` - Window state
- `src/red.cpp` - Added Distance Timeline window (168 lines added)
- `src/zarr_loader.cpp` - Enhanced movement data loading (128 lines added)
- `src/zarr_loader.h` - Added distance accessors (14 lines added)

### Key Additions
1. Distance Timeline ImGui window
2. Movement dataset selection UI (renderMovementDatasetUI)
3. Distance data loading from Zarr
4. Statistics calculation (avg, max, min)
5. Current frame position tracking

### Related Commits
- `0e6244c` - Cleaned up loading from zarr
- `cf3b692` - Full data provenance now plots purely from zarr
- `25b3f20` - Interpolation and dataset selection for plotting
- `a010b88` - Debugging multiple chaser bbox 0 labels
- `baef725` - Plotting chaser and target plus bounding box
- `97bde0e` - Timeline back and functional

---

## Accessor Methods (zarr_loader.h)

### Movement Data Access
```cpp
const std::vector<float>& getMovementTimeSeconds()
const std::vector<float>& getMovementSmoothedSpeedMm()
const std::vector<float>& getMovementInstantaneousSpeedMm()
const std::vector<float>& getMovementDistanceToTargetMm()  // DISTANCE DATA
const std::vector<int32_t>& getMovementFrameIndices()
const std::string& getMovementRunName()
const std::string& getMovementTrackId()
const std::string& getMovementCategory()
```

### Movement Series Management
```cpp
size_t getMovementSeriesCount() const;
const ZarrDetectionData::MovementSeries* getMovementSeries(size_t index) const;
size_t getSelectedMovementSeriesIndex() const;
const ZarrDetectionData::MovementSeries* getSelectedMovementSeries() const;
bool selectMovementSeries(size_t index);
void finalizeMovementSelection();
```

---

## Visualization Features

### Plot Configuration
- **X-axis**: Time in seconds
- **Y-axis**: Distance in millimeters
- **Line color**: Green (0.3f, 0.85f, 0.4f, 1.0f)
- **Line width**: 2.0f
- **Current frame indicator**: Yellow vertical line, width 3.0f
- **Plot size**: Full width, 300 pixels height

### Interactive Elements
- Window can be resized and moved (standard ImGui)
- Axes auto-scale on first load (ImGuiCond_Once)
- Y-axis: 0.0 to (max_distance * 1.1)
- Dataset selector: combo box when multiple datasets available
- Metadata display: variant, source run, FPS, camera dimensions

### Data Validation
- Filters NaN and infinite values
- Validates frame index bounds
- Checks sample count synchronization
- Clamps interpolated time to data range

---

## Error Handling

### Data Availability Checks
```cpp
if (!selected_series || sample_count == 0) {
    ImGui::TextUnformatted("No distance data available.");
} else if (valid_count == 0) {
    ImGui::TextUnformatted("No valid distance samples available.");
}
```

### Robustness Measures
- Infinity/NaN filtering with std::isfinite()
- Bounds checking on array accesses
- Safe type casting with static_cast
- Division by zero prevention
- Null pointer checks on retrieved data

