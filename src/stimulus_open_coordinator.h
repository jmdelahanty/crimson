#pragma once

#include "recording_open_workflow.h"
#include "stimulus_media_open.h"

#include <cstdint>
#include <functional>

namespace crimson::session {

struct StimulusOpenCommand {
  SessionDescriptor current_session;
  media::StimulusMediaOpenRequest media;
};

struct StimulusOpenOperations {
  std::function<media::StimulusMediaOpenResult(
      const media::StimulusMediaOpenRequest &)>
      open_media;
  std::function<void()> clear_media;
};

struct StimulusOpenResult {
  bool ready = false;
  uint64_t generation = 0;
  media::StimulusMediaOpenResult media;
};

StimulusOpenResult
executeStimulusOpen(RecordingOpenWorkflowController &workflow,
                    const StimulusOpenCommand &command,
                    const StimulusOpenOperations &operations);

} // namespace crimson::session
