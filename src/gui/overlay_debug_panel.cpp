#include "gui/overlay_debug_panel.h"

#include "imgui.h"

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

    result.show_eye_masks = context.show_eye_masks;
    ImGui::Separator();
    ImGui::Text("Eye Mask Overlay:");
    ImGui::Checkbox("Show refined eye masks", &result.show_eye_masks);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Visualize refined eye masks as semi-transparent overlays.");
    }
    if (!context.zarr_loader.getEyeMaskRunName().empty()) {
        ImGui::Text("  Eye mask run: %s",
                    context.zarr_loader.getEyeMaskRunName().c_str());
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
    drawEyeMaskSection(context, result);
}
