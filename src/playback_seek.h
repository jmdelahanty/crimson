#pragma once

#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace crimson::playback {

enum class PlaybackSeekPhase : uint8_t {
  Preview,
  Commit,
  Discrete,
};

enum class PlaybackSeekOrigin : uint8_t {
  CameraControls,
  KeyboardShortcut,
  Timeline,
  Programmatic,
};

enum class PlaybackSeekAccuracy : uint8_t {
  ApproximateAllowed,
  Exact,
};

enum class PlaybackSeekExecutionMode : uint8_t {
  LogicalCursorOnly,
  BackendSeek,
};

enum class PlaybackSeekExecutionPath : uint8_t {
  None,
  LogicalCursor,
  ResidentBuffer,
  BackendDecoder,
};

enum class PlaybackSeekExecutionStatus : uint8_t {
  Submitted,
  Completed,
  Deduplicated,
  Rejected,
  Failed,
  Cancelled,
  DiscardedStale,
};

enum class PlaybackSeekRejection : uint8_t {
  None,
  InvalidTimeline,
  UnsupportedAccuracy,
};

struct PlaybackSeekRequest {
  PlaybackSeekPhase phase = PlaybackSeekPhase::Discrete;
  PlaybackSeekOrigin origin = PlaybackSeekOrigin::Programmatic;
  int64_t target_frame = 0;
  int64_t frame_count = 0;
};

struct PlaybackSeekTransaction {
  uint64_t generation = 0;
  PlaybackSeekRequest request;
};

struct PlaybackSeekAdapterCapabilities {
  bool logical_cursor_preview = false;
  bool approximate_backend_seek = false;
  bool exact_backend_seek = true;
  bool resident_frame_selection = false;
};

struct PlaybackSeekExecutionPlan {
  bool valid = false;
  PlaybackSeekRejection rejection = PlaybackSeekRejection::None;
  PlaybackSeekExecutionMode mode = PlaybackSeekExecutionMode::BackendSeek;
  PlaybackSeekAccuracy accuracy = PlaybackSeekAccuracy::Exact;
  bool pause_playback = true;
  bool prefer_resident_frame = false;
};

struct PlaybackSeekExecutionResult {
  PlaybackSeekExecutionStatus status = PlaybackSeekExecutionStatus::Rejected;
  PlaybackSeekExecutionPath path = PlaybackSeekExecutionPath::None;
  int64_t resolved_frame = -1;
  double queue_ms = 0.0;
  double service_ms = 0.0;
  std::string error;
};

struct PlaybackSeekTelemetryEvent {
  PlaybackSeekTransaction transaction;
  PlaybackSeekExecutionPlan plan;
  PlaybackSeekExecutionResult result;
};

struct PlaybackSeekTelemetryMetrics {
  uint64_t requests = 0;
  uint64_t previews = 0;
  uint64_t commits = 0;
  uint64_t discrete = 0;
  uint64_t superseded = 0;
  uint64_t logical_cursor_completions = 0;
  uint64_t resident_buffer_completions = 0;
  uint64_t backend_submissions = 0;
  uint64_t completed = 0;
  uint64_t deduplicated = 0;
  uint64_t rejected = 0;
  uint64_t failed = 0;
  uint64_t cancelled = 0;
  uint64_t discarded_stale = 0;
  uint64_t active_generation = 0;
  PlaybackSeekTelemetryEvent last_event;
};

std::optional<PlaybackSeekRequest>
makePlaybackSeekRequest(PlaybackSeekPhase phase, PlaybackSeekOrigin origin,
                        int64_t target_frame, int64_t frame_count);

PlaybackSeekExecutionPlan
planPlaybackSeek(const PlaybackSeekTransaction &transaction,
                 const PlaybackSeekAdapterCapabilities &capabilities);

class PlaybackSeekCoordinator {
public:
  PlaybackSeekTransaction begin(const PlaybackSeekRequest &request);
  PlaybackSeekTelemetryEvent record(const PlaybackSeekTransaction &transaction,
                                    const PlaybackSeekExecutionPlan &plan,
                                    PlaybackSeekExecutionResult result);
  std::optional<PlaybackSeekTelemetryEvent>
  recordActive(PlaybackSeekExecutionResult result);
  std::optional<PlaybackSeekTelemetryEvent> cancelActive();

  bool isCurrent(uint64_t generation) const;
  uint64_t activeGeneration() const { return active_generation_; }
  std::optional<PlaybackSeekTransaction> activeTransaction() const;
  const PlaybackSeekTelemetryMetrics &metrics() const { return metrics_; }
  void reset();

private:
  uint64_t next_generation_ = 0;
  uint64_t active_generation_ = 0;
  PlaybackSeekTransaction active_transaction_;
  PlaybackSeekExecutionPlan active_plan_;
  std::chrono::steady_clock::time_point active_started_at_{};
  PlaybackSeekTelemetryMetrics metrics_;
};

std::string_view playbackSeekPhaseName(PlaybackSeekPhase phase);
std::string_view playbackSeekOriginName(PlaybackSeekOrigin origin);
std::string_view playbackSeekAccuracyName(PlaybackSeekAccuracy accuracy);
std::string_view playbackSeekExecutionModeName(PlaybackSeekExecutionMode mode);
std::string_view playbackSeekExecutionPathName(PlaybackSeekExecutionPath path);
std::string_view
playbackSeekExecutionStatusName(PlaybackSeekExecutionStatus status);
std::string_view playbackSeekRejectionName(PlaybackSeekRejection rejection);
void writePlaybackSeekDiagnostics(std::ostream &output, std::string_view prefix,
                                  const PlaybackSeekTelemetryMetrics &metrics);

} // namespace crimson::playback
