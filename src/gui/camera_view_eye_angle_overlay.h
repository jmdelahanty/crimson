#pragma once

#include "gui/camera_view_overlay_renderer.h"
#include "zarr_loader.h"

#include "imgui.h"

#include <array>
#include <string>
#include <vector>

struct CameraViewEyeAngleOverlayState {
    std::array<std::vector<ImVec2>, 2> visual_cone_polygons;
    std::array<ImVec2, 2> visual_cone_dirs_world = {
        ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f)};
    std::array<ImVec2, 2> visual_cone_label_points = {
        ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f)};
    std::array<bool, 2> visual_cone_valid = {false, false};
    bool drew_vergence_label = false;
};

void resetCameraViewEyeAngleOverlaySmoothing(const std::string& run_id);

void drawCameraViewEyeAngleOverlayForEye(
    const ZarrDetectionLoader::FrameDetections::EyeMask& mask_info,
    const ZarrDetectionLoader::FrameDetections::SubjectShape* subject_shape,
    const std::array<float, 4>& detection_box,
    int eye,
    const std::string& base_id,
    const ImVec4& base_color,
    double cell_w,
    double cell_h,
    float scene_height_f,
    const CameraViewMaskOverlayOptions& options,
    CameraViewMaskPerfMetrics& metrics,
    CameraViewEyeAngleOverlayState& state);
