#include "crop_presentation_coordinator.h"

#include <algorithm>
#include <cstdlib>

namespace crimson::crop {

CropPresentationDecision CropPresentationCoordinator::update(
    int64_t presented_camera_frame,
    const CropSourceSelection& selection,
    std::optional<int64_t> crop_surface_camera_frame) {
  CropPresentationDecision decision;
  decision.selection = selection;
  decision.generation = next_generation_++;
  decision.presented_camera_frame = presented_camera_frame;
  metrics_.last_generation = decision.generation;
  ++metrics_.candidate_updates;

  if (presented_camera_frame < 0 ||
      selection.camera_frame != presented_camera_frame) {
    ++metrics_.mismatched_selection_frames;
    return defer(std::move(decision));
  }

  if (!selection.selected()) {
    if (selection.status == CropSourceSelectionStatus::InvalidState) {
      ++metrics_.invalid_presentations;
      return defer(std::move(decision));
    }
    if (selection.status == CropSourceSelectionStatus::AwaitingExactFrame) {
      if (visibleMatches(selection)) {
        metrics_.consecutive_unavailable = 0;
        ++metrics_.held_presentations;
        metrics_.last_camera_frame = presented_camera_frame;
        metrics_.last_rendered_generation = decision.generation;
        decision.action = CropPresentationAction::Hold;
        decision.commit_crop = true;
        decision.render_current = true;
        return decision;
      }
      return defer(std::move(decision));
    }

    metrics_.consecutive_unavailable = 0;
    ++metrics_.cleared_presentations;
    metrics_.last_camera_frame = presented_camera_frame;
    metrics_.last_rendered_generation = decision.generation;
    clearVisibleFrame();
    decision.action = CropPresentationAction::Clear;
    decision.commit_crop = true;
    return decision;
  }

  if (!selection.source_frame_index || !crop_surface_camera_frame ||
      *crop_surface_camera_frame != presented_camera_frame) {
    ++metrics_.mismatched_surface_frames;
    return defer(std::move(decision));
  }
  if (selection.geometry && !selection.geometry->valid()) {
    ++metrics_.invalid_presentations;
    return defer(std::move(decision));
  }
  if (selection.source == CropSourceKind::LiveGeometry &&
      (!selection.geometry || !selection.geometry->usableForLiveCrop() ||
       *selection.source_frame_index != presented_camera_frame)) {
    ++metrics_.invalid_presentations;
    return defer(std::move(decision));
  }

  const bool was_visible = visibleMatches(selection);
  metrics_.consecutive_unavailable = 0;
  metrics_.last_camera_frame = presented_camera_frame;
  metrics_.last_rendered_generation = decision.generation;
  recordVisibleFrame(presented_camera_frame, selection,
                     *crop_surface_camera_frame);
  decision.action = was_visible ? CropPresentationAction::Hold
                                : CropPresentationAction::Present;
  decision.commit_crop = true;
  decision.render_current = true;
  if (was_visible) {
    ++metrics_.held_presentations;
  } else {
    ++metrics_.exact_presentations;
  }
  return decision;
}

void CropPresentationCoordinator::resetVisibleFrame() {
  clearVisibleFrame();
  metrics_.consecutive_unavailable = 0;
}

const CropPresentationMetrics& CropPresentationCoordinator::metrics() const {
  return metrics_;
}

bool CropPresentationCoordinator::visibleMatches(
    const CropSourceSelection& selection) const {
  return visible_source_ && visible_source_frame_ && selection.source &&
         selection.source_frame_index &&
         visible_camera_frame_ == selection.camera_frame &&
         *visible_source_ == *selection.source &&
         *visible_source_frame_ == *selection.source_frame_index;
}

void CropPresentationCoordinator::clearVisibleFrame() {
  visible_source_.reset();
  visible_source_frame_.reset();
  visible_camera_frame_ = -1;
  metrics_.presented_crop_camera_frame = -1;
  metrics_.presented_source_frame = -1;
  metrics_.presented_source.reset();
  metrics_.camera_skew_frames = 0;
}

void CropPresentationCoordinator::recordVisibleFrame(
    int64_t presented_camera_frame,
    const CropSourceSelection& selection,
    int64_t crop_surface_camera_frame) {
  visible_source_ = selection.source;
  visible_source_frame_ = selection.source_frame_index;
  visible_camera_frame_ = presented_camera_frame;
  metrics_.presented_crop_camera_frame = crop_surface_camera_frame;
  metrics_.presented_source_frame = *selection.source_frame_index;
  metrics_.presented_source = selection.source;
  metrics_.camera_skew_frames =
      presented_camera_frame - crop_surface_camera_frame;
  metrics_.max_abs_camera_skew_frames = std::max(
      metrics_.max_abs_camera_skew_frames,
      static_cast<uint64_t>(std::llabs(metrics_.camera_skew_frames)));
}

CropPresentationDecision CropPresentationCoordinator::defer(
    CropPresentationDecision decision) {
  ++metrics_.unavailable_presentations;
  ++metrics_.deferred_presentations;
  ++metrics_.consecutive_unavailable;
  metrics_.max_consecutive_unavailable = std::max(
      metrics_.max_consecutive_unavailable,
      metrics_.consecutive_unavailable);
  decision.action = CropPresentationAction::Wait;
  return decision;
}

}  // namespace crimson::crop
