#pragma once

#include "zarr/stimulus_repository.h"

#include <cstdint>
#include <optional>

namespace crimson::playback {

enum class StimulusPresentationAction : uint8_t {
  Clear,
  Wait,
  Hold,
  Present,
};

struct StimulusPresentationDecision {
  StimulusPresentationAction action = StimulusPresentationAction::Clear;
  zarr::StimulusFrameResolution resolution;
  uint64_t generation = 0;
  int32_t presented_camera_frame = -1;
  bool commit_composite = false;
  bool render_current = false;
};

struct StimulusPresentationMetrics {
  uint64_t candidate_updates = 0;
  uint64_t camera_presentations = 0;
  uint64_t mapped_presentations = 0;
  uint64_t missing_presentations = 0;
  uint64_t out_of_range_presentations = 0;
  uint64_t interpolated_presentations = 0;
  uint64_t exact_presentations = 0;
  uint64_t held_presentations = 0;
  uint64_t unavailable_presentations = 0;
  uint64_t deferred_presentations = 0;
  uint64_t mismatched_mapping_frames = 0;
  uint64_t mismatched_decoded_frames = 0;
  uint64_t cleared_presentations = 0;
  uint64_t consecutive_unavailable = 0;
  uint64_t max_consecutive_unavailable = 0;
  uint64_t last_generation = 0;
  uint64_t last_rendered_generation = 0;
  int32_t last_camera_frame = -1;
  int32_t last_target_stimulus_frame = -1;
  int32_t presented_stimulus_frame = -1;
  int32_t stimulus_source_camera_frame = -1;
  int64_t camera_skew_frames = 0;
  uint64_t max_abs_camera_skew_frames = 0;
};

class StimulusPresentationCoordinator {
 public:
  StimulusPresentationDecision update(
      int32_t presented_camera_frame,
      const zarr::StimulusFrameResolution& resolution,
      std::optional<int32_t> decoded_stimulus_frame);

  void resetVisibleFrame();
  const StimulusPresentationMetrics& metrics() const;

 private:
  void clearVisibleFrame();
  void recordVisibleFrame(int32_t presented_camera_frame,
                          int32_t stimulus_source_camera_frame,
                          int32_t stimulus_frame);

  uint64_t next_generation_ = 1;
  std::optional<int32_t> visible_stimulus_frame_;
  StimulusPresentationMetrics metrics_;
};

}  // namespace crimson::playback
