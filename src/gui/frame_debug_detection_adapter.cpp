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
    presentation.available = context.detection_descriptor.available;
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
    presentation.run_name = context.detection_descriptor.run_name;
    presentation.frame_ready = true;
    presentation.camera_frame = context.current_frame_num;
    presentation.observations.reserve(context.zarr_boxes.size());
    for (size_t index = 0; index < context.zarr_boxes.size(); ++index) {
        crimson::gui::DetectionInspectObservation observation;
        if (context.detection_frame != nullptr &&
            index < context.detection_frame->observations.size()) {
            const auto& source = context.detection_frame->observations[index];
            if (source.score_valid && std::isfinite(source.score)) {
                observation.confidence = source.score;
                observation.confidence_valid = true;
            }
            if (source.class_id_valid) {
                observation.class_id = source.class_id;
                observation.class_id_valid = true;
            }
            if (source.source_kind != 0) {
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

    if (context.detection_frame != nullptr &&
        !context.detection_frame->observations.empty()) {
        float maximum = 0.0f;
        bool maximum_valid = false;
        for (const auto& observation : context.detection_frame->observations) {
            if (observation.score_valid && std::isfinite(observation.score) &&
                (!maximum_valid || observation.score > maximum)) {
                maximum = observation.score;
                maximum_valid = true;
            }
        }
        if (maximum_valid) {
            presentation.detail_lines.push_back(
                "Max confidence: " + formatConfidence(maximum));
        }
    }
    if (context.detection_descriptor.has_class_ids) {
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
    if (context.detection_descriptor.interpolation_available) {
        presentation.detail_lines.push_back("Interpolation available: Yes");
        presentation.detail_lines.push_back(
            std::string("Current frame interpolated: ") +
            (context.frame_is_interpolated ? "Yes" : "No"));
        presentation.detail_lines.push_back(
            std::string("Using interpolation: ") +
            (context.dataset_has_synthetic_boxes ? "Yes" : "No"));
        presentation.detail_lines.push_back(
            "Method: " + context.detection_descriptor.interpolation_method);
    }
    return presentation;
}
