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
    result.mask_overlay_mode = context.mask_overlay_mode;
    result.show_eye_direction_beams = context.show_eye_direction_beams;
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

        const CameraViewMaskOverlayMode modes[] = {
            CameraViewMaskOverlayMode::Realtime,
            CameraViewMaskOverlayMode::Review,
            CameraViewMaskOverlayMode::Debug,
        };
        if (ImGui::BeginCombo("Mode##subject_mask_overlay_mode",
                              cameraViewMaskOverlayModeLabel(
                                  result.mask_overlay_mode))) {
            for (CameraViewMaskOverlayMode mode : modes) {
                const bool selected = mode == result.mask_overlay_mode;
                if (ImGui::Selectable(cameraViewMaskOverlayModeLabel(mode),
                                      selected)) {
                    result.mask_overlay_mode = mode;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Realtime draws fills and the selected contour only; Review/Debug draw contours and eye geometry.");
        }

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
    ImGui::Checkbox("Eye direction beams", &result.show_eye_direction_beams);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Draw translucent direction triangles from the eye fit axes. Disable this to keep eye masks, contours, axes, and angle labels without the beam overlay.");
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

void drawSubjectShapeSection(const FrameDebugWindowContext& context,
                             FrameDebugWindowResult& result) {
    if (!context.zarr_loader.hasSubjectShapeData()) {
        return;
    }

    auto options = context.subject_shape_overlay_options;
    ImGui::Separator();
    ImGui::Text("Subject Shape Geometry:");
    ImGui::Checkbox("Show subject shape", &options.show_overlay);
    ImGui::Text("  Run: %s", context.zarr_loader.getSubjectShapeRunName().c_str());
    if (!context.zarr_loader
             .getSubjectShapeSourceRefinedSubjectMasksRun()
             .empty()) {
        ImGui::TextWrapped(
            "  Source masks: %s",
            context.zarr_loader
                .getSubjectShapeSourceRefinedSubjectMasksRun()
                .c_str());
    }
    ImGui::Text("  Schema/method: v%d / %s v%d",
                context.zarr_loader.getSubjectShapeSchemaVersion(),
                context.zarr_loader.getSubjectShapeMethod().c_str(),
                context.zarr_loader.getSubjectShapeMethodVersion());
    if (!context.zarr_loader.getSubjectShapeWarning().empty()) {
        ImGui::TextWrapped("  Warning: %s",
                           context.zarr_loader.getSubjectShapeWarning().c_str());
    }

    ImGui::BeginDisabled(!options.show_overlay);
    ImGui::Checkbox("Snout tip", &options.show_snout_tip);
    ImGui::SameLine();
    ImGui::Checkbox("Tail base", &options.show_tail_base);
    ImGui::SameLine();
    ImGui::Checkbox("Tail tip", &options.show_tail_tip);
    ImGui::Checkbox("Caudal swim-bladder anchor",
                    &options.show_caudal_anchor);
    ImGui::Checkbox("Centerline", &options.show_centerline);
    ImGui::Checkbox("Dense B-spline centerline (geometry/QC)",
                    &options.show_bspline_sample);
    ImGui::Checkbox("Body frame axes", &options.show_body_frame_axes);
    ImGui::Checkbox("Body contour", &options.show_body_contour);
    ImGui::SameLine();
    ImGui::Checkbox("Swim-bladder contour",
                    &options.show_swim_bladder_contour);
    ImGui::Checkbox("Eye contours", &options.show_eye_contours);
    ImGui::Checkbox("Spline debug points",
                    &options.show_bspline_debug_points);
    ImGui::SameLine();
    ImGui::Checkbox("Spline control points",
                    &options.show_bspline_control_points);
    ImGui::Checkbox("Dense tail geometry samples (source geometry)",
                    &options.show_tail_samples);
    ImGui::Checkbox("Tail normals", &options.show_tail_normals);
    ImGui::EndDisabled();

    result.subject_shape_overlay_options = options;
}

void drawTailKinematicsOverlaySection(const FrameDebugWindowContext& context,
                                      FrameDebugWindowResult& result) {
    if (!context.zarr_loader.hasTailKinematicsData()) {
        return;
    }

    auto options = context.tail_kinematics_overlay_options;
    ImGui::Separator();
    ImGui::Text("Tail Kinematics:");
    ImGui::Checkbox("Show tail kinematics overlay", &options.show_overlay);
    ImGui::Text("  Run: %s",
                context.zarr_loader.getTailKinematicsRunName().c_str());
    if (!context.zarr_loader
             .getTailKinematicsSourceSubjectShapeRun()
             .empty()) {
        ImGui::TextWrapped(
            "  Source shape: %s",
            context.zarr_loader
                .getTailKinematicsSourceSubjectShapeRun()
                .c_str());
    }
    if (!context.zarr_loader.getTailKinematicsWarning().empty()) {
        ImGui::TextWrapped(
            "  Warning: %s",
            context.zarr_loader.getTailKinematicsWarning().c_str());
    }

    ImGui::BeginDisabled(!options.show_overlay);
    ImGui::Checkbox("Tail-angle samples k=10 (analysis output)",
                    &options.show_samples);
    ImGui::Checkbox("Connect k=10 samples", &options.show_segments);
    ImGui::Checkbox("Tail angle vectors", &options.show_angle_vectors);
    ImGui::Checkbox("Lateral deflection", &options.show_lateral_deflection);
    ImGui::Checkbox("Color invalid frames", &options.color_invalid_frames);
    ImGui::EndDisabled();

    result.tail_kinematics_overlay_options = options;
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
    result.show_eye_direction_beams = context.show_eye_direction_beams;
    result.mask_overlay_mode = context.mask_overlay_mode;
    drawEyeMaskSection(context, result);
    drawSubjectShapeSection(context, result);
    drawTailKinematicsOverlaySection(context, result);
}
