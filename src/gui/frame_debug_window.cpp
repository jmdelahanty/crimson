#include "gui/frame_debug_window.h"
#include "gui/frame_debug_bbox_panel.h"
#include "gui/frame_debug_review_panel.h"
#include "gui/frame_debug_status_panel.h"
#include "gui/frame_inspect_window.h"
#include "gui/overlay_debug_panel.h"
#include "gui/refined_keypoint_review_panel.h"
#include "gui/canonical_overlay_session.h"
#include "gui/keypoint_overlay_inspect_adapter.h"
#include "gui/subject_mask_overlay_inspect_adapter.h"
#include "gui/subject_shape_overlay_inspect_adapter.h"
#include "gui/camera_view_subject_shape_controls_adapter.h"
#include "gui/subject_shape_overlay_controls.h"
#include "gui/eye_geometry_overlay_controls.h"
#include "gui/eye_geometry_overlay_inspect_adapter.h"

#include "imgui.h"

FrameDebugWindowResult drawFrameDebugWindow(const FrameDebugWindowContext& context,
                                            FrameDebugWindowState& state) {
    FrameDebugWindowResult result;
    result.review_frame_filters = context.review_frame_filters;
    result.show_keypoint_markers = context.show_keypoint_markers;
    result.show_heading_arrows = context.show_heading_arrows;
    result.show_eye_masks = context.show_eye_masks;
    result.show_eye_geometry = context.show_eye_geometry;
    result.show_subject_body_mask = context.show_subject_body_mask;
    result.show_eye_left_mask = context.show_eye_left_mask;
    result.show_eye_right_mask = context.show_eye_right_mask;
    result.show_swim_bladder_mask = context.show_swim_bladder_mask;
    result.show_eye_direction_beams = context.show_eye_direction_beams;
    result.show_eye_gaze_rays = context.show_eye_gaze_rays;
    result.show_eye_angle_arcs = context.show_eye_angle_arcs;
    result.show_eye_angle_labels = context.show_eye_angle_labels;
    result.mask_overlay_mode = context.mask_overlay_mode;
    result.subject_shape_overlay_options =
        context.subject_shape_overlay_options;
    result.tail_kinematics_overlay_options =
        context.tail_kinematics_overlay_options;
    result.show_movement_trail = context.show_movement_trail;
    result.movement_trail_seconds = context.movement_trail_seconds;
    result.movement_trail_valid_samples_only =
        context.movement_trail_valid_samples_only;
    result.stimulus_inset_options = context.stimulus_inset_options;
    result.chaser_distance_polar_inset_options =
        context.chaser_distance_polar_inset_options;
    result.show_stimulus_debug_windows =
        context.show_stimulus_debug_windows;

    using crimson::gui::FrameInspectModule;
    using crimson::workspace::FrameInspectView;
    const FrameDebugModuleCatalog module_catalog =
        buildFrameDebugModuleCatalog(context);
    crimson::gui::FrameInspectWindowComposition composition;
    composition.draw_header = [&]() {
        if (context.canonical_overlays) {
            const auto& snapshot = *context.canonical_overlays;
            ImGui::Text("Canonical recording overlays | frame %lld | %s",
                        static_cast<long long>(snapshot.requested_frame),
                        crimson::gui::canonicalOverlayStateName(snapshot.state));
            ImGui::TextDisabled("Read-only: exact eye-bound product sources; stored observation identities.");
            if (!snapshot.error.empty()) ImGui::TextWrapped("%s", snapshot.error.c_str());
        } else {
            drawFrameDebugStatusHeader(context, state);
        }
    };
    const auto add_module = [&](FrameInspectView view) {
        composition.modules.push_back(FrameInspectModule{
            view,
            context.canonical_overlays && view == FrameInspectView::EyeMasks
                ? "Masks / Shape" : frameDebugModuleLabel(context, view),
            context.zarr_loaded &&
                ((context.canonical_overlays &&
                  (view == FrameInspectView::Keypoints || view == FrameInspectView::EyeMasks ||
                   view == FrameInspectView::EyeAngles)) ||
                 frameDebugModuleAvailable(context, module_catalog, view)),
            [&, view]() {
                if (context.canonical_overlays &&
                    (view == FrameInspectView::Keypoints || view == FrameInspectView::EyeMasks ||
                     view == FrameInspectView::EyeAngles)) {
                    const auto& snapshot = *context.canonical_overlays;
                    const auto show_status = [](const char* label, const auto& product) {
                        ImGui::Text("%s: %s", label, crimson::gui::canonicalOverlayStateName(product.state));
                        if (!product.error.empty()) ImGui::TextWrapped("%s", product.error.c_str());
                    };
                    if (view == FrameInspectView::Keypoints) {
                        ImGui::Checkbox("Show keypoint markers", &result.show_keypoint_markers);
                        ImGui::SameLine();
                        ImGui::Checkbox("Show heading arrows", &result.show_heading_arrows);
                        show_status("Keypoints", snapshot.keypoints);
                        auto presentation = crimson::gui::makeKeypointOverlayInspectPresentation(
                            snapshot.keypoints.descriptor.run_name.empty() ? nullptr : &snapshot.keypoints.descriptor,
                            snapshot.keypoints.frame.get(), snapshot.requested_frame);
                        crimson::gui::drawFrameInspectKeypointModule(presentation, state.canonical_keypoint_inspect);
                        ImGui::TextDisabled("Heading authority: the exact bound subject-shape body frame.");
                    } else if (view == FrameInspectView::EyeAngles) {
                        crimson::gui::EyeGeometryOverlayControlState controls{
                            result.show_eye_geometry, result.show_eye_direction_beams,
                            result.show_eye_gaze_rays, result.show_eye_angle_arcs,
                            result.show_eye_angle_labels};
                        crimson::gui::drawEyeGeometryOverlayControls(
                            controls, {!snapshot.eyes.descriptor.run_name.empty(), true});
                        result.show_eye_geometry = controls.show_overlay;
                        result.show_eye_direction_beams = controls.show_direction_beams;
                        result.show_eye_gaze_rays = controls.show_gaze_rays;
                        result.show_eye_angle_arcs = controls.show_angle_arcs;
                        result.show_eye_angle_labels = controls.show_angle_labels;
                        show_status("Eye geometry", snapshot.eyes);
                        if (!controls.show_overlay)
                            ImGui::TextDisabled("Enable eye geometry to request per-observation data.");
                        auto eyes = crimson::gui::makeEyeGeometryOverlayInspectPresentation(
                            snapshot.eyes.descriptor.run_name.empty() ? nullptr : &snapshot.eyes.descriptor,
                            snapshot.eyes.frame.get(), snapshot.requested_frame);
                        crimson::gui::drawFrameInspectEyeAngleModule(eyes, state.eye_angle_inspect);
                    } else {
                        ImGui::Checkbox("Show subject masks", &result.show_eye_masks);
                        ImGui::Checkbox("Body", &result.show_subject_body_mask);
                        ImGui::SameLine(); ImGui::Checkbox("Left eye", &result.show_eye_left_mask);
                        ImGui::SameLine(); ImGui::Checkbox("Right eye", &result.show_eye_right_mask);
                        ImGui::SameLine(); ImGui::Checkbox("Swim bladder", &result.show_swim_bladder_mask);
                        show_status("Masks", snapshot.masks);
                        show_status("Contours", snapshot.mask_contours);
                        if (!snapshot.mask_contour_error.empty())
                            ImGui::TextDisabled("Contours: %s", snapshot.mask_contour_error.c_str());
                        auto masks = crimson::gui::makeSubjectMaskOverlayInspectPresentation(
                            snapshot.masks.descriptor.run_name.empty() ? nullptr : &snapshot.masks.descriptor,
                            snapshot.masks.frame.get(), snapshot.requested_frame);
                        crimson::gui::drawFrameInspectSubjectMaskModule(masks, state.subject_mask_inspect);
                        ImGui::Separator();
                        auto shape_controls = makeCameraViewSubjectShapeOverlayControlState(
                            result.subject_shape_overlay_options);
                        const bool contour_available =
                            !snapshot.mask_contours.descriptor.presentation_cache_run.empty();
                        crimson::gui::drawSubjectShapeOverlayControls(
                            shape_controls,
                            {!snapshot.shapes.descriptor.run_name.empty(),
                             contour_available, contour_available,
                             contour_available,
                             snapshot.shapes.descriptor.bspline_sample_point_count > 0,
                             snapshot.shapes.descriptor.bspline_control_point_count > 0,
                             snapshot.shapes.descriptor.tail_sample_point_count > 0,
                             "Show shape / contours"});
                        applyCameraViewSubjectShapeOverlayControlState(
                            shape_controls, &result.subject_shape_overlay_options);
                        show_status("Shape", snapshot.shapes);
                        auto shapes = crimson::gui::makeSubjectShapeOverlayInspectPresentation(
                            snapshot.shapes.descriptor.run_name.empty() ? nullptr : &snapshot.shapes.descriptor,
                            snapshot.shapes.frame.get(), snapshot.requested_frame);
                        crimson::gui::drawFrameInspectSubjectShapeModule(shapes, state.subject_shape_inspect);
                    }
                    return;
                }
                drawFrameDebugStatusModule(
                    context, module_catalog, state, result, view);
                switch (view) {
                    case FrameInspectView::Detect:
                        drawFrameDebugReviewPanel(context, result);
                        drawFrameDebugBBoxPanel(context, state, result);
                        drawTrackKinematicsOverlayPanel(context, result);
                        break;
                    case FrameInspectView::Keypoints:
                        drawKeypointHeadingOverlayPanel(context, result);
                        {
                            const RefinedKeypointReviewPanelContext
                                keypoint_review_panel_context{
                                    context.zarr_loader,
                                    context.detection_details,
                                    context.current_frame_num,
                                    context.bbox_edit_state.selected_frame,
                                    context.bbox_edit_state.selected_box,
                                    context.play_video,
                            };
                            const auto keypoint_review_panel_result =
                                drawRefinedKeypointReviewPanel(
                                    keypoint_review_panel_context,
                                    state.keypoint_review_panel);
                            result.selected_keypoint_selection =
                                keypoint_review_panel_result.selected_selection;
                            result.keypoint_edit_action =
                                keypoint_review_panel_result.edit_action;
                            result.request_keypoint_review_write =
                                keypoint_review_panel_result
                                    .request_review_write;
                            result.keypoint_review_options =
                                keypoint_review_panel_result.review_options;
                        }
                        break;
                    case FrameInspectView::EyeMasks:
                        drawEyeMaskOverlayPanel(context, result);
                        break;
                    case FrameInspectView::TailKinematics:
                        break;
                    case FrameInspectView::EyeAngles:
                        break;
                }
            },
        });
    };
    add_module(FrameInspectView::Detect);
    add_module(FrameInspectView::Keypoints);
    add_module(FrameInspectView::EyeMasks);
    add_module(FrameInspectView::TailKinematics);
    add_module(FrameInspectView::EyeAngles);

    ImGui::SetNextWindowSize(ImVec2(760.0f, 840.0f), ImGuiCond_FirstUseEver);
    crimson::gui::FrameInspectWindowOptions options;
    options.tab_bar_id = "##frame_inspect_data_tabs";
    options.empty_message = context.zarr_loaded
                                ? "No inspection data is available."
                                : "[Zarr] Detections: Not loaded";
    crimson::gui::drawFrameInspectWindow(
        options, state.active_view, state.view_sync, composition);
    return result;
}
