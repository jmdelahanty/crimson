#include "platform/macos/apple_acquisition_crop_playback_session.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

void AssignError(std::string* destination, const std::string& value) {
  if (destination != nullptr) {
    *destination = value;
  }
}

bool RatesMatch(double left, double right) {
  const double tolerance = std::max(1e-6, std::abs(left) * 1e-6);
  return std::isfinite(left) && std::isfinite(right) &&
         std::abs(left - right) <= tolerance;
}

}  // namespace

struct AppleAcquisitionCropPlaybackSession::Impl {
  std::unique_ptr<crimson::zarr::AcquisitionCropRepository> repository;
  AppleVideoPlaybackBuffer playback;
  int full_frame_width = 0;
  int full_frame_height = 0;
  int64_t last_target_video_frame = -1;
  AppleAcquisitionCropPlaybackMetrics current_metrics;

  void resetState() {
    repository.reset();
    full_frame_width = 0;
    full_frame_height = 0;
    last_target_video_frame = -1;
    current_metrics = {};
  }
};

AppleAcquisitionCropPlaybackSession::AppleAcquisitionCropPlaybackSession()
    : impl_(std::make_unique<Impl>()) {}

AppleAcquisitionCropPlaybackSession::~AppleAcquisitionCropPlaybackSession() {
  close();
}

bool AppleAcquisitionCropPlaybackSession::open(
    std::unique_ptr<crimson::zarr::AcquisitionCropRepository> repository,
    int full_frame_width,
    int full_frame_height,
    size_t buffer_capacity,
    std::string* error) {
  close();
  if (!repository) {
    AssignError(error, "acquisition crop repository is null");
    return false;
  }
  if (full_frame_width <= 0 || full_frame_height <= 0) {
    AssignError(error, "full-frame dimensions must be positive");
    return false;
  }
  const auto& descriptor = repository->descriptor();
  if (descriptor.frame_count <= 0 || descriptor.output_width <= 0 ||
      descriptor.output_height <= 0 || descriptor.frame_rate <= 0.0 ||
      descriptor.frame_count > std::numeric_limits<int>::max() ||
      static_cast<uint64_t>(descriptor.frame_count) !=
          repository->cameraFrameCount()) {
    AssignError(error,
                "acquisition crop repository descriptor is inconsistent");
    return false;
  }
  for (int64_t frame = 0; frame < descriptor.frame_count; ++frame) {
    const auto geometry = repository->liveGeometry(
        frame, full_frame_width, full_frame_height);
    if (!geometry || !geometry->valid()) {
      AssignError(error,
                  "acquisition crop geometry is invalid at camera frame " +
                      std::to_string(frame));
      return false;
    }
  }

  if (descriptor.resolved_video_path.empty()) {
    AssignError(error,
                "acquisition crop repository has no resolved video path");
    return false;
  }
  if (!impl_->playback.open(descriptor.resolved_video_path.string(), "crop",
                            buffer_capacity, error)) {
    return false;
  }
  const auto& info = impl_->playback.info();
  if (info.width != descriptor.output_width ||
      info.height != descriptor.output_height ||
      info.frame_count != descriptor.frame_count ||
      !RatesMatch(info.nominal_frame_rate, descriptor.frame_rate)) {
    impl_->playback.close();
    AssignError(error,
                "acquisition crop video disagrees with repository contract");
    return false;
  }

  impl_->repository = std::move(repository);
  impl_->full_frame_width = full_frame_width;
  impl_->full_frame_height = full_frame_height;
  impl_->last_target_video_frame = -1;
  impl_->current_metrics = {};
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

void AppleAcquisitionCropPlaybackSession::close() {
  if (!impl_) {
    return;
  }
  impl_->playback.close();
  impl_->resetState();
}

void AppleAcquisitionCropPlaybackSession::suspend() {
  if (!isOpen()) {
    return;
  }
  impl_->playback.suspend();
  impl_->last_target_video_frame = -1;
  impl_->current_metrics.last_action =
      AppleAcquisitionCropDecodeAction::Clear;
  impl_->current_metrics.last_target_video_frame = -1;
}

bool AppleAcquisitionCropPlaybackSession::isOpen() const {
  return impl_->playback.isOpen() && impl_->repository != nullptr;
}

const AppleVideoAssetInfo& AppleAcquisitionCropPlaybackSession::info() const {
  return impl_->playback.info();
}

const crimson::zarr::AcquisitionCropRepository*
AppleAcquisitionCropPlaybackSession::repository() const {
  return impl_->repository.get();
}

int AppleAcquisitionCropPlaybackSession::fullFrameWidth() const {
  return impl_->full_frame_width;
}

int AppleAcquisitionCropPlaybackSession::fullFrameHeight() const {
  return impl_->full_frame_height;
}

bool AppleAcquisitionCropPlaybackSession::requestCameraFrame(
    int64_t camera_frame,
    bool discontinuity,
    std::string* error) {
  if (!isOpen()) {
    AssignError(error, "acquisition crop playback session is not open");
    return false;
  }
  if (error != nullptr) {
    error->clear();
  }

  const auto resolution = impl_->repository->resolveCameraFrame(camera_frame);
  ++impl_->current_metrics.camera_requests;
  impl_->current_metrics.last_camera_frame = camera_frame;
  if (resolution.status ==
      crimson::zarr::AcquisitionCropMappingStatus::OutOfRange) {
    ++impl_->current_metrics.out_of_range_requests;
    impl_->current_metrics.last_target_video_frame = -1;
    impl_->current_metrics.last_action =
        AppleAcquisitionCropDecodeAction::Clear;
    return true;
  }
  ++impl_->current_metrics.mapped_requests;
  if (!resolution.video_frame) {
    ++impl_->current_metrics.failed_requests;
    AssignError(error, "mapped acquisition crop frame has no video identity");
    return false;
  }
  const int64_t target = *resolution.video_frame;
  impl_->current_metrics.last_target_video_frame = target;
  if (target < 0 || target >= impl_->playback.info().frame_count) {
    ++impl_->current_metrics.failed_requests;
    AssignError(error,
                "mapped acquisition crop frame is outside the video range");
    return false;
  }

  const bool target_buffered =
      impl_->playback.frameForTarget(target, true).has_value();
  if (target_buffered) {
    impl_->current_metrics.last_action =
        AppleAcquisitionCropDecodeAction::Hold;
    ++impl_->current_metrics.hold_requests;
  } else if (discontinuity || impl_->last_target_video_frame < 0 ||
             target < impl_->last_target_video_frame ||
             target - impl_->last_target_video_frame >=
                 static_cast<int64_t>(impl_->playback.capacity())) {
    impl_->current_metrics.last_action =
        AppleAcquisitionCropDecodeAction::Seek;
    ++impl_->current_metrics.seek_requests;
    if (!impl_->playback.requestSeek(target, error)) {
      ++impl_->current_metrics.failed_requests;
      return false;
    }
  } else {
    impl_->current_metrics.last_action =
        AppleAcquisitionCropDecodeAction::Follow;
    ++impl_->current_metrics.follow_requests;
    impl_->playback.setTargetFrame(target);
  }
  impl_->last_target_video_frame = target;
  return true;
}

bool AppleAcquisitionCropPlaybackSession::waitForCameraFrame(
    int64_t camera_frame,
    std::chrono::milliseconds timeout,
    std::string* error) {
  if (!isOpen()) {
    AssignError(error, "acquisition crop playback session is not open");
    return false;
  }
  if (error != nullptr) {
    error->clear();
  }
  const auto resolution = impl_->repository->resolveCameraFrame(camera_frame);
  if (resolution.status != crimson::zarr::AcquisitionCropMappingStatus::Mapped ||
      !resolution.video_frame) {
    AssignError(error, "camera frame has no mapped acquisition crop frame");
    return false;
  }
  if (!impl_->playback.waitForFrame(*resolution.video_frame, timeout)) {
    const auto decoder_metrics = impl_->playback.metrics();
    AssignError(error, decoder_metrics.last_error.empty()
                           ? "timed out waiting for acquisition crop frame"
                           : decoder_metrics.last_error);
    return false;
  }
  if (!frameForCameraFrame(camera_frame)) {
    AssignError(error,
                "decoded acquisition crop identity does not match mapping");
    return false;
  }
  return true;
}

std::optional<AppleAlignedAcquisitionCropFrame>
AppleAcquisitionCropPlaybackSession::frameForCameraFrame(
    int64_t camera_frame) const {
  if (!isOpen()) {
    return std::nullopt;
  }
  auto resolution = impl_->repository->resolveCameraFrame(camera_frame);
  if (resolution.status != crimson::zarr::AcquisitionCropMappingStatus::Mapped ||
      !resolution.video_frame) {
    return std::nullopt;
  }
  auto decoded = impl_->playback.frameForTarget(*resolution.video_frame, true);
  if (!decoded || decoded->metadata.stream_id != "crop" ||
      decoded->metadata.frame_number != *resolution.video_frame ||
      decoded->metadata.local_frame_number != *resolution.video_frame) {
    return std::nullopt;
  }

  auto source_state = impl_->repository->acquisitionFrameState(
      camera_frame, decoded->metadata.frame_number, impl_->full_frame_width,
      impl_->full_frame_height);
  crimson::crop::CropFrameSourceState frame_state;
  frame_state.camera_frame = camera_frame;
  frame_state.acquisition = source_state;
  auto selection = crimson::crop::SelectCropSource(
      impl_->repository->sourceCapabilities(), frame_state,
      crimson::crop::CropSourcePreference::PreferAcquisitionVideo,
      crimson::crop::CropFallbackPolicy::WaitForPreferred,
      static_cast<int64_t>(impl_->repository->cameraFrameCount()));
  if (!selection.selected() ||
      selection.source != crimson::crop::CropSourceKind::AcquisitionVideo ||
      selection.source_frame_index != resolution.video_frame) {
    return std::nullopt;
  }
  return AppleAlignedAcquisitionCropFrame{
      std::move(resolution), std::move(source_state), std::move(selection),
      std::move(*decoded)};
}

crimson::zarr::AcquisitionCropFrameResolution
AppleAcquisitionCropPlaybackSession::resolveCameraFrame(
    int64_t camera_frame) const {
  if (!impl_->repository) {
    crimson::zarr::AcquisitionCropFrameResolution resolution;
    resolution.camera_frame = camera_frame;
    return resolution;
  }
  return impl_->repository->resolveCameraFrame(camera_frame);
}

AppleAcquisitionCropPlaybackMetrics
AppleAcquisitionCropPlaybackSession::metrics() const {
  auto result = impl_->current_metrics;
  result.decoder = impl_->playback.metrics();
  return result;
}
