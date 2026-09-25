#include "ui_reference_scene_evidence.h"

#include <cmath>

namespace crimson::ui_reference {

namespace {

bool validDisplayTransform(const SceneDisplayTransform &display) {
  return std::isfinite(display.origin_x) && std::isfinite(display.origin_y) &&
         std::isfinite(display.scale_x) && std::isfinite(display.scale_y) &&
         display.scale_x > 0.0 && display.scale_y > 0.0;
}

} // namespace

nlohmann::json
polarSceneEvidenceJson(const polar::ChaserDistancePolarScene &scene,
                       const SceneDisplayTransform &display,
                       const polar::ChaserDistancePolarDescriptor *descriptor) {
  nlohmann::json result = {
      {"ready", scene.ready()},
      {"status", polar::chaserDistancePolarSceneStatusName(scene.status)},
      {"availability",
       polar::chaserDistancePolarAvailabilityName(scene.frame_availability)},
      {"requested_frame", scene.requested_camera_frame},
      {"source_frame", scene.source_camera_frame
                           ? nlohmann::json(*scene.source_camera_frame)
                           : nlohmann::json(nullptr)},
      {"point_count", scene.point_count},
      {"primitive_count", scene.primitives.size()},
      {"text_count", scene.text.size()},
      {"semantic_signature",
       polar::chaserDistancePolarSceneSemanticSignature(scene)},
  };
  if (descriptor != nullptr) {
    result["descriptor"] = {
        {"availability",
         polar::chaserDistancePolarAvailabilityName(descriptor->availability)},
        {"source_group", descriptor->provenance.source_group},
        {"run_name", descriptor->provenance.run_name},
        {"component_name", descriptor->provenance.component_name},
        {"run_selection", polar::chaserDistancePolarSelectionProvenanceName(
                              descriptor->provenance.run_selection)},
        {"component_selection",
         polar::chaserDistancePolarSelectionProvenanceName(
             descriptor->provenance.component_selection)},
        {"row_count", descriptor->row_count},
        {"chaser_count", descriptor->chaser_count},
        {"distance_unit",
         polar::chaserDistancePolarDistanceUnitName(descriptor->distance_unit)},
        {"coordinate_frame", descriptor->coordinate_frame},
        {"angle_convention", descriptor->angle_convention},
        {"normalized_coordinate_frame",
         static_cast<int>(descriptor->normalized_coordinate_frame)},
        {"normalized_angle_convention",
         static_cast<int>(descriptor->normalized_angle_convention)},
        {"dataset_global_max_distance_mm",
         descriptor->dataset_global_max_distance_mm},
        {"display_max_distance_mm",
         descriptor->radial_scale.display_max_distance_mm},
    };
  }
  if (!scene.ready() || !validDisplayTransform(display)) {
    return result;
  }

  auto pointJson = [&](polar::ChaserDistancePolarScenePoint point) {
    return nlohmann::json{{"x", point.x - scene.box.x},
                          {"y", point.y - scene.box.y}};
  };
  auto colorJson = [](const polar::ChaserDistancePolarRgba &color) {
    return nlohmann::json::array(
        {color.red, color.green, color.blue, color.alpha});
  };
  result["viewport"] = {{"width", scene.viewport.width_px},
                        {"height", scene.viewport.height_px}};
  result["box"] = {{"x", scene.box.x},
                   {"y", scene.box.y},
                   {"width", scene.box.width},
                   {"height", scene.box.height}};
  result["screen_box"] = {
      {"x", display.origin_x + scene.box.x * display.scale_x},
      {"y", display.origin_y + scene.box.y * display.scale_y},
      {"width", scene.box.width * display.scale_x},
      {"height", scene.box.height * display.scale_y},
  };
  result["graph"] = {
      {"x", scene.graph.x - scene.box.x},
      {"y", scene.graph.y - scene.box.y},
      {"width", scene.graph.width},
      {"height", scene.graph.height},
  };
  result["center"] = pointJson(scene.center);
  result["radius_px"] = scene.radius_px;
  result["display_max_distance_mm"] = scene.display_max_distance_mm;

  result["points"] = nlohmann::json::array();
  for (const auto &primitive : scene.primitives) {
    if (primitive.type !=
        polar::ChaserDistancePolarScenePrimitiveType::Marker) {
      continue;
    }
    result["points"].push_back({
        {"chaser_index", primitive.chaser_index},
        {"center", pointJson(primitive.first)},
        {"screen_center",
         {{"x", display.origin_x + primitive.first.x * display.scale_x},
          {"y", display.origin_y + primitive.first.y * display.scale_y}}},
        {"radius_px", primitive.radius_px},
        {"fill", colorJson(primitive.fill)},
        {"stroke", colorJson(primitive.stroke)},
    });
  }
  result["text"] = nlohmann::json::array();
  for (const auto &annotation : scene.text) {
    result["text"].push_back({
        {"layer", polar::chaserDistancePolarSceneLayerName(annotation.layer)},
        {"anchor", pointJson(annotation.anchor)},
        {"centered", annotation.centered},
        {"color", colorJson(annotation.color)},
        {"content", annotation.content},
    });
  }
  return result;
}

nlohmann::json stimulusCameraOverlaySceneEvidenceJson(
    const stimulus::StimulusCameraOverlayScene &scene,
    const SceneDisplayTransform &display,
    const timeline::StimulusContextTimelineDescriptor *descriptor) {
  nlohmann::json result = {
      {"ready", scene.ready()},
      {"status", stimulus::stimulusCameraOverlaySceneStatusName(scene.status)},
      {"availability", stimulus::stimulusCameraOverlayFrameAvailabilityName(
                           scene.frame_availability)},
      {"requested_frame", scene.requested_camera_frame},
      {"source_frame", scene.source_camera_frame
                           ? nlohmann::json(*scene.source_camera_frame)
                           : nlohmann::json(nullptr)},
      {"event_source_frame",
       scene.event_source_camera_frame
           ? nlohmann::json(*scene.event_source_camera_frame)
           : nlohmann::json(nullptr)},
      {"step_index", scene.step_index ? nlohmann::json(*scene.step_index)
                                      : nlohmann::json(nullptr)},
      {"grating_direction_camera_deg",
       scene.grating_direction_camera_deg
           ? nlohmann::json(*scene.grating_direction_camera_deg)
           : nlohmann::json(nullptr)},
      {"primitive_count", scene.primitives.size()},
      {"text_count", scene.text.size()},
      {"event_box",
       {{"x", scene.event_box.x},
        {"y", scene.event_box.y},
        {"width", scene.event_box.width},
        {"height", scene.event_box.height}}},
      {"step_box",
       {{"x", scene.step_box.x},
        {"y", scene.step_box.y},
        {"width", scene.step_box.width},
        {"height", scene.step_box.height}}},
      {"viewport",
       {{"width", scene.viewport.width_px},
        {"height", scene.viewport.height_px}}},
      {"display_origin", {{"x", display.origin_x}, {"y", display.origin_y}}},
      {"display_scale", {{"x", display.scale_x}, {"y", display.scale_y}}},
      {"semantic_signature",
       stimulus::stimulusCameraOverlaySceneSemanticSignature(scene)},
      {"screen_event_box",
       {{"x", display.origin_x + scene.event_box.x * display.scale_x},
        {"y", display.origin_y + scene.event_box.y * display.scale_y},
        {"width", scene.event_box.width * display.scale_x},
        {"height", scene.event_box.height * display.scale_y}}},
      {"screen_step_box",
       {{"x", display.origin_x + scene.step_box.x * display.scale_x},
        {"y", display.origin_y + scene.step_box.y * display.scale_y},
        {"width", scene.step_box.width * display.scale_x},
        {"height", scene.step_box.height * display.scale_y}}},
      {"descriptor",
       descriptor != nullptr
           ? nlohmann::json{{"run_name", descriptor->run_name},
                            {"frame_count", descriptor->frame_count},
                            {"event_count", descriptor->event_count},
                            {"step_count", descriptor->step_count}}
           : nlohmann::json(nullptr)}};
  auto colorJson = [](const auto &color) {
    return nlohmann::json::array(
        {color.red, color.green, color.blue, color.alpha});
  };
  result["primitives"] = nlohmann::json::array();
  for (const auto &primitive : scene.primitives) {
    result["primitives"].push_back({
        {"type", static_cast<int>(primitive.type)},
        {"layer",
         stimulus::stimulusCameraOverlaySceneLayerName(primitive.layer)},
        {"first", {{"x", primitive.first.x}, {"y", primitive.first.y}}},
        {"second", {{"x", primitive.second.x}, {"y", primitive.second.y}}},
        {"third", {{"x", primitive.third.x}, {"y", primitive.third.y}}},
        {"corner_radius_px", primitive.corner_radius_px},
        {"stroke_width_px", primitive.stroke_width_px},
        {"fill", colorJson(primitive.fill)},
        {"stroke", colorJson(primitive.stroke)},
        {"has_fill", primitive.has_fill},
        {"has_stroke", primitive.has_stroke},
    });
  }
  result["text"] = nlohmann::json::array();
  for (const auto &annotation : scene.text) {
    result["text"].push_back({
        {"layer",
         stimulus::stimulusCameraOverlaySceneLayerName(annotation.layer)},
        {"anchor", {{"x", annotation.anchor.x}, {"y", annotation.anchor.y}}},
        {"color", colorJson(annotation.color)},
        {"content", annotation.content},
    });
  }
  return result;
}

} // namespace crimson::ui_reference
