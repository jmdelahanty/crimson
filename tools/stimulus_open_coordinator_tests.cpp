#include "stimulus_open_coordinator.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition      \
                << '\n';                                                       \
      return false;                                                            \
    }                                                                          \
  } while (false)

using crimson::session::RecordingOpenWorkflowController;
using crimson::session::SessionLifecycle;
using crimson::session::SessionPhase;
using crimson::session::StimulusOpenCommand;
using crimson::session::StimulusOpenOperations;

bool testSuccessfulOpenCommitsResolvedMedia() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker progress;
  RecordingOpenWorkflowController workflow(lifecycle, progress);
  StimulusOpenCommand command;
  command.current_session.video_path = "camera.mp4";
  command.media.path = "stimulus.mp4";
  command.media.buffer_capacity = 18;
  command.media.schedule_initial_seek = true;
  command.media.initial_camera_frame = 41;

  bool opened = false;
  crimson::media::StimulusMediaOpenRequest observed_request;
  StimulusOpenOperations operations;
  operations.open_media =
      [&](const crimson::media::StimulusMediaOpenRequest &request) {
        opened = true;
        observed_request = request;
        crimson::media::StimulusMediaOpenResult result;
        result.ready = true;
        result.initial_seek_scheduled = true;
        result.path = "resolved-stimulus.mp4";
        return result;
      };

  const auto result =
      crimson::session::executeStimulusOpen(workflow, command, operations);
  CHECK(opened);
  CHECK(observed_request.buffer_capacity == 18);
  CHECK(observed_request.schedule_initial_seek);
  CHECK(observed_request.initial_camera_frame == 41);
  CHECK(result.ready);
  CHECK(result.generation > 0);
  CHECK(result.media.path == "resolved-stimulus.mp4");
  CHECK(result.media.initial_seek_scheduled);
  const auto snapshot = lifecycle.snapshot();
  CHECK(snapshot.phase == SessionPhase::Ready);
  CHECK(snapshot.active.video_path == "camera.mp4");
  CHECK(snapshot.active.stimulus_video_path == "resolved-stimulus.mp4");
  return true;
}

bool testOpenFailureFailsTransactionWithoutClearingPriorMedia() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker progress;
  RecordingOpenWorkflowController workflow(lifecycle, progress);
  int clears = 0;
  StimulusOpenOperations operations;
  operations.open_media =
      [](const crimson::media::StimulusMediaOpenRequest &request) {
        crimson::media::StimulusMediaOpenResult result;
        result.path = request.path;
        result.error = "decoder rejected media";
        return result;
      };
  operations.clear_media = [&]() { ++clears; };

  StimulusOpenCommand command;
  command.media.path = "invalid.mp4";
  const auto result =
      crimson::session::executeStimulusOpen(workflow, command, operations);
  CHECK(!result.ready);
  CHECK(result.media.error == "decoder rejected media");
  CHECK(clears == 0);
  CHECK(lifecycle.snapshot().phase == SessionPhase::Failed);
  return true;
}

bool testExceptionAfterOpenStartsFailsClosed() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker progress;
  RecordingOpenWorkflowController workflow(lifecycle, progress);
  int clears = 0;
  StimulusOpenOperations operations;
  operations.open_media = [](const crimson::media::StimulusMediaOpenRequest &)
      -> crimson::media::StimulusMediaOpenResult {
    throw std::runtime_error("decoder callback failed");
  };
  operations.clear_media = [&]() { ++clears; };

  StimulusOpenCommand command;
  command.media.path = "stimulus.mp4";
  const auto result =
      crimson::session::executeStimulusOpen(workflow, command, operations);
  CHECK(!result.ready);
  CHECK(result.media.error == "decoder callback failed");
  CHECK(clears == 1);
  CHECK(lifecycle.snapshot().phase == SessionPhase::Failed);
  return true;
}

bool testEmptyPathDoesNotBeginTransaction() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker progress;
  RecordingOpenWorkflowController workflow(lifecycle, progress);
  StimulusOpenOperations operations;
  operations.open_media = [](const crimson::media::StimulusMediaOpenRequest &) {
    return crimson::media::StimulusMediaOpenResult{};
  };

  const auto result = crimson::session::executeStimulusOpen(
      workflow, StimulusOpenCommand{}, operations);
  CHECK(!result.ready);
  CHECK(!result.media.error.empty());
  CHECK(lifecycle.snapshot().phase == SessionPhase::Empty);
  CHECK(!workflow.active());
  return true;
}

bool testCloseBeforeProbeAndAfterProbeNeverCommits() {
  for (const bool close_before_probe : {true, false}) {
    SessionLifecycle lifecycle;
    crimson::loading::LoadingProgressTracker progress;
    RecordingOpenWorkflowController workflow(lifecycle, progress);
    bool closed = close_before_probe;
    int probes = 0;
    StimulusOpenOperations operations;
    operations.opening_cancelled = [&] { return closed; };
    operations.open_media = [&](const crimson::media::StimulusMediaOpenRequest &request) {
      ++probes;
      closed = true;
      crimson::media::StimulusMediaOpenResult result;
      result.ready = true;
      result.path = request.path;
      return result;
    };
    StimulusOpenCommand command;
    command.media.path = "stimulus.mp4";
    const auto result = crimson::session::executeStimulusOpen(
        workflow, command, operations);
    CHECK(!result.ready);
    CHECK(result.media.error == "Session opening cancelled");
    CHECK(probes == (close_before_probe ? 0 : 1));
    CHECK(!workflow.active());
    CHECK(lifecycle.snapshot().phase != SessionPhase::Ready);
  }
  return true;
}

} // namespace

int main() {
  if (!testSuccessfulOpenCommitsResolvedMedia() ||
      !testOpenFailureFailsTransactionWithoutClearingPriorMedia() ||
      !testExceptionAfterOpenStartsFailsClosed() ||
      !testEmptyPathDoesNotBeginTransaction() ||
      !testCloseBeforeProbeAndAfterProbeNeverCommits()) {
    return EXIT_FAILURE;
  }
  std::cout << "stimulus_open_coordinator_tests: PASS\n";
  return EXIT_SUCCESS;
}
