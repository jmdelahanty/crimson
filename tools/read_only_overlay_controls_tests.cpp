#include "read_only_overlay_controls.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                     \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__    \
                      << ": " #condition << '\n';                             \
            return false;                                                      \
        }                                                                      \
    } while (false)

crimson::overlay::ReadOnlyOverlayInput makeComponentInput() {
    using namespace crimson::overlay;
    ReadOnlyOverlayInput input;
    input.identity = {0, 12, 0, 12};
    input.source_width = 100.0;
    input.source_height = 100.0;
    const auto mask =
        std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{255});
    const char* labels[] = {"subject_body", "eye_left", "eye_right",
                            "swim_bladder", "unknown_component"};
    for (size_t index = 0; index < 5; ++index) {
        SubjectMaskComponentInput component;
        component.cache_namespace = "fixture";
        component.label = labels[index];
        component.source_crop_row_id = 4;
        component.channel_index = index;
        component.source_rect = {10.0, 10.0, 20.0, 20.0};
        component.mask_width = 1;
        component.mask_height = 1;
        component.mask = mask;
        component.contour = {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}};
        input.subject_masks.push_back(std::move(component));
    }

    EyeGeometryInput eyes;
    eyes.eye_row = 9;
    eyes.source_rect = {10.0, 10.0, 20.0, 20.0};
    eyes.coordinate_width = 20.0;
    eyes.coordinate_height = 20.0;
    eyes.frame_valid = true;
    for (size_t eye = 0; eye < 2; ++eye) {
        eyes.eyes[eye].valid = true;
        eyes.eyes[eye].major_axis = {
            true, {2.0, 4.0 + eye * 8.0}, {8.0, 4.0 + eye * 8.0}};
        eyes.eyes[eye].minor_axis = {
            true, {5.0, 2.0 + eye * 8.0}, {5.0, 6.0 + eye * 8.0}};
    }
    input.eye_geometry.push_back(std::move(eyes));
    return input;
}

bool hasLabelFragment(const crimson::overlay::ReadOnlyOverlayScene& scene,
                      const std::string& fragment) {
    return std::any_of(scene.primitives.begin(), scene.primitives.end(),
                       [&](const auto& primitive) {
                           return primitive.label.find(fragment) !=
                                  std::string::npos;
                       });
}

bool testModeAndDefaults() {
    using namespace crimson::overlay;
    ReadOnlyOverlayControlState controls;
    ReadOnlyOverlayInput input;
    applyReadOnlyOverlayControls(controls, &input);
    CHECK(input.show_keypoints);
    CHECK(input.show_headings);
    CHECK(input.show_subject_mask_fills);
    CHECK(input.show_subject_mask_contours);
    CHECK(input.show_eye_geometry);
    CHECK(input.show_subject_shape);
    CHECK(!input.show_subject_shape_body_axes);
    CHECK(std::string(readOnlyMaskOverlayModeName(controls.mask_mode)) ==
          "Review");

    controls.mask_mode = ReadOnlyMaskOverlayMode::Realtime;
    applyReadOnlyOverlayControls(controls, &input);
    CHECK(input.show_subject_mask_fills);
    CHECK(!input.show_subject_mask_contours);
    CHECK(!input.show_eye_geometry);

    controls.mask_mode = ReadOnlyMaskOverlayMode::Debug;
    controls.show_subject_masks = false;
    controls.show_eye_geometry = false;
    applyReadOnlyOverlayControls(controls, &input);
    CHECK(!input.show_subject_mask_fills);
    CHECK(!input.show_subject_mask_contours);
    CHECK(!input.show_eye_geometry);
    CHECK(std::string(readOnlyMaskOverlayModeName(controls.mask_mode)) ==
          "Debug");
    applyReadOnlyOverlayControls(controls, nullptr);
    return true;
}

bool testSemanticComponentVisibility() {
    using namespace crimson::overlay;
    auto input = makeComponentInput();
    ReadOnlyOverlayControlState controls;
    controls.show_subject_body_mask = false;
    controls.show_eye_left_mask = false;
    controls.show_eye_right_mask = true;
    controls.show_swim_bladder_mask = false;
    controls.show_eye_direction_beams = false;
    controls.show_eye_gaze_rays = false;
    controls.show_eye_angle_arcs = false;
    controls.show_eye_angle_labels = false;
    applyReadOnlyOverlayControls(controls, &input);
    const auto scene = buildReadOnlyOverlayScene(input);
    CHECK(scene.ready());
    CHECK(scene.raster_masks.size() == 2);
    CHECK(scene.raster_masks[0].label == "eye_right");
    CHECK(scene.raster_masks[1].label == "unknown_component");
    CHECK(!hasLabelFragment(scene, "##eye_major_9_0"));
    CHECK(!hasLabelFragment(scene, "##eye_minor_9_0"));
    CHECK(hasLabelFragment(scene, "##eye_major_9_1"));
    CHECK(hasLabelFragment(scene, "##eye_minor_9_1"));
    return true;
}

bool testShapeDetailMapping() {
    using namespace crimson::overlay;
    ReadOnlyOverlayControlState controls;
    controls.show_subject_shape = false;
    controls.show_subject_shape_body_axes = true;
    controls.show_subject_shape_snout_tip = false;
    controls.show_subject_shape_caudal_anchor = false;
    controls.show_subject_shape_tail_base = false;
    controls.show_subject_shape_tail_tip = false;
    controls.show_subject_shape_centerline = false;
    controls.show_subject_shape_bspline = false;
    controls.show_subject_shape_bspline_debug_points = true;
    controls.show_subject_shape_bspline_control_points = true;
    controls.show_subject_shape_tail_samples = true;
    controls.show_subject_shape_tail_normals = true;
    ReadOnlyOverlayInput input;
    applyReadOnlyOverlayControls(controls, &input);
    CHECK(!input.show_subject_shape);
    CHECK(input.show_subject_shape_body_axes);
    CHECK(!input.show_subject_shape_snout_tip);
    CHECK(!input.show_subject_shape_caudal_anchor);
    CHECK(!input.show_subject_shape_tail_base);
    CHECK(!input.show_subject_shape_tail_tip);
    CHECK(!input.show_subject_shape_centerline);
    CHECK(!input.show_subject_shape_bspline);
    CHECK(input.show_subject_shape_bspline_debug_points);
    CHECK(input.show_subject_shape_bspline_control_points);
    CHECK(input.show_subject_shape_tail_samples);
    CHECK(input.show_subject_shape_tail_normals);
    return true;
}

}  // namespace

int main() {
    if (!testModeAndDefaults() || !testSemanticComponentVisibility() ||
        !testShapeDetailMapping()) {
        return 1;
    }
    std::cout << "read_only_overlay_controls_tests: PASS\n";
    return 0;
}
