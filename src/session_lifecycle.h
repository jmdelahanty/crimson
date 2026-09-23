#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace crimson::session {

enum class SessionPhase : uint8_t {
  Empty,
  Opening,
  Ready,
  ReplacementPending,
  Closing,
  Closed,
  Failed,
};

const char *sessionPhaseName(SessionPhase phase);

struct SessionDescriptor {
  std::string video_path;
  std::string zarr_path;
  std::string stimulus_video_path;
  std::string recording_clip_index_path;

  bool empty() const;
  bool operator==(const SessionDescriptor &other) const;
};

struct SessionReplacementRequest {
  bool requested = false;
  std::string video_path;
  std::string zarr_path;
  std::string stimulus_video_path;
  int video_buffer_capacity = 6;
  int stimulus_buffer_capacity = 6;
  std::string recording_clip_index_path;

  SessionDescriptor descriptor() const;
};

struct SessionSnapshot {
  SessionPhase phase = SessionPhase::Empty;
  uint64_t generation = 0;
  SessionDescriptor active;
  SessionDescriptor pending;
  std::optional<SessionReplacementRequest> replacement;
  std::string error;

  bool ready() const;
  bool busy() const;
};

// Tracks logical session ownership only. Platform adapters continue to own
// windows, decoders, repositories, and GPU resources.
class SessionLifecycle {
public:
  uint64_t beginOpen(SessionDescriptor requested);
  bool completeOpen(uint64_t generation,
                    std::optional<SessionDescriptor> resolved = std::nullopt);
  bool failOpen(uint64_t generation, std::string error);

  bool requestReplacement(SessionReplacementRequest request,
                          std::string *error = nullptr);
  std::optional<SessionReplacementRequest> replacementRequest() const;

  bool beginClose();
  void completeClose();
  SessionSnapshot snapshot() const;

private:
  mutable std::mutex mutex_;
  SessionSnapshot state_;
};

} // namespace crimson::session
