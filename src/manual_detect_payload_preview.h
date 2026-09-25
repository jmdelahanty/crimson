#pragma once

#include "zarr_bbox_edit.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct ManualDetectPayloadPreview {
    size_t total_frames = 0;
    size_t total_detections = 0;
    size_t clean_rows = 0;
    size_t interpolated_rows = 0;
    size_t manual_rows = 0;
    bool valid = false;
    std::string error;
    std::vector<int32_t> frame_indices;
    std::vector<std::array<double, 4>> bbox_norm_coords;
    std::vector<float> scores;
    std::vector<int32_t> class_ids;
    std::vector<int32_t> frame_counts;
    std::vector<int32_t> n_detections;
    std::vector<int32_t> frame_mapping;
    std::vector<int8_t> detection_source;
    std::vector<std::string> reason;
};

ManualDetectPayloadPreview buildManualDetectPayloadPreview(
    bool zarr_loaded,
    const ZarrDetectionLoader& zarr_loader,
    const ZarrBBoxEditState& bbox_edit_state,
    int fallback_image_width,
    int fallback_image_height);

std::string summarizeManualDetectPayloadPreview(
    const ManualDetectPayloadPreview& preview,
    size_t dirty_frame_count);
