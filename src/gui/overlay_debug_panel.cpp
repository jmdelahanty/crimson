#include "gui/overlay_debug_panel.h"

#include "imgui.h"

#include <algorithm>
#include <limits>
#include <string>

namespace {

void drawKeypointHeadingOverlaySection(const FrameDebugWindowContext& context,
                                       FrameDebugWindowResult& result) {
    if (!(context.zarr_loader.hasKeypointData() ||
          context.zarr_loader.hasHeadingData())) {
        return;
    }

    result.show_keypoint_markers = context.show_keypoint_markers;
    result.show_heading_arrows = context.show_heading_arrows;

    ImGui::Separator();
    if (context.zarr_loader.hasKeypointData()) {
        ImGui::Text("Keypoint Overlay:");
        ImGui::Checkbox("Show keypoint markers", &result.show_keypoint_markers);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Overlay swim bladder and eye keypoints on the video frame.");
        }
        if (!context.zarr_loader.getKeypointsRunName().empty()) {
            ImGui::Text("  Keypoints run: %s (%s)",
                        context.zarr_loader.getKeypointsRunName().c_str(),
                        context.zarr_loader.isRefinedKeypoints() ? "refined"
                                                                 : "raw");
        }
        if (context.detection_details != nullptr &&
            context.detection_details->is_refined_keypoints &&
            !context.detection_details->keypoint_usable.empty()) {
            size_t usable_count = 0;
            size_t flip_count = 0;
            size_t det_count = context.detection_details->keypoint_usable.size();
            for (size_t qi = 0; qi < det_count; ++qi) {
                if (context.detection_details->keypoint_usable[qi] != 0) {
                    usable_count++;
                }
                if (qi < context.detection_details->keypoint_flip_corrected.size() &&
                    context.detection_details->keypoint_flip_corrected[qi] != 0) {
                    flip_count++;
                }
            }
            ImGui::Text("  Quality: %zu/%zu usable (%zu flip-corrected)",
                        usable_count,
                        det_count,
                        flip_count);
            if (!context.detection_details->keypoint_reason.empty() &&
                !context.detection_details->keypoint_reason[0].empty()) {
                ImGui::TextWrapped("  Reason: %s",
                                   context.detection_details->keypoint_reason[0]
                                       .c_str());
            }
        }
        if (context.detection_details != nullptr &&
            context.detection_details->keypoints_per_detection > 0 &&
            !context.detection_details->keypoint_labels.empty()) {
            std::string label_list;
            for (size_t i = 0;
                 i < context.detection_details->keypoint_labels.size();
                 ++i) {
                if (i > 0) {
                    label_list += ", ";
                }
                label_list += context.detection_details->keypoint_labels[i];
                if (label_list.size() > 72 &&
                    i + 1 < context.detection_details->keypoint_labels.size()) {
                    label_list += "...";
                    break;
                }
            }
            if (!label_list.empty()) {
                ImGui::TextWrapped("  Labels: %s", label_list.c_str());
            }
        }
        if (context.zarr_loader.activeDatasetHasSyntheticDetections()) {
            ImGui::TextWrapped(
                "Synthetic detections are present; interpolated boxes draw with hollow keypoint markers.");
        }
    }

    if (context.zarr_loader.hasHeadingData()) {
        if (context.zarr_loader.hasKeypointData()) {
            ImGui::Spacing();
        }
        ImGui::Text("Heading Overlay:");
        ImGui::Checkbox("Show heading arrows", &result.show_heading_arrows);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Visualize swim bladder headings from the keypoints run.");
        }
        if (!context.zarr_loader.getKeypointsRunName().empty() &&
            !context.zarr_loader.hasKeypointData()) {
            ImGui::Text("  Keypoints run: %s (%s)",
                        context.zarr_loader.getKeypointsRunName().c_str(),
                        context.zarr_loader.isRefinedKeypoints() ? "refined"
                                                                 : "raw");
        }
        if (context.zarr_loader.activeDatasetHasSyntheticDetections()) {
            ImGui::TextWrapped(
                "Synthetic detections are present; arrows render only for real boxes.");
        }
    }
}

void drawInterpolationSection(const FrameDebugWindowContext& context) {
    if (!context.zarr_loader.hasInterpolation()) {
        return;
    }

    ImGui::Separator();
    ImGui::Text("Interpolation Status:");
    ImGui::Text("  Current frame interpolated: %s",
                context.frame_is_interpolated ? "Yes" : "No");
    ImGui::Text("  Dataset uses interpolation: %s",
                context.zarr_loader.activeDatasetHasSyntheticDetections()
                    ? "Yes"
                    : "No");
    ImGui::Text("  Method: %s",
                context.zarr_loader.getInterpolationMethod().c_str());
}

void drawEyeMaskSection(const FrameDebugWindowContext& context,
                        FrameDebugWindowResult& result) {
    if (!context.zarr_loader.hasEyeMasks()) {
        return;
    }

    const bool refined_subject_masks =
        context.zarr_loader.eyeMasksUseRefinedSubjectMasks();
    result.show_eye_masks = context.show_eye_masks;
    ImGui::Separator();
    ImGui::Text("%s", refined_subject_masks ? "Subject Mask Overlay:"
                                            : "Eye Mask Overlay:");
    ImGui::Checkbox(refined_subject_masks ? "Show subject masks"
                                          : "Show eye masks",
                    &result.show_eye_masks);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            refined_subject_masks
                ? "Visualize refined subject-mask components as semi-transparent overlays."
                : "Visualize legacy refined eye masks as semi-transparent overlays.");
    }
    if (!context.zarr_loader.getEyeMaskSourceLabel().empty()) {
        ImGui::Text("  Source: %s",
                    context.zarr_loader.getEyeMaskSourceLabel().c_str());
    }
    if (!context.zarr_loader.getEyeMaskSourcePath().empty()) {
        ImGui::TextWrapped("  Dataset: %s",
                           context.zarr_loader.getEyeMaskSourcePath().c_str());
    } else if (!context.zarr_loader.getEyeMaskRunName().empty()) {
        ImGui::Text("  %s run: %s",
                    refined_subject_masks ? "Subject mask" : "Eye mask",
                    context.zarr_loader.getEyeMaskRunName().c_str());
    }
    if (refined_subject_masks) {
        result.show_subject_body_mask = context.show_subject_body_mask;
        result.show_eye_left_mask = context.show_eye_left_mask;
        result.show_eye_right_mask = context.show_eye_right_mask;
        result.show_swim_bladder_mask = context.show_swim_bladder_mask;

        const auto& labels = context.zarr_loader.getEyeMaskChannelLabels();
        const auto& channels = context.zarr_loader.getEyeMaskChannelIndices();
        const std::string left_channel =
            channels[0] == std::numeric_limits<size_t>::max()
                ? "unavailable"
                : std::to_string(channels[0]);
        const std::string right_channel =
            channels[1] == std::numeric_limits<size_t>::max()
                ? "unavailable"
                : std::to_string(channels[1]);
        ImGui::Text("  Channels: %s=%s, %s=%s",
                    labels[0].c_str(),
                    left_channel.c_str(),
                    labels[1].c_str(),
                    right_channel.c_str());
        const auto& components =
            context.zarr_loader.getRefinedSubjectMaskOverlayComponents();
        const size_t contour_components = static_cast<size_t>(
            std::count_if(
                components.begin(),
                components.end(),
                [](const ZarrDetectionData::RefinedSubjectMaskComponentInfo&
                       component) {
                    return component.contours_available;
                }));
        ImGui::Text("  Contours: %zu/%zu components",
                    contour_components,
                    components.size());

        ImGui::Checkbox("Subject body", &result.show_subject_body_mask);
        ImGui::SameLine();
        ImGui::Checkbox("Swim bladder", &result.show_swim_bladder_mask);
        ImGui::Checkbox("Left eye", &result.show_eye_left_mask);
        ImGui::SameLine();
        ImGui::Checkbox("Right eye", &result.show_eye_right_mask);
    }
    if (!context.zarr_loader.getEyeMaskWarning().empty()) {
        ImGui::TextWrapped("  Warning: %s",
                           context.zarr_loader.getEyeMaskWarning().c_str());
    }
    if (context.zarr_loader.activeDatasetHasSyntheticDetections()) {
        ImGui::TextWrapped(
            "Synthetic detections are present; masks are skipped for interpolated boxes.");
    }
}

}  // namespace

void drawKeypointHeadingOverlayPanel(const FrameDebugWindowContext& context,
                                     FrameDebugWindowResult& result) {
    result.show_keypoint_markers = context.show_keypoint_markers;
    result.show_heading_arrows = context.show_heading_arrows;

    drawKeypointHeadingOverlaySection(context, result);
    drawInterpolationSection(context);
}

void drawEyeMaskOverlayPanel(const FrameDebugWindowContext& context,
                             FrameDebugWindowResult& result) {
    result.show_eye_masks = context.show_eye_masks;
    result.show_subject_body_mask = context.show_subject_body_mask;
    result.show_eye_left_mask = context.show_eye_left_mask;
    result.show_eye_right_mask = context.show_eye_right_mask;
    result.show_swim_bladder_mask = context.show_swim_bladder_mask;
    drawEyeMaskSection(context, result);
}
