#pragma once

#include "platform/macos/apple_video_frame_provider.h"
#include "platform/macos/apple_video_playback_buffer.h"
#include "stimulus_playback_coordinator.h"
#include "zarr/stimulus_repository.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct AppleAlignedStimulusFrame {
  crimson::zarr::StimulusFrameResolution resolution;
  AppleDecodedVideoFrame decoded_frame;
};

struct AppleStimulusPlaybackMetrics {
  uint64_t camera_requests = 0;
  uint64_t mapped_requests = 0;
  uint64_t missing_requests = 0;
  uint64_t out_of_range_requests = 0;
  uint64_t hold_requests = 0;
  uint64_t follow_requests = 0;
  uint64_t seek_requests = 0;
  uint64_t failed_requests = 0;
  int32_t last_camera_frame = -1;
  int32_t last_target_stimulus_frame = -1;
  crimson::playback::StimulusDecodeAction last_action =
      crimson::playback::StimulusDecodeAction::Clear;
  AppleVideoPlaybackBufferMetrics decoder;
};

class AppleStimulusPlaybackSession {
 public:
  AppleStimulusPlaybackSession();
  ~AppleStimulusPlaybackSession();

  AppleStimulusPlaybackSession(const AppleStimulusPlaybackSession&) = delete;
  AppleStimulusPlaybackSession& operator=(
      const AppleStimulusPlaybackSession&) = delete;

  bool open(
      std::unique_ptr<crimson::zarr::StimulusRepository> repository,
      size_t buffer_capacity,
      std::string* error = nullptr);
  void close();
  void suspend();

  bool isOpen() const;
  const AppleVideoAssetInfo& info() const;
  const crimson::zarr::StimulusRepository* repository() const;

  bool requestCameraFrame(
      int32_t camera_frame,
      bool discontinuity,
      std::string* error = nullptr);
  bool waitForCameraFrame(
      int32_t camera_frame,
      std::chrono::milliseconds timeout,
      std::string* error = nullptr);
  std::optional<AppleAlignedStimulusFrame> frameForCameraFrame(
      int32_t camera_frame) const;
  crimson::zarr::StimulusFrameResolution resolveCameraFrame(
      int32_t camera_frame) const;

  AppleStimulusPlaybackMetrics metrics() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
