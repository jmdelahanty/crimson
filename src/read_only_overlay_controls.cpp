#include "read_only_overlay_controls.h"

namespace crimson::overlay {

const char* readOnlyMaskOverlayModeName(ReadOnlyMaskOverlayMode mode) {
    switch (mode) {
        case ReadOnlyMaskOverlayMode::Realtime:
            return "Realtime";
        case ReadOnlyMaskOverlayMode::Review:
            return "Review";
        case ReadOnlyMaskOverlayMode::Debug:
            return "Debug";
    }
    return "Review";
}

void applyReadOnlyOverlayControls(const ReadOnlyOverlayControlState& controls,
                                  ReadOnlyOverlayInput* input) {
    if (input == nullptr) {
        return;
    }

    const bool detailed_masks =
        controls.mask_mode != ReadOnlyMaskOverlayMode::Realtime;
    input->show_keypoints = controls.show_keypoints;
    input->show_headings = controls.show_headings;
    input->show_subject_mask_fills = controls.show_subject_masks;
    input->show_subject_mask_contours =
        controls.show_subject_masks && detailed_masks;
    input->independent_mask_contours = controls.independent_mask_contours;
    if (controls.independent_mask_contours) {
        input->show_subject_mask_contours =
            controls.show_subject_body_contour ||
            controls.show_eye_left_contour ||
            controls.show_eye_right_contour ||
            controls.show_swim_bladder_contour;
    }
    input->show_subject_body_contour = controls.show_subject_body_contour;
    input->show_eye_left_contour = controls.show_eye_left_contour;
    input->show_eye_right_contour = controls.show_eye_right_contour;
    input->show_swim_bladder_contour = controls.show_swim_bladder_contour;
    input->show_subject_body_mask = controls.show_subject_body_mask;
    input->show_eye_left_mask = controls.show_eye_left_mask;
    input->show_eye_right_mask = controls.show_eye_right_mask;
    input->show_swim_bladder_mask = controls.show_swim_bladder_mask;

    input->show_eye_geometry = controls.show_eye_geometry && detailed_masks;
    input->show_eye_direction_beams = controls.show_eye_direction_beams;
    input->show_eye_gaze_rays = controls.show_eye_gaze_rays;
    input->show_eye_angle_arcs = controls.show_eye_angle_arcs;
    input->show_eye_angle_labels = controls.show_eye_angle_labels;

    input->show_subject_shape = controls.show_subject_shape;
    input->show_subject_shape_body_axes =
        controls.show_subject_shape_body_axes;
    input->show_subject_shape_snout_tip =
        controls.show_subject_shape_snout_tip;
    input->show_subject_shape_caudal_anchor =
        controls.show_subject_shape_caudal_anchor;
    input->show_subject_shape_tail_base =
        controls.show_subject_shape_tail_base;
    input->show_subject_shape_tail_tip =
        controls.show_subject_shape_tail_tip;
    input->show_subject_shape_centerline =
        controls.show_subject_shape_centerline;
    input->show_subject_shape_bspline = controls.show_subject_shape_bspline;
    input->show_subject_shape_bspline_debug_points =
        controls.show_subject_shape_bspline_debug_points;
    input->show_subject_shape_bspline_control_points =
        controls.show_subject_shape_bspline_control_points;
    input->show_subject_shape_tail_samples =
        controls.show_subject_shape_tail_samples;
    input->show_subject_shape_tail_normals =
        controls.show_subject_shape_tail_normals;
}

}  // namespace crimson::overlay
