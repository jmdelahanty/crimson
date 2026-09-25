#pragma once

#include "recording_open_workflow.h"

#include <cstdint>
#include <functional>
#include <string>

namespace crimson::session {

struct ArchiveOpenCommand {
  SessionDescriptor current_session;
  std::string selected_path;
  int64_t current_frame = 0;
};

struct ArchiveOpenOperations {
  std::function<bool(const std::string &selected_path,
                     std::string &resolved_path, std::string &error)>
      open_archive;
  std::function<void(int64_t current_frame)> adopt_archive;
  std::function<void()> clear_archive;
  std::function<void()> resolve_affiliated_media;
  std::function<void()> resolve_stimulus_media;
  std::function<std::string()> active_recording_clip_index_path;
  std::function<bool()> opening_cancelled;
};

struct ArchiveOpenResult {
  bool ready = false;
  uint64_t generation = 0;
  std::string resolved_path;
  std::string error;
};

ArchiveOpenResult executeArchiveOpen(RecordingOpenWorkflowController &workflow,
                                     const ArchiveOpenCommand &command,
                                     const ArchiveOpenOperations &operations);

} // namespace crimson::session
