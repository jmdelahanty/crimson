#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace crimson::gui {

struct SubjectMaskInspectComponent {
  std::string label;
  size_t channel_index = 0;
  bool channel_index_valid = false;
  bool present = false;
  size_t pixel_payload_value_count = 0;
  bool pixel_payload_available = false;
  size_t contour_point_count = 0;
  bool contour_available = false;
};

struct SubjectMaskInspectObservation {
  uint64_t instance_key = 0;
  bool selectable = false;
  bool valid = false;
  int64_t source_crop_row_id = -1;
  bool source_crop_row_id_valid = false;
  double roi_width = 0.0;
  double roi_height = 0.0;
  bool roi_valid = false;
  bool axes_available = false;
  bool angle_labels_available = false;
  std::vector<SubjectMaskInspectComponent> components;
};

struct SubjectMaskInspectPresentation {
  bool available = false;
  std::string presentation_label = "Read-only presentation";
  std::string unavailable_message = "No subject-mask run is open.";
  std::string surface_label;
  std::string run_name;
  bool frame_ready = false;
  int64_t camera_frame = -1;
  std::vector<SubjectMaskInspectObservation> observations;
  std::vector<std::string> detail_lines;
  std::string warning;
};

struct SubjectMaskInspectModuleState {
  uint64_t selected_instance_key = 0;
};

void drawFrameInspectSubjectMaskModule(
    const SubjectMaskInspectPresentation &presentation,
    SubjectMaskInspectModuleState &state);

} // namespace crimson::gui
