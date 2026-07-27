#include "session_lifecycle.h"

#include <utility>

namespace crimson::session {

const char *sessionPhaseName(SessionPhase phase) {
  switch (phase) {
  case SessionPhase::Empty:
    return "empty";
  case SessionPhase::Opening:
    return "opening";
  case SessionPhase::Ready:
    return "ready";
  case SessionPhase::ReplacementPending:
    return "replacement_pending";
  case SessionPhase::Closing:
    return "closing";
  case SessionPhase::Closed:
    return "closed";
  case SessionPhase::Failed:
    return "failed";
  }
  return "unknown";
}

bool SessionDescriptor::empty() const {
  return video_path.empty() && zarr_path.empty() &&
         stimulus_video_path.empty();
}

bool SessionDescriptor::operator==(const SessionDescriptor &other) const {
  return video_path == other.video_path && zarr_path == other.zarr_path &&
         stimulus_video_path == other.stimulus_video_path;
}

SessionDescriptor SessionReplacementRequest::descriptor() const {
  return {video_path, zarr_path, stimulus_video_path};
}

bool SessionSnapshot::ready() const { return phase == SessionPhase::Ready; }

bool SessionSnapshot::busy() const {
  return phase == SessionPhase::Opening ||
         phase == SessionPhase::ReplacementPending ||
         phase == SessionPhase::Closing;
}

uint64_t SessionLifecycle::beginOpen(SessionDescriptor requested) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++state_.generation;
  state_.pending = std::move(requested);
  state_.replacement.reset();
  state_.error.clear();
  state_.phase = SessionPhase::Opening;
  return state_.generation;
}

bool SessionLifecycle::completeOpen(
    uint64_t generation, std::optional<SessionDescriptor> resolved) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (generation != state_.generation ||
      state_.phase != SessionPhase::Opening) {
    return false;
  }
  state_.active = resolved ? std::move(*resolved) : std::move(state_.pending);
  state_.pending = {};
  state_.replacement.reset();
  state_.error.clear();
  state_.phase = state_.active.empty() ? SessionPhase::Empty
                                       : SessionPhase::Ready;
  return true;
}

bool SessionLifecycle::failOpen(uint64_t generation, std::string error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (generation != state_.generation ||
      state_.phase != SessionPhase::Opening) {
    return false;
  }
  state_.pending = {};
  state_.replacement.reset();
  state_.error = error.empty() ? "Session open failed" : std::move(error);
  state_.phase = SessionPhase::Failed;
  return true;
}

bool SessionLifecycle::requestReplacement(SessionReplacementRequest request,
                                          std::string *error) {
  if (!request.requested || request.descriptor().empty()) {
    if (error != nullptr) {
      *error = "A replacement session requires at least one media path";
    }
    return false;
  }
  if (request.video_buffer_capacity <= 0 ||
      request.stimulus_buffer_capacity <= 0) {
    if (error != nullptr) {
      *error = "Replacement session buffer capacities must be positive";
    }
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  ++state_.generation;
  state_.pending = request.descriptor();
  state_.replacement = std::move(request);
  state_.error.clear();
  state_.phase = SessionPhase::ReplacementPending;
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

std::optional<SessionReplacementRequest>
SessionLifecycle::replacementRequest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_.replacement;
}

bool SessionLifecycle::beginClose() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_.phase == SessionPhase::Closing ||
      state_.phase == SessionPhase::Closed) {
    return false;
  }
  ++state_.generation;
  state_.phase = SessionPhase::Closing;
  state_.pending = {};
  return true;
}

void SessionLifecycle::completeClose() {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.active = {};
  state_.pending = {};
  state_.phase = SessionPhase::Closed;
}

SessionSnapshot SessionLifecycle::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

} // namespace crimson::session
