#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace crimson::gui {

enum class DetectionInspectTimelineState {
  Closed,
  Opening,
  Ready,
  Failed,
};

struct DetectionInspectObservation {
  uint64_t instance_key = 0;
  bool selectable = false;
  float confidence = 0.0f;
  bool confidence_valid = false;
  int32_t class_id = 0;
  bool class_id_valid = false;
  std::string source_label;
};

struct DetectionInspectPresentation {
  bool available = false;
  std::string presentation_label = "Read-only presentation";
  std::string unavailable_message =
      "No canonical or refined detection run is open.";
  std::string surface_label;
  std::string run_name;
  bool frame_ready = false;
  int64_t camera_frame = -1;
  std::vector<DetectionInspectObservation> observations;
  std::vector<std::string> detail_lines;
  bool timeline_visible = false;
  DetectionInspectTimelineState timeline_state =
      DetectionInspectTimelineState::Closed;
  std::string timeline_error;
};

struct DetectionInspectModuleState {
  uint64_t selected_instance_key = 0;
};

struct DetectionInspectModuleResult {
  bool request_open_timeline = false;
};

DetectionInspectModuleResult drawFrameInspectDetectionModule(
    const DetectionInspectPresentation &presentation,
    DetectionInspectModuleState &state);

} // namespace crimson::gui
