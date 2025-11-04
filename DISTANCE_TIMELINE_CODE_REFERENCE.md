# Distance Timeline - Complete Code Reference

## Main UI Window Implementation

### File: `/src/red.cpp` (Lines 3347-3472)

This is the core visualization window that plots distance over time.

```cpp
if (ImGui::Begin("Distance Timeline")) {
    const auto* selected_series = renderMovementDatasetUI("Dataset##distance");

    const auto& time_data = zarr_loader.getMovementTimeSeconds();
    const auto& distance_mm = zarr_loader.getMovementDistanceToTargetMm();
    const auto& frame_indices = zarr_loader.getMovementFrameIndices();

    size_t sample_count = std::min(time_data.size(), distance_mm.size());
    if (!selected_series || sample_count == 0) {
        ImGui::TextUnformatted("No distance data available.");
    } else {
        std::vector<double> time_plot;
        std::vector<double> distance_plot;
        time_plot.reserve(sample_count);
        distance_plot.reserve(sample_count);

        double sum_distance = 0.0;
        double max_distance = 0.0;
        double min_distance = std::numeric_limits<double>::infinity();
        size_t valid_count = 0;

        // Filter valid samples
        for (size_t i = 0; i < sample_count; ++i) {
            float raw_distance = distance_mm[i];
            if (!std::isfinite(static_cast<double>(raw_distance))) {
                continue;
            }
            double t = static_cast<double>(time_data[i]);
            double value = static_cast<double>(raw_distance);
            time_plot.push_back(t);
            distance_plot.push_back(value);
            sum_distance += value;
            max_distance = std::max(max_distance, value);
            min_distance = std::min(min_distance, value);
            ++valid_count;
        }

        if (valid_count == 0) {
            ImGui::TextUnformatted("No valid distance samples available.");
        } else {
            ImGui::Text("Valid points: %zu / %zu", valid_count, sample_count);

            // Create plot
            ImVec2 plot_size = ImVec2(-1, 300);
            if (!time_plot.empty() && ImPlot::BeginPlot("##distance_plot", plot_size)) {
                ImPlot::SetupAxes("Time (s)", "Distance (mm)");
                ImPlot::SetupAxisLimits(ImAxis_X1, time_plot.front(), time_plot.back(), ImGuiCond_Once);
                double y_max = (max_distance > 0.0) ? max_distance * 1.1 : 1.0;
                if (y_max <= 0.0) {
                    y_max = 1.0;
                }
                ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, y_max, ImGuiCond_Once);
                
                // Plot green line
                ImPlot::SetNextLineStyle(ImVec4(0.3f, 0.85f, 0.4f, 1.0f), 2.0f);
                ImPlot::PlotLine("Distance to Target",
                                 time_plot.data(),
                                 distance_plot.data(),
                                 static_cast<int>(time_plot.size()));

                // Draw current frame indicator
                if (current_frame_num >= 0) {
                    double current_time = -1.0;

                    // Find current time by looking up current_frame_num in frame_indices
                    if (!frame_indices.empty()) {
                        auto it = std::find(frame_indices.begin(), frame_indices.end(), current_frame_num);
                        if (it != frame_indices.end()) {
                            size_t idx = std::distance(frame_indices.begin(), it);
                            if (idx < time_data.size()) {
                                current_time = static_cast<double>(time_data[idx]);
                            }
                        } else {
                            // Interpolate between neighboring frames
                            auto upper = std::lower_bound(frame_indices.begin(), frame_indices.end(), current_frame_num);
                            if (upper != frame_indices.end() && upper != frame_indices.begin()) {
                                auto lower = upper - 1;
                                size_t lower_idx = std::distance(frame_indices.begin(), lower);
                                size_t upper_idx = std::distance(frame_indices.begin(), upper);

                                if (upper_idx < time_data.size() && lower_idx < time_data.size()) {
                                    int32_t f0 = *lower;
                                    int32_t f1 = *upper;
                                    float t0 = time_data[lower_idx];
                                    float t1 = time_data[upper_idx];
                                    float delta_f = static_cast<float>(f1 - f0);
                                    if (delta_f != 0.0f) {
                                        float alpha = static_cast<float>(current_frame_num - f0) / delta_f;
                                        current_time = static_cast<double>(t0 + alpha * (t1 - t0));
                                    }
                                }
                            } else if (upper == frame_indices.begin() && !time_data.empty()) {
                                current_time = static_cast<double>(time_data.front());
                            } else if (upper == frame_indices.end() && !time_data.empty()) {
                                current_time = static_cast<double>(time_data.back());
                            }
                        }
                    }

                    // Fallback: estimate from FPS
                    if (current_time < 0.0 && video_fps > 0.0) {
                        double estimated_time = static_cast<double>(current_frame_num) / video_fps;
                        if (!time_data.empty()) {
                            double min_time = static_cast<double>(time_data.front());
                            double max_time = static_cast<double>(time_data.back());
                            current_time = std::clamp(estimated_time, min_time, max_time);
                        } else {
                            current_time = estimated_time;
                        }
                    }

                    // Draw yellow vertical line at current time
                    if (current_time >= 0.0) {
                        ImPlotRect limits = ImPlot::GetPlotLimits();
                        double current_line_x[2] = {current_time, current_time};
                        double current_line_y[2] = {limits.Y.Min, limits.Y.Max};
                        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), 3.0f);
                        ImPlot::PlotLine("##current_time", current_line_x, current_line_y, 2);
                    }
                }

                ImPlot::EndPlot();
            }

            // Display statistics
            ImGui::SeparatorText("Statistics");
            double avg_distance = sum_distance / static_cast<double>(valid_count);
            ImGui::BulletText("Average Distance: %.2f mm", avg_distance);
            ImGui::BulletText("Max Distance: %.2f mm", max_distance);
            if (std::isfinite(min_distance)) {
                ImGui::BulletText("Min Distance: %.2f mm", min_distance);
            }
        }
    }
}
ImGui::End();
```

---

## Dataset Selection UI

### File: `/src/red.cpp` (Lines 3113-3190)

Lambda function that creates a combo box for selecting between movement datasets.

```cpp
auto renderMovementDatasetUI = [&](const char* combo_label) -> const ZarrDetectionData::MovementSeries* {
    size_t series_count = zarr_loader.getMovementSeriesCount();
    size_t selected_index = zarr_loader.getSelectedMovementSeriesIndex();
    const auto* selected_series = zarr_loader.getMovementSeries(selected_index);

    // If multiple series, show combo box
    if (series_count > 1) {
        std::ostringstream summary;
        if (selected_series) {
            summary << selected_series->category << "/" << selected_series->run_name
                    << " (track " << selected_series->track_id << ")";
        } else {
            summary << "Select dataset";
        }
        if (ImGui::BeginCombo(combo_label, summary.str().c_str())) {
            for (size_t i = 0; i < series_count; ++i) {
                const auto* series = zarr_loader.getMovementSeries(i);
                if (!series) {
                    continue;
                }
                std::ostringstream label;
                label << series->category << "/" << series->run_name
                      << " (track " << series->track_id << ")";
                bool is_selected = (i == selected_index);
                if (ImGui::Selectable(label.str().c_str(), is_selected)) {
                    if (zarr_loader.selectMovementSeries(i)) {
                        selected_index = i;
                        selected_series = zarr_loader.getMovementSeries(i);
                    }
                }
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    } else if (selected_series) {
        ImGui::Text("Dataset: %s/%s (track %s)",
                    selected_series->category.c_str(),
                    selected_series->run_name.c_str(),
                    selected_series->track_id.c_str());
    }

    // Display metadata about selected series
    if (selected_series) {
        if (!selected_series->detection_variant.empty() ||
            !selected_series->source_detect_run.empty()) {
            if (!selected_series->source_detect_run.empty()) {
                ImGui::Text("Variant: %s | Source run: %s",
                            selected_series->detection_variant.empty()
                                ? "unknown"
                                : selected_series->detection_variant.c_str(),
                            selected_series->source_detect_run.c_str());
            } else {
                ImGui::Text("Variant: %s",
                            selected_series->detection_variant.empty()
                                ? "unknown"
                                : selected_series->detection_variant.c_str());
            }
        }
        if (selected_series->fps > 0.0 || selected_series->smoothing_seconds > 0.0) {
            if (selected_series->fps > 0.0 && selected_series->smoothing_seconds > 0.0) {
                ImGui::Text("FPS: %.2f | Smoothing: %.2f s",
                            selected_series->fps,
                            selected_series->smoothing_seconds);
            } else if (selected_series->fps > 0.0) {
                ImGui::Text("FPS: %.2f", selected_series->fps);
            } else {
                ImGui::Text("Smoothing: %.2f s", selected_series->smoothing_seconds);
            }
        }
        if (selected_series->video_width > 0 && selected_series->video_height > 0) {
            ImGui::Text("Camera size: %dx%d",
                        selected_series->video_width,
                        selected_series->video_height);
        }
    }

    return selected_series;
};
```

---

## Distance Calculation - selectBoundingBoxForTarget

### File: `/src/red.cpp` (Lines 1440-1472)

Finds the bounding box closest to the target using Euclidean distance.

```cpp
auto selectBoundingBoxForTarget = [&](double cam_x, double cam_y) -> size_t {
    auto choose = [&](auto predicate) -> size_t {
        double best_distance = std::numeric_limits<double>::infinity();
        size_t best_index = kInvalidBBoxIndex;
        for (size_t idx = 0; idx < chaser_bboxes.size(); ++idx) {
            const auto& box = chaser_bboxes[idx];
            if (!predicate(box)) {
                continue;
            }
            if (!std::isfinite(box.centroid_x) || !std::isfinite(box.centroid_y)) {
                continue;
            }
            // Calculate Euclidean distance
            double dx = cam_x - static_cast<double>(box.centroid_x);
            double dy = cam_y - static_cast<double>(box.centroid_y);
            double dist_sq = dx * dx + dy * dy;
            if (dist_sq < best_distance) {
                best_distance = dist_sq;
                best_index = idx;
            }
        }
        return best_index;
    };

    // Priority 1: Target bounding box
    size_t idx = choose([](const auto& box) { return box.is_target; });
    if (idx != kInvalidBBoxIndex) {
        return idx;
    }
    // Priority 2: Fish with ID < 0 (unknown)
    idx = choose([](const auto& box) { return box.fish_id < 0; });
    if (idx != kInvalidBBoxIndex) {
        return idx;
    }
    // Priority 3: Any bounding box
    return choose([](const auto&) { return true; });
};
```

---

## Data Structures

### File: `/src/zarr_loader.h` (Lines 175-191)

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
    std::vector<float> distance_to_target_mm;  // DISTANCE DATA
    std::vector<int32_t> frame_indices;
};
```

### File: `/src/zarr_loader.h` (Lines 213-232)

```cpp
struct ChaserStateRecord {
    int32_t stimulus_frame_num = -1;
    int32_t camera_frame_id = -1;
    int32_t chaser_index = -1;
    float chaser_pos_x = std::numeric_limits<float>::quiet_NaN();
    float chaser_pos_y = std::numeric_limits<float>::quiet_NaN();
    float target_pos_x = std::numeric_limits<float>::quiet_NaN();
    float target_pos_y = std::numeric_limits<float>::quiet_NaN();
    float chaser_radius_px = std::numeric_limits<float>::quiet_NaN();
    float distance_to_target_px = std::numeric_limits<float>::quiet_NaN();  // DISTANCE IN PIXELS
    float target_speed_px_per_s = std::numeric_limits<float>::quiet_NaN();
    int64_t timestamp_ns_session = 0;
    uint8_t is_chasing = 0;
    bool texture_space = true;
    double chaser_camera_x = std::numeric_limits<double>::quiet_NaN();
    double chaser_camera_y = std::numeric_limits<double>::quiet_NaN();
    double target_camera_x = std::numeric_limits<double>::quiet_NaN();
    double target_camera_y = std::numeric_limits<double>::quiet_NaN();
    bool has_camera_coords = false;
};
```

---

## Accessor Methods

### File: `/src/zarr_loader.h` (Lines 359-402)

```cpp
const std::vector<float>& getMovementTimeSeconds() const {
    static const std::vector<float> kEmpty;
    const auto* series = getSelectedMovementSeries();
    return series ? series->time_seconds : kEmpty;
}

const std::vector<float>& getMovementSmoothedSpeedMm() const {
    static const std::vector<float> kEmpty;
    const auto* series = getSelectedMovementSeries();
    return series ? series->smoothed_speed_mm : kEmpty;
}

const std::vector<float>& getMovementInstantaneousSpeedMm() const {
    static const std::vector<float> kEmpty;
    const auto* series = getSelectedMovementSeries();
    return series ? series->instant_speed_mm : kEmpty;
}

const std::vector<float>& getMovementDistanceToTargetMm() const {
    static const std::vector<float> kEmpty;
    const auto* series = getSelectedMovementSeries();
    return series ? series->distance_to_target_mm : kEmpty;  // DISTANCE DATA
}

const std::vector<int32_t>& getMovementFrameIndices() const {
    static const std::vector<int32_t> kEmpty;
    const auto* series = getSelectedMovementSeries();
    return series ? series->frame_indices : kEmpty;
}

const std::string& getMovementRunName() const {
    static const std::string kEmpty;
    const auto* series = getSelectedMovementSeries();
    return series ? series->run_name : kEmpty;
}

const std::string& getMovementTrackId() const {
    static const std::string kEmpty;
    const auto* series = getSelectedMovementSeries();
    return series ? series->track_id : kEmpty;
}

const std::string& getMovementCategory() const {
    static const std::string kEmpty;
    const auto* series = getSelectedMovementSeries();
    return series ? series->category : kEmpty;
}

size_t getMovementSeriesCount() const;
const ZarrDetectionData::MovementSeries* getMovementSeries(size_t index) const;
size_t getSelectedMovementSeriesIndex() const;
const ZarrDetectionData::MovementSeries* getSelectedMovementSeries() const;
bool selectMovementSeries(size_t index);
```

---

## Usage Example

The Distance Timeline is typically accessed through the ImGui menu during video playback:

1. **Load a Zarr file** with movement analysis data
2. **Open the Distance Timeline window** from the visualization menu
3. **Select a dataset** from the combo box (if multiple movement series exist)
4. **View the plot**: Time on x-axis (seconds), Distance on y-axis (mm)
5. **Follow along**: Yellow vertical line tracks current playback position
6. **Check statistics**: View average, max, and min distances below the plot

