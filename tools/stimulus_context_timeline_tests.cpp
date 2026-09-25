#include "stimulus_context_timeline.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                    \
      return false;                                                            \
    }                                                                          \
  } while (false)

std::unique_ptr<crimson::timeline::StimulusContextTimelineRepository>
makeRepository() {
  using namespace crimson::timeline;
  StimulusContextTimelineDescriptor descriptor;
  descriptor.run_name = "stimulus_fixture";
  descriptor.frame_count = 50;
  descriptor.event_types = {{11, "Step Start", 0}, {56, "Chase", 0}};

  std::vector<StimulusContextEvent> events = {
      {2, 200, 20, 2000, 56, {}, "target", "{}", {}},
      {0, 100, 10, 1000, 11, {}, "step", "{}", {}},
      {1, 101, 10, 1100, 11, {}, "step duplicate", "plain", {}},
      {3, 300, -1, 3000, 99, {}, "unmapped", "{}", {}},
  };

  StimulusContextStep first;
  first.step_index = 0;
  first.step_name = "Moving";
  first.stimulus_mode = "MOVING_GRATING";
  first.start_camera_frame = 5;
  first.end_camera_frame = 15;
  first.duration_s = 0.11;
  first.moving_grating.present = true;
  first.moving_grating.grating_direction_camera_deg = 90.0;

  StimulusContextStep second;
  second.step_index = 1;
  second.step_name = "Chaser";
  second.stimulus_mode = "CHASER";
  second.start_camera_frame = 15;
  second.end_camera_frame = 30;
  second.duration_s = 0.16;

  return MakeStimulusContextTimelineRepository(
      std::move(descriptor), std::move(events),
      {std::move(second), std::move(first)});
}

bool testDescriptorAndMaintainedOrdering() {
  using namespace crimson::timeline;
  auto repository = makeRepository();
  CHECK(repository != nullptr);
  const auto& descriptor = repository->descriptor();
  CHECK(descriptor.run_name == "stimulus_fixture");
  CHECK(descriptor.frame_count == 50);
  CHECK(descriptor.event_count == 4);
  CHECK(descriptor.step_count == 2);
  CHECK(descriptor.event_types.size() == 3);
  CHECK(findStimulusEventType(descriptor, 11)->event_count == 2);
  CHECK(findStimulusEventType(descriptor, 99)->display_name == "Event 99");

  const auto snapshot = repository->snapshot();
  CHECK(snapshot != nullptr);
  CHECK(snapshot->events.front().source_event_index == 0);
  CHECK(snapshot->events[1].source_event_index == 1);
  CHECK(snapshot->events.back().camera_frame == -1);
  CHECK(snapshot->steps.front().step_index == 0);
  CHECK(snapshot->steps.front().kind == StimulusStepKind::MovingGrating);
  CHECK(snapshot->steps.back().kind == StimulusStepKind::Chaser);
  CHECK(snapshot == repository->snapshot());
  return true;
}

bool testFrameResolutionAndHandoffPrecedence() {
  using namespace crimson::timeline;
  const auto snapshot = makeRepository()->snapshot();
  CHECK(findStimulusContextStepForFrame(*snapshot, 4) == nullptr);
  CHECK(findStimulusContextStepForFrame(*snapshot, 14)->step_index == 0);
  CHECK(findStimulusContextStepForFrame(*snapshot, 15)->step_index == 1);
  CHECK(findStimulusContextStepForFrame(*snapshot, 30)->step_index == 1);
  CHECK(findStimulusContextStepForFrame(*snapshot, 31) == nullptr);

  const auto frame_events = stimulusContextEventsForFrame(*snapshot, 10);
  CHECK(frame_events.size() == 2);
  CHECK(frame_events.front()->event_type_id == 11);
  CHECK(frame_events.back()->label == "Step Start - step duplicate [plain]");

  const auto window = stimulusContextTimelineWindow(*snapshot, 12, 22);
  CHECK(window.valid());
  CHECK(window.event_indices.size() == 1);
  CHECK(window.step_indices.size() == 2);
  CHECK(!stimulusContextTimelineWindow(*snapshot, -1, 10).valid());
  CHECK(!stimulusContextTimelineWindow(*snapshot, 60, 70).valid());
  return true;
}

bool testTimeAndExactEventMapping() {
  using namespace crimson::timeline;
  const auto snapshot = makeRepository()->snapshot();
  CHECK(std::fabs(stimulusContextTimeForFrame(20, 100.0) - 0.2) < 1e-12);
  CHECK(stimulusContextTimeForFrame(20, 0.0) == 20.0);
  CHECK(stimulusContextNearestEventFrame(*snapshot, 0.19, 100.0) == 20);
  CHECK(stimulusContextNearestEventFrame(*snapshot, 0.1, 100.0) == 10);
  CHECK(stimulusContextNearestEventFrame(*snapshot, 0.15, 100.0) == 10);
  return true;
}

bool testLabelsAndKinds() {
  using namespace crimson::timeline;
  CHECK(buildStimulusEventLabel("", 7, "context", "{}") == "context");
  CHECK(buildStimulusEventLabel("Start", 7, "Start", "null") == "Start");
  CHECK(buildStimulusEventLabel("Start", 7, "trial", "ignored", true) ==
        "Start - trial");
  CHECK(stimulusStepKind("CONCENTRIC_GRATING") ==
        StimulusStepKind::ConcentricGrating);
  CHECK(stimulusStepKind("LOOMING_DOT") == StimulusStepKind::LoomingDot);
  CHECK(stimulusStepKind("unknown") == StimulusStepKind::Other);
  return true;
}

}  // namespace

int main() {
  if (!testDescriptorAndMaintainedOrdering() ||
      !testFrameResolutionAndHandoffPrecedence() ||
      !testTimeAndExactEventMapping() || !testLabelsAndKinds()) {
    return 1;
  }
  std::cout << "stimulus_context_timeline_tests: PASS\n";
  return 0;
}
