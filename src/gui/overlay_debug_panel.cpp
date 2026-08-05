#include "gui/overlay_debug_panel.h"

#include "gui/camera_view_subject_mask_controls_adapter.h"
#include "gui/camera_view_subject_shape_controls_adapter.h"
#include "gui/eye_geometry_overlay_controls.h"
#include "gui/subject_mask_overlay_controls.h"
#include "gui/subject_shape_overlay_controls.h"

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
    result.show_eye_gaze_rays = context.show_eye_gaze_rays;
    result.show_eye_angle_arcs = context.show_eye_angle_arcs;
    result.show_eye_angle_labels = context.show_eye_angle_labels;
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
        CameraViewMaskOverlayOptions mask_options;
        mask_options.show_subject_body = context.show_subject_body_mask;
        mask_options.show_eye_left = context.show_eye_left_mask;
        mask_options.show_eye_right = context.show_eye_right_mask;
        mask_options.show_swim_bladder = context.show_swim_bladder_mask;
        mask_options.mode = context.mask_overlay_mode;
        auto subject_mask_controls =
            makeCameraViewSubjectMaskOverlayControlState(mask_options);
        crimson::gui::drawSubjectMaskOverlayControls(
            subject_mask_controls,
            {true, true, false, false, false, false, false},
            {"Realtime draws fills and the selected contour only; "
             "Review/Debug draw contours and eye geometry."});

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
        const size_t sampled_contour_components = static_cast<size_t>(
            std::count_if(
                components.begin(),
                components.end(),
                [](const ZarrDetectionData::RefinedSubjectMaskComponentInfo&
                       component) {
                    return component.contours_available &&
                           component.sampled_contours_used;
                }));
        ImGui::Text("  Contours: %zu/%zu components (%zu sampled, %zu ragged)",
                    contour_components,
                    components.size(),
                    sampled_contour_components,
                    contour_components - sampled_contour_components);
        const std::string optional_overlay_status =
            context.zarr_loader.getRefinedSubjectMaskOptionalOverlayStatus();
        if (!optional_overlay_status.empty()) {
            ImGui::TextDisabled("  %s", optional_overlay_status.c_str());
        }

        crimson::gui::drawSubjectMaskOverlayControls(
            subject_mask_controls,
            {false, false, true, true, true, true, true});
        applyCameraViewSubjectMaskOverlayControlState(subject_mask_controls,
                                                      &mask_options);
        result.mask_overlay_mode = mask_options.mode;
        result.show_subject_body_mask = mask_options.show_subject_body;
        result.show_swim_bladder_mask = mask_options.show_swim_bladder;
        result.show_eye_left_mask = mask_options.show_eye_left;
        result.show_eye_right_mask = mask_options.show_eye_right;
    }
    crimson::gui::EyeGeometryOverlayControlState eye_geometry_controls;
    eye_geometry_controls.show_direction_beams =
        context.show_eye_direction_beams;
    eye_geometry_controls.show_gaze_rays = context.show_eye_gaze_rays;
    eye_geometry_controls.show_angle_arcs = context.show_eye_angle_arcs;
    eye_geometry_controls.show_angle_labels = context.show_eye_angle_labels;
    crimson::gui::drawEyeGeometryOverlayControls(eye_geometry_controls,
                                                 {true, false});
    result.show_eye_direction_beams =
        eye_geometry_controls.show_direction_beams;
    result.show_eye_gaze_rays = eye_geometry_controls.show_gaze_rays;
    result.show_eye_angle_arcs = eye_geometry_controls.show_angle_arcs;
    result.show_eye_angle_labels = eye_geometry_controls.show_angle_labels;
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

    ImGui::Separator();
    ImGui::Text("Subject Shape Geometry:");
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

    auto controls = makeCameraViewSubjectShapeOverlayControlState(
        context.subject_shape_overlay_options);
    crimson::gui::drawSubjectShapeOverlayControls(
        controls, {true, true, true, true});
    applyCameraViewSubjectShapeOverlayControlState(
        controls, &result.subject_shape_overlay_options);
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

void drawTrackKinematicsOverlaySection(const FrameDebugWindowContext& context,
                                       FrameDebugWindowResult& result) {
    if (!context.zarr_loader.hasMovementData()) {
        return;
    }

    result.show_movement_trail = context.show_movement_trail;
    result.movement_trail_seconds = context.movement_trail_seconds;
    result.movement_trail_valid_samples_only =
        context.movement_trail_valid_samples_only;

    ImGui::Separator();
    ImGui::Text("Track Kinematics Overlay:");
    ImGui::Checkbox("Show motion trail", &result.show_movement_trail);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Draw the selected track-kinematics position history with older samples faded out.");
    }
    ImGui::BeginDisabled(!result.show_movement_trail);
    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderFloat("Trail duration (s)",
                       &result.movement_trail_seconds,
                       0.1f,
                       5.0f,
                       "%.1f");
    result.movement_trail_seconds =
        std::clamp(result.movement_trail_seconds, 0.1f, 5.0f);
    ImGui::Checkbox("Valid samples only",
                    &result.movement_trail_valid_samples_only);
    ImGui::EndDisabled();

    ImGui::TextWrapped("  Source: %s | %s | %s",
                       context.zarr_loader.getMovementRunName().c_str(),
                       context.zarr_loader.getMovementTrackId().c_str(),
                       context.zarr_loader.getMovementSpeedLevel().c_str());
}

void drawStimulusOverlaySection(const FrameDebugWindowContext& context,
                                FrameDebugWindowResult& result) {
    if (!(context.zarr_loader.hasStimulusAlignment() ||
          context.zarr_loader.hasStimulusSteps())) {
        return;
    }

    result.stimulus_inset_options = context.stimulus_inset_options;
    result.show_stimulus_debug_windows =
        context.show_stimulus_debug_windows;

    ImGui::Separator();
    ImGui::Text("Stimulus Video:");
    ImGui::Checkbox("Camera-view inset",
                    &result.stimulus_inset_options.show_inset);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Draw the aligned stimulus video as a small inset in the camera view.");
    }
    ImGui::BeginDisabled(!result.stimulus_inset_options.show_inset);
    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderFloat("Inset width (px)",
                       &result.stimulus_inset_options.width_px,
                       120.0f,
                       360.0f,
                       "%.0f");
    result.stimulus_inset_options.width_px =
        std::clamp(result.stimulus_inset_options.width_px, 120.0f, 360.0f);
    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderFloat("Inset opacity",
                       &result.stimulus_inset_options.opacity,
                       0.20f,
                       1.0f,
                       "%.2f");
    result.stimulus_inset_options.opacity =
        std::clamp(result.stimulus_inset_options.opacity, 0.20f, 1.0f);
    ImGui::Checkbox("Frame label",
                    &result.stimulus_inset_options.show_frame_label);
    ImGui::EndDisabled();

    ImGui::Checkbox("Debug stimulus windows",
                    &result.show_stimulus_debug_windows);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Show the old standalone stimulus and stimulus-buffer debug windows.");
    }
}

void drawChaserDistancePolarOverlaySection(
    const FrameDebugWindowContext& context,
    FrameDebugWindowResult& result) {
    const auto* descriptor = context.chaser_distance_polar_descriptor;
    if (descriptor == nullptr || !descriptor->ready()) {
        return;
    }

    result.chaser_distance_polar_inset_options =
        context.chaser_distance_polar_inset_options;

    ImGui::Separator();
    ImGui::Text("Chaser Distance Polar Plot:");
    ImGui::Checkbox(
        "Camera-view polar inset",
        &result.chaser_distance_polar_inset_options.show_inset);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Draw a fish-centered chaser distance/bearing polar plot in the camera view.");
    }
    ImGui::BeginDisabled(
        !result.chaser_distance_polar_inset_options.show_inset);
    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderFloat("Polar inset width (px)",
                       &result.chaser_distance_polar_inset_options.width_px,
                       140.0f,
                       360.0f,
                       "%.0f");
    result.chaser_distance_polar_inset_options.width_px =
        std::clamp(result.chaser_distance_polar_inset_options.width_px,
                   140.0f,
                   360.0f);
    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderFloat("Polar inset opacity",
                       &result.chaser_distance_polar_inset_options.opacity,
                       0.20f,
                       1.0f,
                       "%.2f");
    result.chaser_distance_polar_inset_options.opacity =
        std::clamp(result.chaser_distance_polar_inset_options.opacity,
                   0.20f,
                   1.0f);
    ImGui::Checkbox("Polar labels",
                    &result.chaser_distance_polar_inset_options.show_labels);
    ImGui::SameLine();
    ImGui::Checkbox("Readout",
                    &result.chaser_distance_polar_inset_options.show_readout);
    ImGui::EndDisabled();
    ImGui::TextWrapped("  Source: %s / %s",
                       descriptor->provenance.run_name.c_str(),
                       descriptor->provenance.component_name.c_str());
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
    result.show_eye_gaze_rays = context.show_eye_gaze_rays;
    result.show_eye_angle_arcs = context.show_eye_angle_arcs;
    result.show_eye_angle_labels = context.show_eye_angle_labels;
    result.mask_overlay_mode = context.mask_overlay_mode;
    drawEyeMaskSection(context, result);
    drawSubjectShapeSection(context, result);
    drawTailKinematicsOverlaySection(context, result);
}

void drawTrackKinematicsOverlayPanel(const FrameDebugWindowContext& context,
                                     FrameDebugWindowResult& result) {
    drawTrackKinematicsOverlaySection(context, result);
    drawChaserDistancePolarOverlaySection(context, result);
    drawStimulusOverlaySection(context, result);
}
