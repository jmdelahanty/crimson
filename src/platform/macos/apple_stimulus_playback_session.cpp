#include "platform/macos/apple_stimulus_playback_session.h"

#include <limits>
#include <utility>

namespace {

void AssignError(std::string* destination, const std::string& value) {
  if (destination != nullptr) {
    *destination = value;
  }
}

}  // namespace

struct AppleStimulusPlaybackSession::Impl {
  std::unique_ptr<crimson::zarr::StimulusRepository> repository;
  std::unique_ptr<crimson::playback::StimulusPlaybackCoordinator> coordinator;
  AppleVideoPlaybackBuffer playback;
  AppleStimulusPlaybackMetrics current_metrics;

  void resetState() {
    coordinator.reset();
    repository.reset();
    current_metrics = {};
  }
};

AppleStimulusPlaybackSession::AppleStimulusPlaybackSession()
    : impl_(std::make_unique<Impl>()) {}

AppleStimulusPlaybackSession::~AppleStimulusPlaybackSession() { close(); }

bool AppleStimulusPlaybackSession::open(
    std::unique_ptr<crimson::zarr::StimulusRepository> repository,
    size_t buffer_capacity,
    std::string* error) {
  close();
  if (!repository) {
    AssignError(error, "stimulus repository is null");
    return false;
  }
  if (!repository->hasMapping()) {
    AssignError(error, "stimulus repository has no frame mapping");
    return false;
  }
  const std::string video_path =
      repository->resolvedSourceVideoPath().empty()
          ? repository->sourceVideoPath()
          : repository->resolvedSourceVideoPath();
  if (video_path.empty()) {
    AssignError(error, "stimulus repository has no source video path");
    return false;
  }
  if (!impl_->playback.open(video_path, "stimulus", buffer_capacity, error)) {
    return false;
  }

  impl_->repository = std::move(repository);
  impl_->coordinator =
      std::make_unique<crimson::playback::StimulusPlaybackCoordinator>(
          *impl_->repository);
  impl_->current_metrics = {};
  return true;
}

void AppleStimulusPlaybackSession::close() {
  if (!impl_) {
    return;
  }
  impl_->playback.close();
  impl_->resetState();
}

void AppleStimulusPlaybackSession::suspend() {
  if (!impl_->playback.isOpen()) {
    return;
  }
  impl_->playback.suspend();
  impl_->coordinator->reset();
  impl_->current_metrics.last_action =
      crimson::playback::StimulusDecodeAction::Clear;
  impl_->current_metrics.last_target_stimulus_frame = -1;
}

bool AppleStimulusPlaybackSession::isOpen() const {
  return impl_->playback.isOpen() && impl_->repository != nullptr;
}

const AppleVideoAssetInfo& AppleStimulusPlaybackSession::info() const {
  return impl_->playback.info();
}

const crimson::zarr::StimulusRepository*
AppleStimulusPlaybackSession::repository() const {
  return impl_->repository.get();
}

bool AppleStimulusPlaybackSession::requestCameraFrame(
    int32_t camera_frame,
    bool discontinuity,
    std::string* error) {
  if (!isOpen()) {
    AssignError(error, "stimulus playback session is not open");
    return false;
  }
  if (error != nullptr) {
    error->clear();
  }

  const auto preliminary = impl_->repository->resolveCameraFrame(camera_frame);
  crimson::playback::StimulusDecoderSnapshot decoder;
  const auto decoder_metrics = impl_->playback.metrics();
  if (decoder_metrics.last_decoded_frame >= 0 &&
      decoder_metrics.last_decoded_frame <=
          std::numeric_limits<int32_t>::max()) {
    decoder.last_decoded_frame =
        static_cast<int32_t>(decoder_metrics.last_decoded_frame);
  }
  decoder.max_forward_decode_frames = impl_->playback.capacity();
  if (preliminary.status == crimson::zarr::StimulusMappingStatus::Mapped &&
      preliminary.stimulus_frame) {
    decoder.target_buffered = impl_->playback
                                  .frameForTarget(
                                      *preliminary.stimulus_frame, true)
                                  .has_value();
  }

  auto decision = impl_->coordinator->requestCameraFrame(
      camera_frame, discontinuity, decoder);
  ++impl_->current_metrics.camera_requests;
  impl_->current_metrics.last_camera_frame = camera_frame;
  impl_->current_metrics.last_action = decision.action;

  if (decision.resolution.status ==
      crimson::zarr::StimulusMappingStatus::Missing) {
    ++impl_->current_metrics.missing_requests;
  } else if (decision.resolution.status ==
             crimson::zarr::StimulusMappingStatus::OutOfRange) {
    ++impl_->current_metrics.out_of_range_requests;
  } else {
    ++impl_->current_metrics.mapped_requests;
  }

  if (!decision.target_frame) {
    impl_->current_metrics.last_target_stimulus_frame = -1;
    return true;
  }
  const int32_t target = *decision.target_frame;
  impl_->current_metrics.last_target_stimulus_frame = target;
  if (target < 0 || target >= impl_->playback.info().frame_count) {
    ++impl_->current_metrics.failed_requests;
    AssignError(error,
                "mapped stimulus frame is outside the source video range");
    impl_->coordinator->reset();
    return false;
  }

  switch (decision.action) {
    case crimson::playback::StimulusDecodeAction::Clear:
      return true;
    case crimson::playback::StimulusDecodeAction::Hold:
      ++impl_->current_metrics.hold_requests;
      return true;
    case crimson::playback::StimulusDecodeAction::Follow:
      ++impl_->current_metrics.follow_requests;
      impl_->playback.setTargetFrame(target);
      return true;
    case crimson::playback::StimulusDecodeAction::Seek:
      ++impl_->current_metrics.seek_requests;
      if (!impl_->playback.requestSeek(target, error)) {
        ++impl_->current_metrics.failed_requests;
        return false;
      }
      return true;
  }
  return false;
}

bool AppleStimulusPlaybackSession::waitForCameraFrame(
    int32_t camera_frame,
    std::chrono::milliseconds timeout,
    std::string* error) {
  if (!isOpen()) {
    AssignError(error, "stimulus playback session is not open");
    return false;
  }
  if (error != nullptr) {
    error->clear();
  }
  const auto resolution = impl_->repository->resolveCameraFrame(camera_frame);
  if (resolution.status != crimson::zarr::StimulusMappingStatus::Mapped ||
      !resolution.stimulus_frame) {
    AssignError(error, "camera frame has no mapped stimulus frame");
    return false;
  }
  if (!impl_->playback.waitForFrame(*resolution.stimulus_frame, timeout)) {
    const auto decoder_metrics = impl_->playback.metrics();
    AssignError(error, decoder_metrics.last_error.empty()
                           ? "timed out waiting for mapped stimulus frame"
                           : decoder_metrics.last_error);
    return false;
  }
  const auto frame = impl_->playback.frameForTarget(
      *resolution.stimulus_frame, true);
  if (!frame || frame->metadata.frame_number != *resolution.stimulus_frame) {
    AssignError(error, "decoded stimulus frame identity does not match mapping");
    return false;
  }
  return true;
}

std::optional<AppleAlignedStimulusFrame>
AppleStimulusPlaybackSession::frameForCameraFrame(int32_t camera_frame) const {
  if (!isOpen()) {
    return std::nullopt;
  }
  const auto resolution = impl_->repository->resolveCameraFrame(camera_frame);
  if (resolution.status != crimson::zarr::StimulusMappingStatus::Mapped ||
      !resolution.stimulus_frame) {
    return std::nullopt;
  }
  auto frame = impl_->playback.frameForTarget(*resolution.stimulus_frame, true);
  if (!frame || frame->metadata.frame_number != *resolution.stimulus_frame) {
    return std::nullopt;
  }
  return AppleAlignedStimulusFrame{resolution, std::move(*frame)};
}

crimson::zarr::StimulusFrameResolution
AppleStimulusPlaybackSession::resolveCameraFrame(int32_t camera_frame) const {
  if (!impl_->repository) {
    crimson::zarr::StimulusFrameResolution resolution;
    resolution.camera_frame = camera_frame;
    resolution.status = crimson::zarr::StimulusMappingStatus::OutOfRange;
    return resolution;
  }
  return impl_->repository->resolveCameraFrame(camera_frame);
}

AppleStimulusPlaybackMetrics AppleStimulusPlaybackSession::metrics() const {
  auto result = impl_->current_metrics;
  result.decoder = impl_->playback.metrics();
  return result;
}
