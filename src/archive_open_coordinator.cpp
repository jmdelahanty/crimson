#include "archive_open_coordinator.h"

#include <exception>
#include <utility>

namespace crimson::session {
namespace {

void clearArchive(const ArchiveOpenOperations &operations) {
  if (operations.clear_archive) {
    operations.clear_archive();
  }
}

ArchiveOpenResult failArchiveOpen(RecordingOpenWorkflowController &workflow,
                                  const ArchiveOpenOperations &operations,
                                  ArchiveOpenResult result, std::string error) {
  clearArchive(operations);
  result.error = std::move(error);
  if (workflow.active()) {
    workflow.failProduct("archive", "Archive unavailable", result.error,
                         "Archive unavailable");
  }
  return result;
}

} // namespace

ArchiveOpenResult executeArchiveOpen(RecordingOpenWorkflowController &workflow,
                                     const ArchiveOpenCommand &command,
                                     const ArchiveOpenOperations &operations) {
  ArchiveOpenResult result;
  if (command.selected_path.empty()) {
    result.error = "No Zarr archive path was selected";
    return result;
  }
  if (!operations.open_archive) {
    result.error = "Archive open operation is unavailable";
    return result;
  }

  SessionDescriptor requested_session = command.current_session;
  requested_session.zarr_path = command.selected_path;
  std::string begin_error;
  if (!workflow.begin({requested_session,
                       "Opening Zarr archive",
                       {{"archive", ProductAvailabilityRequirement::Required}}},
                      {}, &begin_error)) {
    result.error = begin_error.empty() ? "Archive open could not begin"
                                       : std::move(begin_error);
    return result;
  }
  result.generation = workflow.generation();
  auto stopIfCancelled = [&]() {
    if (!operations.opening_cancelled || !operations.opening_cancelled()) {
      return false;
    }
    clearArchive(operations);
    workflow.cancel("Session opening cancelled");
    result.error = "Session opening cancelled";
    return true;
  };
  if (stopIfCancelled()) {
    return result;
  }
  if (!workflow.startProduct("archive", "Resolving archive")) {
    return failArchiveOpen(workflow, operations, std::move(result),
                           "Archive loading could not start");
  }

  try {
    std::string open_error;
    const bool open_ready = operations.open_archive(
        command.selected_path, result.resolved_path, open_error);
    if (stopIfCancelled()) {
      return result;
    }
    if (!open_ready) {
      if (open_error.empty()) {
        open_error = "Selected Zarr archive could not be opened";
      }
      return failArchiveOpen(workflow, operations, std::move(result),
                             std::move(open_error));
    }
    if (result.resolved_path.empty()) {
      result.resolved_path = command.selected_path;
    }

    if (operations.adopt_archive) {
      operations.adopt_archive(command.current_frame);
    }
    if (stopIfCancelled()) {
      return result;
    }
    if (operations.resolve_affiliated_media) {
      operations.resolve_affiliated_media();
    }
    if (stopIfCancelled()) {
      return result;
    }
    if (operations.resolve_stimulus_media) {
      operations.resolve_stimulus_media();
    }
    if (stopIfCancelled()) {
      return result;
    }

    requested_session.zarr_path = result.resolved_path;
    if (operations.active_recording_clip_index_path) {
      requested_session.recording_clip_index_path =
          operations.active_recording_clip_index_path();
    }
    if (stopIfCancelled()) {
      return result;
    }
    if (!workflow.completeProduct("archive", "Archive ready", true)) {
      return failArchiveOpen(workflow, operations, std::move(result),
                             "Archive readiness could not be published");
    }
    std::string commit_error;
    if (!workflow.commit(requested_session, "Archive ready",
                         "Archive unavailable", &commit_error)) {
      clearArchive(operations);
      result.error = commit_error.empty() ? "Archive session commit failed"
                                          : std::move(commit_error);
      return result;
    }
    result.ready = true;
    return result;
  } catch (const std::exception &error) {
    return failArchiveOpen(workflow, operations, std::move(result),
                           error.what());
  } catch (...) {
    return failArchiveOpen(workflow, operations, std::move(result),
                           "Archive open operation failed unexpectedly");
  }
}

} // namespace crimson::session
