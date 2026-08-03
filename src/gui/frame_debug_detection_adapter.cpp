#include "gui/frame_debug_detection_adapter.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>

namespace {

std::string formatConfidence(float value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << value;
    return stream.str();
}

}  // namespace

crimson::gui::DetectionInspectPresentation
makeFrameDebugDetectionInspectPresentation(
    const FrameDebugWindowContext &context) {
    crimson::gui::DetectionInspectPresentation presentation;
    presentation.presentation_label = "Current-frame presentation";
    presentation.unavailable_message =
        "Detection runs are unavailable in metadata/stimulus-only mode.";
    presentation.available = context.zarr_loader.hasDetectionData();
    if (!presentation.available) {
        return presentation;
    }

    if (context.detection_dataset_choice >= 0 &&
        context.detection_dataset_choice <
            static_cast<int>(context.detection_dataset_labels.size())) {
        presentation.surface_label =
            context.detection_dataset_labels[context.detection_dataset_choice];
    } else {
        presentation.surface_label = "Legacy detection adapter";
    }
    presentation.run_name = context.zarr_loader.getDetectRunName();
    presentation.frame_ready = true;
    presentation.camera_frame = context.current_frame_num;
    presentation.observations.reserve(context.zarr_boxes.size());
    for (size_t index = 0; index < context.zarr_boxes.size(); ++index) {
        crimson::gui::DetectionInspectObservation observation;
        if (context.detection_details != nullptr) {
            if (index < context.detection_details->scores.size() &&
                std::isfinite(context.detection_details->scores[index])) {
                observation.confidence =
                    context.detection_details->scores[index];
                observation.confidence_valid = true;
            }
            if (index < context.detection_details->class_ids.size()) {
                observation.class_id =
                    context.detection_details->class_ids[index];
                observation.class_id_valid = true;
            }
            if (index < context.detection_details->detection_source.size() &&
                context.detection_details->detection_source[index] != 0) {
                observation.source_label = "Interpolated";
            }
        }
        if (observation.source_label.empty()) {
            observation.source_label =
                context.frame_is_interpolated &&
                        context.dataset_has_synthetic_boxes
                    ? "Interpolated"
                    : "Raw";
        }
        presentation.observations.push_back(std::move(observation));
    }

    if (context.detection_details != nullptr &&
        !context.detection_details->scores.empty()) {
        float maximum = 0.0f;
        bool maximum_valid = false;
        for (const float score : context.detection_details->scores) {
            if (std::isfinite(score) &&
                (!maximum_valid || score > maximum)) {
                maximum = score;
                maximum_valid = true;
            }
        }
        if (maximum_valid) {
            presentation.detail_lines.push_back(
                "Max confidence: " + formatConfidence(maximum));
        }
    }
    if (context.zarr_loader.hasClassIDs()) {
        presentation.detail_lines.push_back("Class IDs available: Yes");
    }
    if (context.zarr_loader.hasHeadingData()) {
        if (!context.dataset_has_synthetic_boxes &&
            context.detection_details != nullptr &&
            !context.detection_details->heading_valid.empty()) {
            const size_t valid = static_cast<size_t>(std::count(
                context.detection_details->heading_valid.begin(),
                context.detection_details->heading_valid.end(), uint8_t{1}));
            presentation.detail_lines.push_back(
                "Heading vectors: " + std::to_string(valid) + " valid");
        } else {
            presentation.detail_lines.push_back(
                "Heading vectors available (use original detections)");
        }
    }
    if (context.zarr_loader.hasInterpolation()) {
        presentation.detail_lines.push_back("Interpolation available: Yes");
        presentation.detail_lines.push_back(
            std::string("Current frame interpolated: ") +
            (context.frame_is_interpolated ? "Yes" : "No"));
        presentation.detail_lines.push_back(
            std::string("Using interpolation: ") +
            (context.dataset_has_synthetic_boxes ? "Yes" : "No"));
        presentation.detail_lines.push_back(
            "Method: " + context.zarr_loader.getInterpolationMethod());
    }
    return presentation;
}
