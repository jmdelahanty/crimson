#include "stimulus_playback_coordinator.h"

#include <cstdint>

namespace crimson::playback {

StimulusPlaybackCoordinator::StimulusPlaybackCoordinator(
    const zarr::StimulusRepository& repository)
    : repository_(&repository) {}

StimulusDecodeDecision StimulusPlaybackCoordinator::requestCameraFrame(
    int32_t camera_frame,
    bool discontinuity,
    const StimulusDecoderSnapshot& decoder) {
  StimulusDecodeDecision decision;
  decision.resolution = repository_->resolveCameraFrame(camera_frame);
  if (decision.resolution.status != zarr::StimulusMappingStatus::Mapped ||
      !decision.resolution.stimulus_frame) {
    last_requested_stimulus_frame_.reset();
    return decision;
  }

  const int32_t target = *decision.resolution.stimulus_frame;
  decision.target_frame = target;
  if (decoder.target_buffered) {
    decision.action = StimulusDecodeAction::Hold;
  } else {
    bool seek = discontinuity || !last_requested_stimulus_frame_;
    if (last_requested_stimulus_frame_ &&
        target < *last_requested_stimulus_frame_) {
      seek = true;
    }
    if (decoder.last_decoded_frame) {
      const int64_t forward_distance =
          static_cast<int64_t>(target) - *decoder.last_decoded_frame;
      if (forward_distance <= 0) {
        seek = true;
      } else if (decoder.max_forward_decode_frames > 0 &&
                 static_cast<uint64_t>(forward_distance) >
                     decoder.max_forward_decode_frames) {
        seek = true;
      }
    }
    decision.action =
        seek ? StimulusDecodeAction::Seek : StimulusDecodeAction::Follow;
  }

  last_requested_stimulus_frame_ = target;
  return decision;
}

void StimulusPlaybackCoordinator::reset() {
  last_requested_stimulus_frame_.reset();
}

std::optional<int32_t>
StimulusPlaybackCoordinator::lastRequestedStimulusFrame() const {
  return last_requested_stimulus_frame_;
}

}  // namespace crimson::playback
