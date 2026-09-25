#include "stimulus_presentation_coordinator.h"

#include <algorithm>
#include <cstdlib>

namespace crimson::playback {

StimulusPresentationDecision StimulusPresentationCoordinator::update(
    int32_t presented_camera_frame,
    const zarr::StimulusFrameResolution& resolution,
    std::optional<int32_t> decoded_stimulus_frame) {
  StimulusPresentationDecision decision;
  decision.resolution = resolution;
  decision.generation = next_generation_++;
  decision.presented_camera_frame = presented_camera_frame;
  metrics_.last_generation = decision.generation;
  ++metrics_.candidate_updates;

  if (resolution.camera_frame != presented_camera_frame) {
    ++metrics_.mismatched_mapping_frames;
    ++metrics_.unavailable_presentations;
    ++metrics_.consecutive_unavailable;
    metrics_.max_consecutive_unavailable =
        std::max(metrics_.max_consecutive_unavailable,
                 metrics_.consecutive_unavailable);
    ++metrics_.deferred_presentations;
    decision.action = StimulusPresentationAction::Wait;
    return decision;
  }

  if (resolution.status == zarr::StimulusMappingStatus::Missing) {
    ++metrics_.missing_presentations;
    ++metrics_.cleared_presentations;
    metrics_.consecutive_unavailable = 0;
    ++metrics_.camera_presentations;
    metrics_.last_camera_frame = presented_camera_frame;
    metrics_.last_rendered_generation = decision.generation;
    clearVisibleFrame();
    decision.commit_composite = true;
    return decision;
  }
  if (resolution.status == zarr::StimulusMappingStatus::OutOfRange) {
    ++metrics_.out_of_range_presentations;
    ++metrics_.cleared_presentations;
    metrics_.consecutive_unavailable = 0;
    ++metrics_.camera_presentations;
    metrics_.last_camera_frame = presented_camera_frame;
    metrics_.last_rendered_generation = decision.generation;
    clearVisibleFrame();
    decision.commit_composite = true;
    return decision;
  }

  ++metrics_.mapped_presentations;
  if (resolution.interpolated) {
    ++metrics_.interpolated_presentations;
  }
  if (!resolution.stimulus_frame) {
    ++metrics_.unavailable_presentations;
    ++metrics_.consecutive_unavailable;
    metrics_.max_consecutive_unavailable =
        std::max(metrics_.max_consecutive_unavailable,
                 metrics_.consecutive_unavailable);
    ++metrics_.deferred_presentations;
    decision.action = StimulusPresentationAction::Wait;
    return decision;
  }

  const int32_t target = *resolution.stimulus_frame;
  metrics_.last_target_stimulus_frame = target;
  if (decoded_stimulus_frame && *decoded_stimulus_frame != target) {
    ++metrics_.mismatched_decoded_frames;
    ++metrics_.unavailable_presentations;
    ++metrics_.consecutive_unavailable;
    metrics_.max_consecutive_unavailable =
        std::max(metrics_.max_consecutive_unavailable,
                 metrics_.consecutive_unavailable);
    ++metrics_.deferred_presentations;
    decision.action = StimulusPresentationAction::Wait;
    return decision;
  }

  if (decoded_stimulus_frame && *decoded_stimulus_frame == target) {
    const bool was_visible =
        visible_stimulus_frame_ && *visible_stimulus_frame_ == target;
    visible_stimulus_frame_ = target;
    metrics_.consecutive_unavailable = 0;
    ++metrics_.camera_presentations;
    metrics_.last_camera_frame = presented_camera_frame;
    recordVisibleFrame(presented_camera_frame, resolution.camera_frame, target);
    metrics_.last_rendered_generation = decision.generation;
    decision.action = was_visible ? StimulusPresentationAction::Hold
                                  : StimulusPresentationAction::Present;
    decision.commit_composite = true;
    decision.render_current = true;
    if (was_visible) {
      ++metrics_.held_presentations;
    } else {
      ++metrics_.exact_presentations;
    }
    return decision;
  }

  if (visible_stimulus_frame_ && *visible_stimulus_frame_ == target) {
    metrics_.consecutive_unavailable = 0;
    ++metrics_.camera_presentations;
    metrics_.last_camera_frame = presented_camera_frame;
    recordVisibleFrame(presented_camera_frame, resolution.camera_frame, target);
    metrics_.last_rendered_generation = decision.generation;
    decision.action = StimulusPresentationAction::Hold;
    decision.commit_composite = true;
    decision.render_current = true;
    ++metrics_.held_presentations;
    return decision;
  }

  ++metrics_.unavailable_presentations;
  ++metrics_.consecutive_unavailable;
  metrics_.max_consecutive_unavailable =
      std::max(metrics_.max_consecutive_unavailable,
               metrics_.consecutive_unavailable);
  ++metrics_.deferred_presentations;
  decision.action = StimulusPresentationAction::Wait;
  return decision;
}

void StimulusPresentationCoordinator::resetVisibleFrame() {
  clearVisibleFrame();
  metrics_.consecutive_unavailable = 0;
}

const StimulusPresentationMetrics&
StimulusPresentationCoordinator::metrics() const {
  return metrics_;
}

void StimulusPresentationCoordinator::clearVisibleFrame() {
  visible_stimulus_frame_.reset();
  metrics_.presented_stimulus_frame = -1;
  metrics_.stimulus_source_camera_frame = -1;
  metrics_.camera_skew_frames = 0;
}

void StimulusPresentationCoordinator::recordVisibleFrame(
    int32_t presented_camera_frame,
    int32_t stimulus_source_camera_frame,
    int32_t stimulus_frame) {
  metrics_.presented_stimulus_frame = stimulus_frame;
  metrics_.stimulus_source_camera_frame = stimulus_source_camera_frame;
  metrics_.camera_skew_frames =
      static_cast<int64_t>(presented_camera_frame) -
      static_cast<int64_t>(metrics_.stimulus_source_camera_frame);
  metrics_.max_abs_camera_skew_frames =
      std::max(metrics_.max_abs_camera_skew_frames,
               static_cast<uint64_t>(std::llabs(metrics_.camera_skew_frames)));
}

}  // namespace crimson::playback
