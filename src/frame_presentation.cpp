#include "frame_presentation.h"

#include <algorithm>

namespace crimson::playback {

FramePresentationDecision
resolveFramePresentation(const FramePresentationRequest &request) {
  FramePresentationDecision decision;
  decision.requested_frame = request.requested_frame;
  decision.discontinuity = request.discontinuity;

  if (request.candidate_frame && *request.candidate_frame >= 0) {
    decision.action = FramePresentationAction::Present;
    decision.presented_frame = *request.candidate_frame;
    decision.commit_candidate = true;
    decision.render_current = true;
    decision.exact = request.requested_frame >= 0 &&
                     *request.candidate_frame == request.requested_frame;
    return decision;
  }

  if (request.missing_policy == MissingFramePolicy::HoldVisible &&
      request.visible_frame && *request.visible_frame >= 0) {
    decision.action = FramePresentationAction::Hold;
    decision.presented_frame = *request.visible_frame;
    decision.render_current = true;
    decision.exact = request.requested_frame >= 0 &&
                     *request.visible_frame == request.requested_frame;
    return decision;
  }

  return decision;
}

void FramePresentationTracker::record(
    int64_t requested_frame, int64_t presented_frame, bool discontinuity,
    std::optional<double> lag_frames) {
  metrics_.last_requested_frame = requested_frame;
  if (presented_frame < 0) {
    return;
  }
  if (discontinuity) {
    ++metrics_.discontinuities;
  }
  if (presented_frame == requested_frame) {
    ++metrics_.exact_presentations;
  }
  if (presented_frame == metrics_.last_presented_frame) {
    ++metrics_.repeated_presentations;
  } else if (!discontinuity && metrics_.last_presented_frame >= 0 &&
             presented_frame > metrics_.last_presented_frame + 1) {
    metrics_.skipped_source_frames += static_cast<uint64_t>(
        presented_frame - metrics_.last_presented_frame - 1);
  }
  if (!discontinuity && lag_frames && *lag_frames > 0.5) {
    ++metrics_.late_presentations;
    metrics_.max_lag_frames =
        std::max(metrics_.max_lag_frames, *lag_frames);
  }
  metrics_.last_presented_frame = presented_frame;
  ++metrics_.presentation_count;
}

void FramePresentationTracker::reset() { metrics_ = {}; }

const FramePresentationMetrics &FramePresentationTracker::metrics() const {
  return metrics_;
}

} // namespace crimson::playback
