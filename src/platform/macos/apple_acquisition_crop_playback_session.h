#pragma once

#include "crop_source_contract.h"
#include "platform/macos/apple_video_playback_buffer.h"
#include "zarr/acquisition_crop_repository.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

enum class AppleAcquisitionCropDecodeAction : uint8_t {
  Clear,
  Hold,
  Follow,
  Seek,
};

struct AppleAlignedAcquisitionCropFrame {
  crimson::zarr::AcquisitionCropFrameResolution resolution;
  crimson::crop::AcquisitionCropFrameState source_state;
  crimson::crop::CropSourceSelection selection;
  AppleDecodedVideoFrame decoded_frame;
};

struct AppleAcquisitionCropPlaybackMetrics {
  uint64_t camera_requests = 0;
  uint64_t mapped_requests = 0;
  uint64_t out_of_range_requests = 0;
  uint64_t hold_requests = 0;
  uint64_t follow_requests = 0;
  uint64_t seek_requests = 0;
  uint64_t failed_requests = 0;
  int64_t last_camera_frame = -1;
  int64_t last_target_video_frame = -1;
  AppleAcquisitionCropDecodeAction last_action =
      AppleAcquisitionCropDecodeAction::Clear;
  AppleVideoPlaybackBufferMetrics decoder;
};

class AppleAcquisitionCropPlaybackSession {
 public:
  AppleAcquisitionCropPlaybackSession();
  ~AppleAcquisitionCropPlaybackSession();

  AppleAcquisitionCropPlaybackSession(
      const AppleAcquisitionCropPlaybackSession&) = delete;
  AppleAcquisitionCropPlaybackSession& operator=(
      const AppleAcquisitionCropPlaybackSession&) = delete;

  bool open(
      std::unique_ptr<crimson::zarr::AcquisitionCropRepository> repository,
      int full_frame_width,
      int full_frame_height,
      size_t buffer_capacity,
      std::string* error = nullptr);
  void close();
  void suspend();

  bool isOpen() const;
  const AppleVideoAssetInfo& info() const;
  const crimson::zarr::AcquisitionCropRepository* repository() const;
  int fullFrameWidth() const;
  int fullFrameHeight() const;

  bool requestCameraFrame(
      int64_t camera_frame,
      bool discontinuity,
      std::string* error = nullptr);
  bool waitForCameraFrame(
      int64_t camera_frame,
      std::chrono::milliseconds timeout,
      std::string* error = nullptr);
  std::optional<AppleAlignedAcquisitionCropFrame> frameForCameraFrame(
      int64_t camera_frame) const;
  crimson::zarr::AcquisitionCropFrameResolution resolveCameraFrame(
      int64_t camera_frame) const;

  AppleAcquisitionCropPlaybackMetrics metrics() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
