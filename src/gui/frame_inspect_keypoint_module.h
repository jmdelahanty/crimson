#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace crimson::gui {

enum class KeypointInspectTimelineState {
  Closed,
  Opening,
  Ready,
  Failed,
};

struct KeypointInspectLandmark {
  std::string label;
  float confidence = 0.0f;
  bool confidence_valid = false;
  bool valid = false;
  bool valid_known = false;
  bool edited = false;
  bool edited_known = false;
};

struct KeypointInspectObservation {
  uint64_t instance_key = 0;
  bool selectable = false;
  float pose_confidence = 0.0f;
  bool pose_confidence_valid = false;
  size_t valid_landmark_count = 0;
  size_t landmark_count = 0;
  std::string state_label;
  std::vector<KeypointInspectLandmark> landmarks;
};

struct KeypointInspectPresentation {
  bool available = false;
  std::string presentation_label = "Read-only presentation";
  std::string unavailable_message =
      "No raw or refined keypoint-v2 run is open.";
  std::string surface_label;
  std::string run_name;
  bool frame_ready = false;
  int64_t camera_frame = -1;
  std::vector<KeypointInspectObservation> observations;
  std::vector<std::string> detail_lines;
  bool timeline_visible = false;
  KeypointInspectTimelineState timeline_state =
      KeypointInspectTimelineState::Closed;
  std::string timeline_error;
};

struct KeypointInspectModuleState {
  uint64_t selected_instance_key = 0;
};

struct KeypointInspectModuleResult {
  bool request_open_timeline = false;
};

KeypointInspectModuleResult
drawFrameInspectKeypointModule(const KeypointInspectPresentation &presentation,
                               KeypointInspectModuleState &state);

} // namespace crimson::gui
