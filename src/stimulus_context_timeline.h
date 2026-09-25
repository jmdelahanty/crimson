#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace crimson::timeline {

enum class StimulusStepKind : uint8_t {
  Other,
  MovingGrating,
  ConcentricGrating,
  LoomingDot,
  Chaser,
};

struct StimulusEventTypeDescriptor {
  int32_t id = -1;
  std::string display_name;
  size_t event_count = 0;
};

struct StimulusMovingGratingContext {
  bool present = false;
  double grating_direction_camera_deg =
      std::numeric_limits<double>::quiet_NaN();
  double orientation_degrees_authored =
      std::numeric_limits<double>::quiet_NaN();
  double camera_to_projector_offset_deg =
      std::numeric_limits<double>::quiet_NaN();
  std::string direction_mapping_status;
  bool direction_mapping_validated = false;
  bool has_direction_mapping_validated = false;
  double speed_mm_s = std::numeric_limits<double>::quiet_NaN();
  double temporal_frequency_hz = std::numeric_limits<double>::quiet_NaN();
};

struct StimulusConcentricGratingContext {
  bool present = false;
  std::string stimulus_role;
  std::string radial_polarity_authored;
  double radial_sign_authored = std::numeric_limits<double>::quiet_NaN();
  bool radial_polarity_validated = false;
  bool has_radial_polarity_validated = false;
  double center_x_px = std::numeric_limits<double>::quiet_NaN();
  double center_y_px = std::numeric_limits<double>::quiet_NaN();
  double center_x_mm = std::numeric_limits<double>::quiet_NaN();
  double center_y_mm = std::numeric_limits<double>::quiet_NaN();
  double target_radius_min_mm = std::numeric_limits<double>::quiet_NaN();
  double target_radius_max_mm = std::numeric_limits<double>::quiet_NaN();
  double speed_mm_s = std::numeric_limits<double>::quiet_NaN();
  double temporal_frequency_hz = std::numeric_limits<double>::quiet_NaN();
};

struct StimulusContextStep {
  int32_t step_index = -1;
  std::string step_name;
  int32_t stimulus_mode_id = -1;
  std::string stimulus_mode;
  StimulusStepKind kind = StimulusStepKind::Other;
  int64_t start_camera_frame = -1;
  int64_t end_camera_frame = -1;
  double duration_s = std::numeric_limits<double>::quiet_NaN();
  std::string raw_protocol_params_json;
  StimulusMovingGratingContext moving_grating;
  StimulusConcentricGratingContext concentric_grating;
};

struct StimulusContextEvent {
  size_t source_event_index = 0;
  int64_t stimulus_frame = -1;
  int64_t camera_frame = -1;
  int64_t timestamp_ns_session = 0;
  int32_t event_type_id = -1;
  std::string event_type_name;
  std::string name_or_context;
  std::string details_json;
  std::string label;
};

struct StimulusContextTimelineDescriptor {
  std::string run_name;
  size_t frame_count = 0;
  size_t event_count = 0;
  size_t step_count = 0;
  std::vector<StimulusEventTypeDescriptor> event_types;
};

struct StimulusContextTimelineSnapshot {
  StimulusContextTimelineDescriptor descriptor;
  std::vector<StimulusContextEvent> events;
  std::vector<StimulusContextStep> steps;
};

struct StimulusContextTimelineWindow {
  int64_t first_frame = 0;
  int64_t last_frame = -1;
  std::vector<size_t> event_indices;
  std::vector<size_t> step_indices;

  bool valid() const { return first_frame >= 0 && last_frame >= first_frame; }
};

class StimulusContextTimelineRepository {
 public:
  virtual ~StimulusContextTimelineRepository() = default;
  virtual const StimulusContextTimelineDescriptor& descriptor() const = 0;
  virtual std::shared_ptr<const StimulusContextTimelineSnapshot> snapshot()
      const = 0;
};

StimulusStepKind stimulusStepKind(const std::string& stimulus_mode);

std::string buildStimulusEventLabel(const std::string& event_type_name,
                                    int32_t event_type_id,
                                    const std::string& name_or_context,
                                    const std::string& details_json,
                                    bool details_are_structured = false);

const StimulusEventTypeDescriptor* findStimulusEventType(
    const StimulusContextTimelineDescriptor& descriptor, int32_t event_type_id);

const StimulusContextStep* findStimulusContextStepForFrame(
    const StimulusContextTimelineSnapshot& snapshot, int64_t camera_frame);

std::vector<const StimulusContextEvent*> stimulusContextEventsForFrame(
    const StimulusContextTimelineSnapshot& snapshot, int64_t camera_frame);

StimulusContextTimelineWindow stimulusContextTimelineWindow(
    const StimulusContextTimelineSnapshot& snapshot, int64_t first_frame,
    int64_t last_frame);

double stimulusContextTimeForFrame(int64_t frame,
                                   double frames_per_second);

int64_t stimulusContextNearestEventFrame(
    const StimulusContextTimelineSnapshot& snapshot, double time_seconds,
    double frames_per_second);

std::unique_ptr<StimulusContextTimelineRepository>
MakeStimulusContextTimelineRepository(
    StimulusContextTimelineDescriptor descriptor,
    std::vector<StimulusContextEvent> events,
    std::vector<StimulusContextStep> steps);

}  // namespace crimson::timeline
