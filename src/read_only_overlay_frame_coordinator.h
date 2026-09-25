#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string_view>

namespace crimson::overlay {

enum class ReadOnlyOverlayFramePlanStatus : uint8_t {
  Inactive,
  Unavailable,
  Request,
};

enum class ReadOnlyOverlayFrameCandidateStatus : uint8_t {
  Pending,
  Mapped,
  Missing,
  Failed,
};

enum class ReadOnlyOverlayFrameAction : uint8_t {
  None,
  Wait,
  Clear,
  Present,
  Failed,
  IgnoreStale,
  RequestRejected,
};

struct ReadOnlyOverlayFrameInput {
  int64_t camera_frame = -1;
  bool demand_enabled = false;
  bool layer_enabled = false;
  bool source_available = false;
  bool presentation_discontinuity = false;
};

struct ReadOnlyOverlayFramePlan {
  uint64_t sequence = 0;
  int64_t camera_frame = -1;
  ReadOnlyOverlayFramePlanStatus status =
      ReadOnlyOverlayFramePlanStatus::Inactive;
  bool issue_request = false;
  bool request_discontinuity = false;
};

struct ReadOnlyOverlayFrameCandidate {
  bool request_accepted = false;
  ReadOnlyOverlayFrameCandidateStatus status =
      ReadOnlyOverlayFrameCandidateStatus::Pending;
  std::optional<int64_t> camera_frame;
};

struct ReadOnlyOverlayFrameDecision {
  uint64_t sequence = 0;
  int64_t requested_frame = -1;
  int64_t candidate_frame = -1;
  ReadOnlyOverlayFrameAction action = ReadOnlyOverlayFrameAction::None;
  bool exact = false;
};

struct ReadOnlyOverlayFrameMetrics {
  uint64_t frame_updates = 0;
  uint64_t inactive_frames = 0;
  uint64_t unavailable_frames = 0;
  uint64_t invalid_frames = 0;
  uint64_t request_plans = 0;
  uint64_t accepted_requests = 0;
  uint64_t rejected_requests = 0;
  uint64_t discontinuity_requests = 0;
  uint64_t pending_frames = 0;
  uint64_t missing_frames = 0;
  uint64_t failed_frames = 0;
  uint64_t exact_presentations = 0;
  uint64_t stale_completions = 0;
  uint64_t mismatched_candidate_frames = 0;
  uint64_t superseded_plans = 0;
  uint64_t last_sequence = 0;
  int64_t last_requested_frame = -1;
  int64_t last_presented_frame = -1;
};

class ReadOnlyOverlayFrameCoordinator {
public:
  ReadOnlyOverlayFramePlan beginFrame(const ReadOnlyOverlayFrameInput &input);
  ReadOnlyOverlayFrameDecision
  finishFrame(const ReadOnlyOverlayFramePlan &plan,
              const ReadOnlyOverlayFrameCandidate &candidate);

  void reset();
  const ReadOnlyOverlayFrameMetrics &metrics() const;

private:
  uint64_t next_sequence_ = 1;
  std::optional<uint64_t> pending_sequence_;
  bool has_accepted_request_ = false;
  ReadOnlyOverlayFrameMetrics metrics_;
};

void writeReadOnlyOverlayFrameDiagnostics(
    std::ostream &output, std::string_view platform, std::string_view source,
    const ReadOnlyOverlayFrameMetrics &metrics);

} // namespace crimson::overlay
