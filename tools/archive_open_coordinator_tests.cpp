#include "archive_open_coordinator.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition      \
                << '\n';                                                       \
      return false;                                                            \
    }                                                                          \
  } while (false)

using crimson::session::ArchiveOpenCommand;
using crimson::session::ArchiveOpenOperations;
using crimson::session::RecordingOpenWorkflowController;
using crimson::session::SessionLifecycle;
using crimson::session::SessionPhase;

bool testSuccessfulArchiveOpen() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker progress;
  RecordingOpenWorkflowController workflow(lifecycle, progress);
  std::vector<std::string> operations;
  ArchiveOpenCommand command;
  command.current_session.video_path = "camera.mp4";
  command.selected_path = "selected.zarr";
  command.current_frame = 73;

  ArchiveOpenOperations adapter;
  adapter.open_archive = [&](const std::string &path, std::string &resolved,
                             std::string &) {
    operations.push_back("open:" + path);
    resolved = "resolved.zarr";
    return true;
  };
  adapter.adopt_archive = [&](int64_t frame) {
    operations.push_back("adopt:" + std::to_string(frame));
  };
  adapter.clear_archive = [&]() { operations.push_back("clear"); };
  adapter.resolve_affiliated_media = [&]() { operations.push_back("media"); };
  adapter.resolve_stimulus_media = [&]() { operations.push_back("stimulus"); };
  adapter.active_recording_clip_index_path = [&]() {
    operations.push_back("clip-index");
    return std::string("recording_clip_index.json");
  };

  const auto result =
      crimson::session::executeArchiveOpen(workflow, command, adapter);
  CHECK(result.ready);
  CHECK(result.error.empty());
  CHECK(result.resolved_path == "resolved.zarr");
  CHECK(result.generation > 0);
  CHECK(operations ==
        std::vector<std::string>({"open:selected.zarr", "adopt:73", "media",
                                  "stimulus", "clip-index"}));
  const auto snapshot = lifecycle.snapshot();
  CHECK(snapshot.phase == SessionPhase::Ready);
  CHECK(snapshot.active.video_path == "camera.mp4");
  CHECK(snapshot.active.zarr_path == "resolved.zarr");
  CHECK(snapshot.active.recording_clip_index_path ==
        "recording_clip_index.json");
  return true;
}

bool testOpenFailureClearsArchiveState() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker progress;
  RecordingOpenWorkflowController workflow(lifecycle, progress);
  int clears = 0;
  ArchiveOpenOperations adapter;
  adapter.open_archive = [](const std::string &, std::string &,
                            std::string &error) {
    error = "manifest rejected";
    return false;
  };
  adapter.clear_archive = [&]() { ++clears; };

  ArchiveOpenCommand command;
  command.selected_path = "invalid.zarr";
  const auto result =
      crimson::session::executeArchiveOpen(workflow, command, adapter);
  CHECK(!result.ready);
  CHECK(result.error == "manifest rejected");
  CHECK(clears == 1);
  CHECK(lifecycle.snapshot().phase == SessionPhase::Failed);
  CHECK(progress.snapshot().terminal());
  return true;
}

bool testAdoptionExceptionFailsClosed() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker progress;
  RecordingOpenWorkflowController workflow(lifecycle, progress);
  int clears = 0;
  ArchiveOpenOperations adapter;
  adapter.open_archive = [](const std::string &, std::string &resolved,
                            std::string &) {
    resolved = "opened.zarr";
    return true;
  };
  adapter.adopt_archive = [](int64_t) {
    throw std::runtime_error("cache adoption failed");
  };
  adapter.clear_archive = [&]() { ++clears; };

  ArchiveOpenCommand command;
  command.selected_path = "selected.zarr";
  const auto result =
      crimson::session::executeArchiveOpen(workflow, command, adapter);
  CHECK(!result.ready);
  CHECK(result.error == "cache adoption failed");
  CHECK(clears == 1);
  CHECK(lifecycle.snapshot().phase == SessionPhase::Failed);
  return true;
}

bool testInvalidCommandDoesNotBeginTransaction() {
  SessionLifecycle lifecycle;
  crimson::loading::LoadingProgressTracker progress;
  RecordingOpenWorkflowController workflow(lifecycle, progress);
  ArchiveOpenOperations adapter;
  adapter.open_archive = [](const std::string &, std::string &, std::string &) {
    return true;
  };

  const auto result = crimson::session::executeArchiveOpen(
      workflow, ArchiveOpenCommand{}, adapter);
  CHECK(!result.ready);
  CHECK(!result.error.empty());
  CHECK(lifecycle.snapshot().phase == SessionPhase::Empty);
  CHECK(!workflow.active());
  return true;
}

} // namespace

int main() {
  if (!testSuccessfulArchiveOpen() || !testOpenFailureClearsArchiveState() ||
      !testAdoptionExceptionFailsClosed() ||
      !testInvalidCommandDoesNotBeginTransaction()) {
    return EXIT_FAILURE;
  }
  std::cout << "archive_open_coordinator_tests: PASS\n";
  return EXIT_SUCCESS;
}
