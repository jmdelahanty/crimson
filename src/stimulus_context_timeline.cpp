#include "stimulus_context_timeline.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <map>
#include <utility>

namespace crimson::timeline {
namespace {

std::string trim(std::string value) {
  const auto first = std::find_if(value.begin(), value.end(), [](char ch) {
    return !std::isspace(static_cast<unsigned char>(ch));
  });
  const auto last = std::find_if(value.rbegin(), value.rend(), [](char ch) {
                      return !std::isspace(static_cast<unsigned char>(ch));
                    }).base();
  return first < last ? std::string(first, last) : std::string{};
}

int64_t sortableFrame(int64_t frame) {
  return frame >= 0 ? frame : std::numeric_limits<int64_t>::max();
}

class OwnedStimulusContextTimelineRepository final
    : public StimulusContextTimelineRepository {
 public:
  explicit OwnedStimulusContextTimelineRepository(
      std::shared_ptr<const StimulusContextTimelineSnapshot> snapshot)
      : snapshot_(std::move(snapshot)) {}

  const StimulusContextTimelineDescriptor& descriptor() const override {
    return snapshot_->descriptor;
  }

  std::shared_ptr<const StimulusContextTimelineSnapshot> snapshot()
      const override {
    return snapshot_;
  }

 private:
  std::shared_ptr<const StimulusContextTimelineSnapshot> snapshot_;
};

}  // namespace

StimulusStepKind stimulusStepKind(const std::string& stimulus_mode) {
  if (stimulus_mode == "MOVING_GRATING") {
    return StimulusStepKind::MovingGrating;
  }
  if (stimulus_mode == "CONCENTRIC_GRATING") {
    return StimulusStepKind::ConcentricGrating;
  }
  if (stimulus_mode == "LOOMING_DOT") {
    return StimulusStepKind::LoomingDot;
  }
  if (stimulus_mode == "CHASER") {
    return StimulusStepKind::Chaser;
  }
  return StimulusStepKind::Other;
}

std::string buildStimulusEventLabel(const std::string& event_type_name,
                                    int32_t event_type_id,
                                    const std::string& name_or_context,
                                    const std::string& details_json,
                                    bool details_are_structured) {
  std::string label = trim(event_type_name);
  const std::string context = trim(name_or_context);
  if (label.empty()) {
    label = !context.empty() ? context
                             : "Event " + std::to_string(event_type_id);
  } else if (!context.empty() && context != label) {
    label += " - " + context;
  }

  std::string details = trim(details_json);
  if (!details.empty() && details != "{}" && details != "null" &&
      !details_are_structured) {
    if (details.size() > 96) {
      details.resize(93);
      details += "...";
    }
    label += " [" + details + "]";
  }
  return label;
}

const StimulusEventTypeDescriptor* findStimulusEventType(
    const StimulusContextTimelineDescriptor& descriptor,
    int32_t event_type_id) {
  const auto found = std::find_if(
      descriptor.event_types.begin(), descriptor.event_types.end(),
      [event_type_id](const auto& type) { return type.id == event_type_id; });
  return found != descriptor.event_types.end() ? &*found : nullptr;
}

const StimulusContextStep* findStimulusContextStepForFrame(
    const StimulusContextTimelineSnapshot& snapshot, int64_t camera_frame) {
  if (camera_frame < 0) {
    return nullptr;
  }
  // Adjacent steps may share a handoff frame. The maintained behavior gives
  // that frame to the later step.
  for (auto step = snapshot.steps.rbegin(); step != snapshot.steps.rend();
       ++step) {
    if (step->start_camera_frame >= 0 && step->end_camera_frame >= 0 &&
        camera_frame >= step->start_camera_frame &&
        camera_frame <= step->end_camera_frame) {
      return &*step;
    }
  }
  return nullptr;
}

std::vector<const StimulusContextEvent*> stimulusContextEventsForFrame(
    const StimulusContextTimelineSnapshot& snapshot, int64_t camera_frame) {
  std::vector<const StimulusContextEvent*> result;
  if (camera_frame < 0) {
    return result;
  }
  for (const auto& event : snapshot.events) {
    if (event.camera_frame == camera_frame) {
      result.push_back(&event);
    }
  }
  return result;
}

StimulusContextTimelineWindow stimulusContextTimelineWindow(
    const StimulusContextTimelineSnapshot& snapshot, int64_t first_frame,
    int64_t last_frame) {
  StimulusContextTimelineWindow result;
  result.first_frame = first_frame;
  result.last_frame = last_frame;
  if (!result.valid() ||
      (snapshot.descriptor.frame_count > 0 &&
       first_frame >= static_cast<int64_t>(snapshot.descriptor.frame_count))) {
    result.last_frame = -1;
    return result;
  }

  for (size_t index = 0; index < snapshot.events.size(); ++index) {
    const int64_t frame = snapshot.events[index].camera_frame;
    if (frame >= first_frame && frame <= last_frame) {
      result.event_indices.push_back(index);
    }
  }
  for (size_t index = 0; index < snapshot.steps.size(); ++index) {
    const auto& step = snapshot.steps[index];
    if (step.start_camera_frame < 0 || step.end_camera_frame < 0) {
      continue;
    }
    if (step.end_camera_frame >= first_frame &&
        step.start_camera_frame <= last_frame) {
      result.step_indices.push_back(index);
    }
  }
  return result;
}

double stimulusContextTimeForFrame(int64_t frame,
                                   double frames_per_second) {
  if (frame < 0) {
    return 0.0;
  }
  return frames_per_second > 0.0
             ? static_cast<double>(frame) / frames_per_second
             : static_cast<double>(frame);
}

int64_t stimulusContextNearestEventFrame(
    const StimulusContextTimelineSnapshot& snapshot, double time_seconds,
    double frames_per_second) {
  int64_t nearest_frame = -1;
  double nearest_distance = std::numeric_limits<double>::infinity();
  for (const auto& event : snapshot.events) {
    if (event.camera_frame < 0) {
      continue;
    }
    const double distance = std::fabs(
        stimulusContextTimeForFrame(event.camera_frame, frames_per_second) -
        time_seconds);
    if (distance < nearest_distance ||
        (distance == nearest_distance && event.camera_frame < nearest_frame)) {
      nearest_distance = distance;
      nearest_frame = event.camera_frame;
    }
  }
  return nearest_frame;
}

std::unique_ptr<StimulusContextTimelineRepository>
MakeStimulusContextTimelineRepository(
    StimulusContextTimelineDescriptor descriptor,
    std::vector<StimulusContextEvent> events,
    std::vector<StimulusContextStep> steps) {
  std::sort(steps.begin(), steps.end(), [](const auto& left, const auto& right) {
    if (sortableFrame(left.start_camera_frame) !=
        sortableFrame(right.start_camera_frame)) {
      return sortableFrame(left.start_camera_frame) <
             sortableFrame(right.start_camera_frame);
    }
    return left.step_index < right.step_index;
  });
  for (auto& step : steps) {
    step.kind = stimulusStepKind(step.stimulus_mode);
  }

  std::sort(events.begin(), events.end(), [](const auto& left,
                                             const auto& right) {
    if (sortableFrame(left.stimulus_frame) !=
        sortableFrame(right.stimulus_frame)) {
      return sortableFrame(left.stimulus_frame) <
             sortableFrame(right.stimulus_frame);
    }
    if (sortableFrame(left.camera_frame) != sortableFrame(right.camera_frame)) {
      return sortableFrame(left.camera_frame) <
             sortableFrame(right.camera_frame);
    }
    if (left.event_type_id != right.event_type_id) {
      return left.event_type_id < right.event_type_id;
    }
    return left.source_event_index < right.source_event_index;
  });

  std::map<int32_t, StimulusEventTypeDescriptor> event_types;
  for (const auto& type : descriptor.event_types) {
    event_types[type.id] = type;
    event_types[type.id].event_count = 0;
  }
  size_t frame_count = descriptor.frame_count;
  for (auto& event : events) {
    auto& type = event_types[event.event_type_id];
    type.id = event.event_type_id;
    if (type.display_name.empty()) {
      type.display_name = !event.event_type_name.empty()
                              ? event.event_type_name
                              : "Event " + std::to_string(event.event_type_id);
    }
    event.event_type_name = type.display_name;
    if (event.label.empty()) {
      event.label = buildStimulusEventLabel(
          event.event_type_name, event.event_type_id, event.name_or_context,
          event.details_json, false);
    }
    ++type.event_count;
    if (event.camera_frame >= 0) {
      frame_count = std::max(frame_count,
                             static_cast<size_t>(event.camera_frame + 1));
    }
  }
  for (const auto& step : steps) {
    if (step.end_camera_frame >= 0) {
      frame_count = std::max(frame_count,
                             static_cast<size_t>(step.end_camera_frame + 1));
    }
  }

  descriptor.frame_count = frame_count;
  descriptor.event_count = events.size();
  descriptor.step_count = steps.size();
  descriptor.event_types.clear();
  descriptor.event_types.reserve(event_types.size());
  for (auto& [id, type] : event_types) {
    if (type.event_count > 0) {
      descriptor.event_types.push_back(std::move(type));
    }
  }

  auto snapshot = std::make_shared<StimulusContextTimelineSnapshot>();
  snapshot->descriptor = std::move(descriptor);
  snapshot->events = std::move(events);
  snapshot->steps = std::move(steps);
  return std::make_unique<OwnedStimulusContextTimelineRepository>(
      std::move(snapshot));
}

}  // namespace crimson::timeline
