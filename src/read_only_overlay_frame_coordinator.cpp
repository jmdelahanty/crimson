#include "read_only_overlay_frame_coordinator.h"

#include <ostream>

namespace crimson::overlay {

ReadOnlyOverlayFramePlan ReadOnlyOverlayFrameCoordinator::beginFrame(
    const ReadOnlyOverlayFrameInput &input) {
  if (pending_sequence_) {
    ++metrics_.superseded_plans;
  }
  pending_sequence_.reset();

  ReadOnlyOverlayFramePlan plan;
  plan.sequence = next_sequence_++;
  plan.camera_frame = input.camera_frame;
  metrics_.last_sequence = plan.sequence;
  ++metrics_.frame_updates;

  if (input.camera_frame < 0) {
    ++metrics_.invalid_frames;
    return plan;
  }
  if (!input.demand_enabled || !input.layer_enabled) {
    ++metrics_.inactive_frames;
    return plan;
  }
  if (!input.source_available) {
    plan.status = ReadOnlyOverlayFramePlanStatus::Unavailable;
    ++metrics_.unavailable_frames;
    return plan;
  }

  plan.status = ReadOnlyOverlayFramePlanStatus::Request;
  plan.issue_request = true;
  plan.request_discontinuity =
      input.presentation_discontinuity && has_accepted_request_;
  pending_sequence_ = plan.sequence;
  ++metrics_.request_plans;
  if (plan.request_discontinuity) {
    ++metrics_.discontinuity_requests;
  }
  return plan;
}

ReadOnlyOverlayFrameDecision ReadOnlyOverlayFrameCoordinator::finishFrame(
    const ReadOnlyOverlayFramePlan &plan,
    const ReadOnlyOverlayFrameCandidate &candidate) {
  ReadOnlyOverlayFrameDecision decision;
  decision.sequence = plan.sequence;
  decision.requested_frame = plan.camera_frame;
  decision.candidate_frame = candidate.camera_frame.value_or(-1);

  if (plan.status != ReadOnlyOverlayFramePlanStatus::Request ||
      !plan.issue_request || !pending_sequence_ ||
      *pending_sequence_ != plan.sequence) {
    ++metrics_.stale_completions;
    decision.action = ReadOnlyOverlayFrameAction::IgnoreStale;
    return decision;
  }
  pending_sequence_.reset();

  if (!candidate.request_accepted) {
    ++metrics_.rejected_requests;
    decision.action = ReadOnlyOverlayFrameAction::RequestRejected;
    return decision;
  }
  has_accepted_request_ = true;
  metrics_.last_requested_frame = plan.camera_frame;
  ++metrics_.accepted_requests;

  if (candidate.camera_frame && *candidate.camera_frame != plan.camera_frame) {
    ++metrics_.stale_completions;
    ++metrics_.mismatched_candidate_frames;
    decision.action = ReadOnlyOverlayFrameAction::IgnoreStale;
    return decision;
  }

  switch (candidate.status) {
  case ReadOnlyOverlayFrameCandidateStatus::Pending:
    ++metrics_.pending_frames;
    decision.action = ReadOnlyOverlayFrameAction::Wait;
    return decision;
  case ReadOnlyOverlayFrameCandidateStatus::Missing:
    ++metrics_.missing_frames;
    decision.action = ReadOnlyOverlayFrameAction::Clear;
    return decision;
  case ReadOnlyOverlayFrameCandidateStatus::Failed:
    ++metrics_.failed_frames;
    decision.action = ReadOnlyOverlayFrameAction::Failed;
    return decision;
  case ReadOnlyOverlayFrameCandidateStatus::Mapped:
    if (!candidate.camera_frame) {
      ++metrics_.failed_frames;
      decision.action = ReadOnlyOverlayFrameAction::Failed;
      return decision;
    }
    metrics_.last_presented_frame = *candidate.camera_frame;
    ++metrics_.exact_presentations;
    decision.action = ReadOnlyOverlayFrameAction::Present;
    decision.exact = true;
    return decision;
  }

  ++metrics_.failed_frames;
  decision.action = ReadOnlyOverlayFrameAction::Failed;
  return decision;
}

void ReadOnlyOverlayFrameCoordinator::reset() {
  next_sequence_ = 1;
  pending_sequence_.reset();
  has_accepted_request_ = false;
  metrics_ = {};
}

const ReadOnlyOverlayFrameMetrics &
ReadOnlyOverlayFrameCoordinator::metrics() const {
  return metrics_;
}

void writeReadOnlyOverlayFrameDiagnostics(
    std::ostream &output, std::string_view platform, std::string_view source,
    const ReadOnlyOverlayFrameMetrics &metrics) {
  output << '[' << platform << "OverlayPresentation] source=" << source
         << " updates=" << metrics.frame_updates
         << " requests=" << metrics.request_plans
         << " accepted=" << metrics.accepted_requests
         << " rejected=" << metrics.rejected_requests
         << " discontinuities=" << metrics.discontinuity_requests
         << " pending=" << metrics.pending_frames
         << " missing=" << metrics.missing_frames
         << " failed=" << metrics.failed_frames
         << " exact=" << metrics.exact_presentations
         << " stale=" << metrics.stale_completions
         << " mismatched=" << metrics.mismatched_candidate_frames
         << " superseded=" << metrics.superseded_plans
         << " last_requested=" << metrics.last_requested_frame
         << " last_presented=" << metrics.last_presented_frame << '\n';
}

} // namespace crimson::overlay
