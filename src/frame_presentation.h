#pragma once

#include <cstdint>
#include <optional>

namespace crimson::playback {

enum class FramePresentationAction : uint8_t {
  Clear,
  Hold,
  Present,
};

enum class MissingFramePolicy : uint8_t {
  HoldVisible,
  ClearVisible,
};

struct FramePresentationRequest {
  int64_t requested_frame = -1;
  std::optional<int64_t> candidate_frame;
  std::optional<int64_t> visible_frame;
  MissingFramePolicy missing_policy = MissingFramePolicy::HoldVisible;
  bool discontinuity = false;
};

struct FramePresentationDecision {
  FramePresentationAction action = FramePresentationAction::Clear;
  int64_t requested_frame = -1;
  int64_t presented_frame = -1;
  bool commit_candidate = false;
  bool render_current = false;
  bool exact = false;
  bool discontinuity = false;
};

FramePresentationDecision
resolveFramePresentation(const FramePresentationRequest &request);

struct FramePresentationMetrics {
  uint64_t presentation_count = 0;
  uint64_t exact_presentations = 0;
  uint64_t repeated_presentations = 0;
  uint64_t skipped_source_frames = 0;
  uint64_t late_presentations = 0;
  uint64_t discontinuities = 0;
  int64_t last_requested_frame = -1;
  int64_t last_presented_frame = -1;
  double max_lag_frames = 0.0;
};

class FramePresentationTracker {
public:
  void record(int64_t requested_frame, int64_t presented_frame,
              bool discontinuity,
              std::optional<double> lag_frames = std::nullopt);
  void reset();
  const FramePresentationMetrics &metrics() const;

private:
  FramePresentationMetrics metrics_;
};

} // namespace crimson::playback
