#include "playback_seek.h"

#include <algorithm>
#include <limits>
#include <ostream>
#include <utility>

namespace crimson::playback {
namespace {

bool terminalStatus(PlaybackSeekExecutionStatus status) {
  switch (status) {
  case PlaybackSeekExecutionStatus::Submitted:
    return false;
  case PlaybackSeekExecutionStatus::Completed:
  case PlaybackSeekExecutionStatus::Deduplicated:
  case PlaybackSeekExecutionStatus::Rejected:
  case PlaybackSeekExecutionStatus::Failed:
  case PlaybackSeekExecutionStatus::Cancelled:
  case PlaybackSeekExecutionStatus::DiscardedStale:
    return true;
  }
  return true;
}

} // namespace

std::optional<PlaybackSeekRequest>
makePlaybackSeekRequest(PlaybackSeekPhase phase, PlaybackSeekOrigin origin,
                        int64_t target_frame, int64_t frame_count) {
  if (frame_count <= 0) {
    return std::nullopt;
  }
  return PlaybackSeekRequest{
      phase,
      origin,
      std::clamp<int64_t>(target_frame, 0, frame_count - 1),
      frame_count,
  };
}

PlaybackSeekExecutionPlan
planPlaybackSeek(const PlaybackSeekTransaction &transaction,
                 const PlaybackSeekAdapterCapabilities &capabilities) {
  PlaybackSeekExecutionPlan result;
  if (transaction.generation == 0 || transaction.request.frame_count <= 0 ||
      transaction.request.target_frame < 0 ||
      transaction.request.target_frame >= transaction.request.frame_count) {
    result.rejection = PlaybackSeekRejection::InvalidTimeline;
    return result;
  }

  result.pause_playback = true;
  if (transaction.request.phase == PlaybackSeekPhase::Preview) {
    result.accuracy = PlaybackSeekAccuracy::ApproximateAllowed;
    if (capabilities.logical_cursor_preview) {
      result.valid = true;
      result.mode = PlaybackSeekExecutionMode::LogicalCursorOnly;
      return result;
    }
    if (!capabilities.approximate_backend_seek) {
      result.rejection = PlaybackSeekRejection::UnsupportedAccuracy;
      return result;
    }
    result.valid = true;
    result.mode = PlaybackSeekExecutionMode::BackendSeek;
    return result;
  }

  if (!capabilities.exact_backend_seek) {
    result.rejection = PlaybackSeekRejection::UnsupportedAccuracy;
    return result;
  }
  result.valid = true;
  result.mode = PlaybackSeekExecutionMode::BackendSeek;
  result.accuracy = PlaybackSeekAccuracy::Exact;
  result.prefer_resident_frame = capabilities.resident_frame_selection;
  return result;
}

PlaybackSeekTransaction
PlaybackSeekCoordinator::begin(const PlaybackSeekRequest &request) {
  if (active_generation_ != 0) {
    ++metrics_.superseded;
  }
  if (next_generation_ == std::numeric_limits<uint64_t>::max()) {
    next_generation_ = 0;
  }
  active_generation_ = ++next_generation_;
  active_transaction_ = PlaybackSeekTransaction{active_generation_, request};
  active_plan_ = {};
  active_started_at_ = std::chrono::steady_clock::now();
  ++metrics_.requests;
  switch (request.phase) {
  case PlaybackSeekPhase::Preview:
    ++metrics_.previews;
    break;
  case PlaybackSeekPhase::Commit:
    ++metrics_.commits;
    break;
  case PlaybackSeekPhase::Discrete:
    ++metrics_.discrete;
    break;
  }
  metrics_.active_generation = active_generation_;
  return active_transaction_;
}

PlaybackSeekTelemetryEvent
PlaybackSeekCoordinator::record(const PlaybackSeekTransaction &transaction,
                                const PlaybackSeekExecutionPlan &plan,
                                PlaybackSeekExecutionResult result) {
  if (!isCurrent(transaction.generation)) {
    result.status = PlaybackSeekExecutionStatus::DiscardedStale;
    result.path = PlaybackSeekExecutionPath::None;
    ++metrics_.discarded_stale;
  } else {
    active_plan_ = plan;
    if (terminalStatus(result.status) && result.service_ms <= 0.0) {
      result.service_ms =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - active_started_at_)
              .count();
    }
    switch (result.status) {
    case PlaybackSeekExecutionStatus::Submitted:
      ++metrics_.backend_submissions;
      break;
    case PlaybackSeekExecutionStatus::Completed:
      ++metrics_.completed;
      if (result.path == PlaybackSeekExecutionPath::LogicalCursor) {
        ++metrics_.logical_cursor_completions;
      } else if (result.path == PlaybackSeekExecutionPath::ResidentBuffer) {
        ++metrics_.resident_buffer_completions;
      }
      break;
    case PlaybackSeekExecutionStatus::Deduplicated:
      ++metrics_.deduplicated;
      break;
    case PlaybackSeekExecutionStatus::Rejected:
      ++metrics_.rejected;
      break;
    case PlaybackSeekExecutionStatus::Failed:
      ++metrics_.failed;
      break;
    case PlaybackSeekExecutionStatus::Cancelled:
      ++metrics_.cancelled;
      break;
    case PlaybackSeekExecutionStatus::DiscardedStale:
      ++metrics_.discarded_stale;
      break;
    }
    if (terminalStatus(result.status)) {
      active_generation_ = 0;
      metrics_.active_generation = 0;
    }
  }

  metrics_.last_event = PlaybackSeekTelemetryEvent{transaction, plan, result};
  return metrics_.last_event;
}

std::optional<PlaybackSeekTelemetryEvent>
PlaybackSeekCoordinator::recordActive(PlaybackSeekExecutionResult result) {
  if (active_generation_ == 0) {
    return std::nullopt;
  }
  return record(active_transaction_, active_plan_, std::move(result));
}

std::optional<PlaybackSeekTelemetryEvent>
PlaybackSeekCoordinator::cancelActive() {
  if (active_generation_ == 0) {
    return std::nullopt;
  }
  PlaybackSeekExecutionResult result;
  result.status = PlaybackSeekExecutionStatus::Cancelled;
  result.path = PlaybackSeekExecutionPath::None;
  return record(active_transaction_, active_plan_, std::move(result));
}

bool PlaybackSeekCoordinator::isCurrent(uint64_t generation) const {
  return generation != 0 && generation == active_generation_;
}

std::optional<PlaybackSeekTransaction>
PlaybackSeekCoordinator::activeTransaction() const {
  if (active_generation_ == 0) {
    return std::nullopt;
  }
  return active_transaction_;
}

void PlaybackSeekCoordinator::reset() {
  next_generation_ = 0;
  active_generation_ = 0;
  active_transaction_ = {};
  active_plan_ = {};
  active_started_at_ = {};
  metrics_ = {};
}

std::string_view playbackSeekPhaseName(PlaybackSeekPhase phase) {
  switch (phase) {
  case PlaybackSeekPhase::Preview:
    return "preview";
  case PlaybackSeekPhase::Commit:
    return "commit";
  case PlaybackSeekPhase::Discrete:
    return "discrete";
  }
  return "unknown";
}

std::string_view playbackSeekOriginName(PlaybackSeekOrigin origin) {
  switch (origin) {
  case PlaybackSeekOrigin::CameraControls:
    return "camera_controls";
  case PlaybackSeekOrigin::KeyboardShortcut:
    return "keyboard_shortcut";
  case PlaybackSeekOrigin::Timeline:
    return "timeline";
  case PlaybackSeekOrigin::Programmatic:
    return "programmatic";
  }
  return "unknown";
}

std::string_view playbackSeekAccuracyName(PlaybackSeekAccuracy accuracy) {
  switch (accuracy) {
  case PlaybackSeekAccuracy::ApproximateAllowed:
    return "approximate_allowed";
  case PlaybackSeekAccuracy::Exact:
    return "exact";
  }
  return "unknown";
}

std::string_view playbackSeekExecutionModeName(PlaybackSeekExecutionMode mode) {
  switch (mode) {
  case PlaybackSeekExecutionMode::LogicalCursorOnly:
    return "logical_cursor_only";
  case PlaybackSeekExecutionMode::BackendSeek:
    return "backend_seek";
  }
  return "unknown";
}

std::string_view playbackSeekExecutionPathName(PlaybackSeekExecutionPath path) {
  switch (path) {
  case PlaybackSeekExecutionPath::None:
    return "none";
  case PlaybackSeekExecutionPath::LogicalCursor:
    return "logical_cursor";
  case PlaybackSeekExecutionPath::ResidentBuffer:
    return "resident_buffer";
  case PlaybackSeekExecutionPath::BackendDecoder:
    return "backend_decoder";
  }
  return "unknown";
}

std::string_view
playbackSeekExecutionStatusName(PlaybackSeekExecutionStatus status) {
  switch (status) {
  case PlaybackSeekExecutionStatus::Submitted:
    return "submitted";
  case PlaybackSeekExecutionStatus::Completed:
    return "completed";
  case PlaybackSeekExecutionStatus::Deduplicated:
    return "deduplicated";
  case PlaybackSeekExecutionStatus::Rejected:
    return "rejected";
  case PlaybackSeekExecutionStatus::Failed:
    return "failed";
  case PlaybackSeekExecutionStatus::Cancelled:
    return "cancelled";
  case PlaybackSeekExecutionStatus::DiscardedStale:
    return "discarded_stale";
  }
  return "unknown";
}

std::string_view playbackSeekRejectionName(PlaybackSeekRejection rejection) {
  switch (rejection) {
  case PlaybackSeekRejection::None:
    return "none";
  case PlaybackSeekRejection::InvalidTimeline:
    return "invalid_timeline";
  case PlaybackSeekRejection::UnsupportedAccuracy:
    return "unsupported_accuracy";
  }
  return "unknown";
}

void writePlaybackSeekDiagnostics(std::ostream &output, std::string_view prefix,
                                  const PlaybackSeekTelemetryMetrics &metrics) {
  output << '[' << prefix << "TransportSeek] requests=" << metrics.requests
         << " previews=" << metrics.previews << " commits=" << metrics.commits
         << " discrete=" << metrics.discrete
         << " superseded=" << metrics.superseded
         << " logical=" << metrics.logical_cursor_completions
         << " resident=" << metrics.resident_buffer_completions
         << " backend_submitted=" << metrics.backend_submissions
         << " completed=" << metrics.completed
         << " deduplicated=" << metrics.deduplicated
         << " rejected=" << metrics.rejected << " failed=" << metrics.failed
         << " cancelled=" << metrics.cancelled
         << " discarded_stale=" << metrics.discarded_stale
         << " active_generation=" << metrics.active_generation << '\n';
}

} // namespace crimson::playback
