#include "stimulus_open_coordinator.h"

#include <exception>
#include <utility>

namespace crimson::session {
namespace {

StimulusOpenResult failStimulusOpen(RecordingOpenWorkflowController &workflow,
                                    StimulusOpenResult result,
                                    std::string error) {
  result.media.error = std::move(error);
  if (workflow.active()) {
    workflow.failProduct("stimulus", "Stimulus media unavailable",
                         result.media.error, "Stimulus media unavailable");
  }
  return result;
}

void clearStimulusMedia(const StimulusOpenOperations &operations) {
  if (operations.clear_media) {
    operations.clear_media();
  }
}

} // namespace

StimulusOpenResult
executeStimulusOpen(RecordingOpenWorkflowController &workflow,
                    const StimulusOpenCommand &command,
                    const StimulusOpenOperations &operations) {
  StimulusOpenResult result;
  result.media.path = command.media.path;
  if (command.media.path.empty()) {
    result.media.error = "No stimulus media path was selected";
    return result;
  }
  if (!operations.open_media) {
    result.media.error = "Stimulus media open operation is unavailable";
    return result;
  }

  SessionDescriptor requested_session = command.current_session;
  requested_session.stimulus_video_path = command.media.path;
  std::string begin_error;
  if (!workflow.begin(
          {requested_session,
           "Opening stimulus media",
           {{"stimulus", ProductAvailabilityRequirement::Required}}},
          {}, &begin_error)) {
    result.media.error = begin_error.empty()
                             ? "Stimulus media open could not begin"
                             : std::move(begin_error);
    return result;
  }
  result.generation = workflow.generation();
  auto cancelled = [&] {
    if (operations.opening_cancelled && operations.opening_cancelled()) {
      workflow.cancel();
      result.media.error = "Session opening cancelled";
      return true;
    }
    return false;
  };
  if (cancelled()) {
    return result;
  }
  if (!workflow.startProduct("stimulus", "Opening stimulus media")) {
    return failStimulusOpen(workflow, std::move(result),
                            "Stimulus media loading could not start");
  }

  try {
    result.media = operations.open_media(command.media);
    if (cancelled()) {
      return result;
    }
    if (!result.media.ready) {
      if (result.media.error.empty()) {
        result.media.error = "Selected stimulus media could not be opened";
      }
      std::string media_error = result.media.error;
      return failStimulusOpen(workflow, std::move(result),
                              std::move(media_error));
    }
    if (!workflow.completeProduct("stimulus", "Stimulus media ready", true)) {
      clearStimulusMedia(operations);
      return failStimulusOpen(
          workflow, std::move(result),
          "Stimulus media readiness could not be published");
    }
    if (cancelled()) {
      return result;
    }
    requested_session.stimulus_video_path = result.media.path;
    std::string commit_error;
    if (!workflow.commit(requested_session, "Stimulus media ready",
                         "Stimulus media unavailable", &commit_error)) {
      clearStimulusMedia(operations);
      result.media.error = commit_error.empty()
                               ? "Stimulus media session commit failed"
                               : std::move(commit_error);
      return result;
    }
    result.ready = true;
    return result;
  } catch (const std::exception &error) {
    clearStimulusMedia(operations);
    return failStimulusOpen(workflow, std::move(result), error.what());
  } catch (...) {
    clearStimulusMedia(operations);
    return failStimulusOpen(workflow, std::move(result),
                            "Stimulus media open failed unexpectedly");
  }
}

} // namespace crimson::session
