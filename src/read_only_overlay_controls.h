#pragma once

#include "read_only_overlay_scene.h"

#include <cstdint>

namespace crimson::overlay {

enum class ReadOnlyMaskOverlayMode : uint8_t {
    Realtime = 0,
    Review = 1,
    Debug = 2,
};

const char* readOnlyMaskOverlayModeName(ReadOnlyMaskOverlayMode mode);

struct ReadOnlyOverlayControlState {
    bool show_keypoints = true;
    bool show_headings = true;

    bool show_subject_masks = true;
    bool show_subject_body_mask = true;
    bool show_eye_left_mask = true;
    bool show_eye_right_mask = true;
    bool show_swim_bladder_mask = true;
    ReadOnlyMaskOverlayMode mask_mode = ReadOnlyMaskOverlayMode::Review;

    bool show_eye_geometry = true;
    bool show_eye_direction_beams = true;
    bool show_eye_gaze_rays = true;
    bool show_eye_angle_arcs = true;
    bool show_eye_angle_labels = true;

    bool show_subject_shape = true;
    bool show_subject_shape_body_axes = false;
    bool show_subject_shape_snout_tip = true;
    bool show_subject_shape_caudal_anchor = true;
    bool show_subject_shape_tail_base = true;
    bool show_subject_shape_tail_tip = true;
    bool show_subject_shape_centerline = true;
    bool show_subject_shape_bspline = true;
    bool show_subject_shape_bspline_debug_points = false;
    bool show_subject_shape_bspline_control_points = false;
    bool show_subject_shape_tail_samples = false;
    bool show_subject_shape_tail_normals = false;
};

struct ReadOnlyOverlayAvailability {
    bool keypoints = false;
    bool headings = false;
    bool subject_masks = false;
    bool eye_geometry = false;
    bool subject_shape = false;
};

void applyReadOnlyOverlayControls(const ReadOnlyOverlayControlState& controls,
                                  ReadOnlyOverlayInput* input);

}  // namespace crimson::overlay
