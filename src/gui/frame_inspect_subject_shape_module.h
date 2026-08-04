#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace crimson::gui {

struct SubjectShapeInspectFeature {
  std::string key;
  std::string label;
  bool valid = false;
  bool valid_known = false;
  size_t point_count = 0;
};

struct SubjectShapeInspectObservation {
  // UI-only row+1 key; this is not scientific observation identity.
  uint64_t row_selection_key = 0;
  bool selectable = false;
  size_t source_row = 0;
  bool source_row_valid = false;
  int64_t detection_index = -1;
  bool detection_index_valid = false;
  int64_t source_refined_row_id = -1;
  bool source_refined_row_id_valid = false;
  int64_t source_crop_row_id = -1;
  bool source_crop_row_id_valid = false;
  double roi_width = 0.0;
  double roi_height = 0.0;
  bool roi_valid = false;
  std::vector<SubjectShapeInspectFeature> features;
  std::vector<std::string> reasons;
};

struct SubjectShapeInspectPresentation {
  bool available = false;
  std::string presentation_label = "Subject shape presentation";
  std::string unavailable_message = "No subject-shape run is open.";
  std::string surface_label;
  std::string run_name;
  bool frame_ready = false;
  int64_t camera_frame = -1;
  std::vector<SubjectShapeInspectObservation> observations;
  std::vector<std::string> detail_lines;
  std::string warning;
};

struct SubjectShapeInspectModuleState {
  uint64_t selected_row_key = 0;
};

void drawFrameInspectSubjectShapeModule(
    const SubjectShapeInspectPresentation &presentation,
    SubjectShapeInspectModuleState &state);

} // namespace crimson::gui
