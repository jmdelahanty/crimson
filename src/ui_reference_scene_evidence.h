#pragma once

#include "chaser_distance_polar_scene.h"
#include "stimulus_camera_overlay_scene.h"

#include <nlohmann/json.hpp>

namespace crimson::ui_reference {

struct SceneDisplayTransform {
  double origin_x = 0.0;
  double origin_y = 0.0;
  double scale_x = 1.0;
  double scale_y = 1.0;
};

nlohmann::json polarSceneEvidenceJson(
    const polar::ChaserDistancePolarScene &scene,
    const SceneDisplayTransform &display,
    const polar::ChaserDistancePolarDescriptor *descriptor = nullptr);

nlohmann::json stimulusCameraOverlaySceneEvidenceJson(
    const stimulus::StimulusCameraOverlayScene &scene,
    const SceneDisplayTransform &display,
    const timeline::StimulusContextTimelineDescriptor *descriptor = nullptr);

} // namespace crimson::ui_reference
