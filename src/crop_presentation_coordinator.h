#pragma once

#include "crop_source_contract.h"

#include <cstdint>
#include <optional>

namespace crimson::crop {

enum class CropPresentationAction : uint8_t {
  Clear,
  Wait,
  Hold,
  Present,
};

struct CropPresentationDecision {
  CropPresentationAction action = CropPresentationAction::Clear;
  CropSourceSelection selection;
  uint64_t generation = 0;
  int64_t presented_camera_frame = -1;
  bool commit_crop = false;
  bool render_current = false;
};

struct CropPresentationMetrics {
  uint64_t candidate_updates = 0;
  uint64_t exact_presentations = 0;
  uint64_t held_presentations = 0;
  uint64_t cleared_presentations = 0;
  uint64_t deferred_presentations = 0;
  uint64_t unavailable_presentations = 0;
  uint64_t invalid_presentations = 0;
  uint64_t mismatched_selection_frames = 0;
  uint64_t mismatched_surface_frames = 0;
  uint64_t consecutive_unavailable = 0;
  uint64_t max_consecutive_unavailable = 0;
  uint64_t last_generation = 0;
  uint64_t last_rendered_generation = 0;
  int64_t last_camera_frame = -1;
  int64_t presented_crop_camera_frame = -1;
  int64_t presented_source_frame = -1;
  int64_t camera_skew_frames = 0;
  uint64_t max_abs_camera_skew_frames = 0;
  std::optional<CropSourceKind> presented_source;
};

class CropPresentationCoordinator {
 public:
  CropPresentationDecision update(
      int64_t presented_camera_frame,
      const CropSourceSelection& selection,
      std::optional<int64_t> crop_surface_camera_frame);

  void resetVisibleFrame();
  const CropPresentationMetrics& metrics() const;

 private:
  bool visibleMatches(const CropSourceSelection& selection) const;
  void clearVisibleFrame();
  void recordVisibleFrame(int64_t presented_camera_frame,
                          const CropSourceSelection& selection,
                          int64_t crop_surface_camera_frame);
  CropPresentationDecision defer(CropPresentationDecision decision);

  uint64_t next_generation_ = 1;
  std::optional<CropSourceKind> visible_source_;
  std::optional<int64_t> visible_source_frame_;
  int64_t visible_camera_frame_ = -1;
  CropPresentationMetrics metrics_;
};

}  // namespace crimson::crop
