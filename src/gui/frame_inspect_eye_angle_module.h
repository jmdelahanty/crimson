#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace crimson::gui {

enum class EyeAngleInspectFieldKind : uint8_t {
  Scalar,
  Vector2,
};

struct EyeAngleInspectRepresentation {
  std::string key;
  std::string label;
  std::string role;
  std::string axis;
};

struct EyeAngleInspectField {
  std::string representation_key;
  std::string label;
  std::string units;
  EyeAngleInspectFieldKind kind = EyeAngleInspectFieldKind::Scalar;
  double value_x = 0.0;
  double value_y = 0.0;
  bool valid = false;
};

struct EyeAngleInspectObservation {
  // UI-only row+1 key; this is not scientific observation identity.
  uint64_t row_selection_key = 0;
  bool selectable = false;
  size_t source_row = 0;
  bool source_row_valid = false;
  int64_t detection_index = -1;
  bool detection_index_valid = false;
  int64_t source_crop_row_id = -1;
  bool source_crop_row_id_valid = false;
  bool frame_valid = false;
  bool frame_valid_known = false;
  bool left_valid = false;
  bool left_valid_known = false;
  bool right_valid = false;
  bool right_valid_known = false;
  bool marginal = false;
  bool marginal_known = false;
  std::string reason;
  std::vector<EyeAngleInspectField> fields;
};

struct EyeAngleInspectPresentation {
  bool available = false;
  std::string presentation_label = "Read-only presentation";
  std::string unavailable_message = "No eye-angle run is open.";
  std::string surface_label;
  std::string run_name;
  std::string default_representation_key;
  std::vector<EyeAngleInspectRepresentation> representations;
  bool frame_ready = false;
  int64_t camera_frame = -1;
  std::vector<EyeAngleInspectObservation> observations;
  std::vector<std::string> detail_lines;
  std::string warning;
};

struct EyeAngleInspectModuleState {
  uint64_t selected_row_key = 0;
  std::string selected_representation_key;
};

void drawFrameInspectEyeAngleModule(
    const EyeAngleInspectPresentation &presentation,
    EyeAngleInspectModuleState &state);

} // namespace crimson::gui
