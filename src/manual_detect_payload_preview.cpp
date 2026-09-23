#include "manual_detect_payload_preview.h"

#include "ui_path_config.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

namespace {

std::array<double, 4> toNormalizedCxCyWh(float x_min,
                                         float y_min,
                                         float width,
                                         float height,
                                         int image_width,
                                         int image_height) {
    const double max_w = static_cast<double>(image_width);
    const double max_h = static_cast<double>(image_height);
    const double clamped_x_min =
        std::clamp(static_cast<double>(x_min), 0.0, max_w);
    const double clamped_y_min =
        std::clamp(static_cast<double>(y_min), 0.0, max_h);
    const double clamped_width = std::clamp(
        static_cast<double>(width), 0.0, std::max(0.0, max_w - clamped_x_min));
    const double clamped_height = std::clamp(
        static_cast<double>(height), 0.0, std::max(0.0, max_h - clamped_y_min));
    const double cx = (clamped_x_min + 0.5 * clamped_width) / max_w;
    const double cy = (clamped_y_min + 0.5 * clamped_height) / max_h;
    const double w = clamped_width / max_w;
    const double h = clamped_height / max_h;
    return {std::clamp(cx, 0.0, 1.0), std::clamp(cy, 0.0, 1.0),
            std::clamp(w, 0.0, 1.0), std::clamp(h, 0.0, 1.0)};
}

std::pair<int8_t, std::string> resolveReasonAndSource(bool force_manual,
                                                      uint8_t source_flag,
                                                      const std::string& source_reason) {
    if (force_manual) {
        return {0, "manual"};
    }
    const std::string lowered = ToLowerCopy(source_reason);
    if (!lowered.empty()) {
        if (lowered == "manual" || lowered.find("manual") != std::string::npos) {
            return {0, "manual"};
        }
        if (lowered == "interpolated" ||
            lowered.find("interp") != std::string::npos) {
            return {1, "interpolated"};
        }
        if (lowered == "clean") {
            return {0, "clean"};
        }
    }
    return {source_flag != 0 ? 1 : 0,
            source_flag != 0 ? "interpolated" : "clean"};
}

void countReason(const std::string& reason, ManualDetectPayloadPreview& preview) {
    if (reason == "manual") {
        ++preview.manual_rows;
    } else if (reason == "interpolated") {
        ++preview.interpolated_rows;
    } else {
        ++preview.clean_rows;
    }
}

template <typename T>
const T* findFrameMetadata(const std::unordered_map<int, T>& frame_map,
                           int frame_id) {
    auto it = frame_map.find(frame_id);
    return (it != frame_map.end()) ? &it->second : nullptr;
}

}  // namespace

ManualDetectPayloadPreview buildManualDetectPayloadPreview(
    bool zarr_loaded,
    const ZarrDetectionLoader& zarr_loader,
    const ZarrBBoxEditState& bbox_edit_state,
    int fallback_image_width,
    int fallback_image_height) {
    ManualDetectPayloadPreview preview;
    if (!zarr_loaded || !zarr_loader.hasDetectionData()) {
        preview.error = "No active Zarr detection dataset.";
        return preview;
    }
    preview.total_frames = zarr_loader.getTotalFrames();
    if (preview.total_frames == 0) {
        preview.error = "Active dataset has zero frames.";
        return preview;
    }

    int image_width = zarr_loader.getImageWidth();
    int image_height = zarr_loader.getImageHeight();
    if (image_width <= 0 || image_height <= 0) {
        image_width = fallback_image_width;
        image_height = fallback_image_height;
    }
    if (image_width <= 0 || image_height <= 0) {
        preview.error = "Could not resolve image dimensions for bbox normalization.";
        return preview;
    }

    preview.frame_counts.reserve(preview.total_frames);
    for (size_t frame_id = 0; frame_id < preview.total_frames; ++frame_id) {
        int32_t frame_count = 0;
        const int frame_id_i32 = static_cast<int>(frame_id);
        const bool has_override = bbox_edit_state.hasFrameOverride(frame_id_i32);
        auto base_detections = zarr_loader.getRawDetections(frame_id, false, false);

        if (!has_override) {
            for (size_t det_idx = 0; det_idx < base_detections.boxes.size();
                 ++det_idx) {
                const auto& box = base_detections.boxes[det_idx];
                const float x_min = box[0];
                const float y_min = box[1];
                const float width = std::max(0.0f, box[2] - box[0]);
                const float height = std::max(0.0f, box[3] - box[1]);

                preview.frame_indices.push_back(static_cast<int32_t>(frame_id));
                preview.bbox_norm_coords.push_back(toNormalizedCxCyWh(
                    x_min, y_min, width, height, image_width, image_height));
                preview.scores.push_back(det_idx < base_detections.scores.size()
                                             ? base_detections.scores[det_idx]
                                             : 1.0f);
                preview.class_ids.push_back(
                    det_idx < base_detections.class_ids.size()
                        ? base_detections.class_ids[det_idx]
                        : 0);
                const uint8_t source_flag =
                    det_idx < base_detections.detection_source.size()
                        ? base_detections.detection_source[det_idx]
                        : 0;
                const std::string source_reason =
                    det_idx < base_detections.detection_reason.size()
                        ? base_detections.detection_reason[det_idx]
                        : std::string{};
                auto [resolved_source, resolved_reason] =
                    resolveReasonAndSource(false, source_flag, source_reason);
                preview.detection_source.push_back(resolved_source);
                preview.reason.push_back(resolved_reason);
                countReason(resolved_reason, preview);
                ++frame_count;
            }
        } else {
            auto override_it = bbox_edit_state.frame_overrides.find(frame_id_i32);
            if (override_it == bbox_edit_state.frame_overrides.end()) {
                preview.error = "Dirty frame override state is inconsistent.";
                return preview;
            }
            const auto& boxes = override_it->second;
            const auto* added_flags =
                findFrameMetadata(bbox_edit_state.frame_added_flags, frame_id_i32);
            const auto* manual_flags =
                findFrameMetadata(bbox_edit_state.frame_manual_flags, frame_id_i32);
            const auto* source_detection = findFrameMetadata(
                bbox_edit_state.frame_source_detection_source, frame_id_i32);
            const auto* source_reason =
                findFrameMetadata(bbox_edit_state.frame_source_reason, frame_id_i32);

            for (size_t box_idx = 0; box_idx < boxes.size(); ++box_idx) {
                const auto& box = boxes[box_idx];
                preview.frame_indices.push_back(static_cast<int32_t>(frame_id));
                preview.bbox_norm_coords.push_back(toNormalizedCxCyWh(
                    box.x_min, box.y_min, box.width, box.height, image_width,
                    image_height));
                preview.scores.push_back(
                    std::isfinite(box.confidence) ? box.confidence : 1.0f);
                preview.class_ids.push_back(static_cast<int32_t>(box.class_id));

                const bool is_added =
                    added_flags && box_idx < added_flags->size() &&
                    (*added_flags)[box_idx] != 0;
                const bool is_manual =
                    manual_flags && box_idx < manual_flags->size() &&
                    (*manual_flags)[box_idx] != 0;
                const uint8_t src_flag =
                    (source_detection && box_idx < source_detection->size())
                        ? (*source_detection)[box_idx]
                        : 0;
                const std::string src_reason =
                    (source_reason && box_idx < source_reason->size())
                        ? (*source_reason)[box_idx]
                        : std::string{};

                auto [resolved_source, resolved_reason] =
                    resolveReasonAndSource(is_added || is_manual, src_flag,
                                           src_reason);
                preview.detection_source.push_back(resolved_source);
                preview.reason.push_back(resolved_reason);
                countReason(resolved_reason, preview);
                ++frame_count;
            }
        }

        preview.frame_counts.push_back(frame_count);
    }

    preview.n_detections = preview.frame_counts;
    preview.frame_mapping = preview.frame_indices;
    preview.total_detections = preview.frame_indices.size();

    const size_t total_rows = preview.total_detections;
    const bool lengths_match =
        preview.bbox_norm_coords.size() == total_rows &&
        preview.scores.size() == total_rows &&
        preview.class_ids.size() == total_rows &&
        preview.frame_mapping.size() == total_rows &&
        preview.detection_source.size() == total_rows &&
        preview.reason.size() == total_rows;
    if (!lengths_match) {
        preview.error = "Payload arrays have inconsistent detection-level lengths.";
        return preview;
    }

    const int64_t frame_sum = std::accumulate(preview.frame_counts.begin(),
                                              preview.frame_counts.end(),
                                              int64_t{0});
    if (frame_sum != static_cast<int64_t>(total_rows)) {
        preview.error = "sum(frame_counts) does not equal detection row count.";
        return preview;
    }

    preview.valid = true;
    return preview;
}

std::string summarizeManualDetectPayloadPreview(
    const ManualDetectPayloadPreview& preview,
    size_t dirty_frame_count) {
    std::ostringstream payload_msg;
    payload_msg << "Manual payload preview: frames=" << preview.total_frames
                << " detections=" << preview.total_detections
                << " clean=" << preview.clean_rows
                << " interpolated=" << preview.interpolated_rows
                << " manual=" << preview.manual_rows
                << " dirty_frames=" << dirty_frame_count;
    return payload_msg.str();
}
