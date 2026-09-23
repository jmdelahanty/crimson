#pragma once

#include "zarr/stimulus_repository.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace crimson::playback {

enum class StimulusDecodeAction : uint8_t {
  Clear,
  Hold,
  Follow,
  Seek,
};

struct StimulusDecoderSnapshot {
  std::optional<int32_t> last_decoded_frame;
  bool target_buffered = false;
  size_t max_forward_decode_frames = 0;
};

struct StimulusDecodeDecision {
  zarr::StimulusFrameResolution resolution;
  StimulusDecodeAction action = StimulusDecodeAction::Clear;
  std::optional<int32_t> target_frame;
};

class StimulusPlaybackCoordinator {
 public:
  explicit StimulusPlaybackCoordinator(
      const zarr::StimulusRepository& repository);

  StimulusDecodeDecision requestCameraFrame(
      int32_t camera_frame,
      bool discontinuity,
      const StimulusDecoderSnapshot& decoder);

  void reset();
  std::optional<int32_t> lastRequestedStimulusFrame() const;

 private:
  const zarr::StimulusRepository* repository_ = nullptr;
  std::optional<int32_t> last_requested_stimulus_frame_;
};

}  // namespace crimson::playback
