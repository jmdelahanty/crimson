#include "platform/nvidia/nvidia_camera_frame_data_adapter.h"

#include "platform/nvidia/nvidia_detection_presentation_adapter.h"

#include <chrono>
#include <limits>
#include <utility>

namespace crimson::platform::nvidia {
namespace {

double durationMs(std::chrono::steady_clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

bool validFrameIndex(int frame) {
  return frame >= 0 &&
         static_cast<uint64_t>(frame) <=
             static_cast<uint64_t>(std::numeric_limits<size_t>::max());
}

} // namespace

CameraFrameQuery selectCameraFrameQuery(const CameraFrameQueryInput &input) {
  CameraFrameQuery result;
  result.has_presented_frame = input.presented_frame >= 0;
  result.query_frame =
      result.has_presented_frame ? input.presented_frame : input.current_frame;
  if (input.mapped_media && input.clip_switch_in_progress &&
      input.pending_switch_parent_frame >= 0 &&
      input.last_presented_parent_frame >= 0 &&
      !input.presented_frame_uses_selected_clip) {
    if (input.last_presented_parent_frame <=
        static_cast<int64_t>(std::numeric_limits<int>::max())) {
      result.query_frame = static_cast<int>(input.last_presented_parent_frame);
      result.retained_previous_clip_frame = true;
    }
  }
  return result;
}

CameraFrameDataAdapter::CameraFrameDataAdapter(
    zarr::DetectionRepository &detection_repository,
    zarr::KeypointOverlayRepository &keypoint_repository,
    CameraFrameDataCallbacks callbacks)
    : detection_repository_(detection_repository),
      keypoint_repository_(keypoint_repository),
      callbacks_(std::move(callbacks)) {}

CameraFrameData
CameraFrameDataAdapter::resolve(const CameraFrameDataRequest &request) const {
  CameraFrameData result;
  result.query_frame = request.query_frame;
  result.detection_descriptor = detection_repository_.descriptor();
  result.keypoint_descriptor = keypoint_repository_.descriptor();
  result.dataset_allows_bbox_edit =
      request.archive_loaded && request.frame_selected &&
      result.detection_descriptor.activeDatasetAllowsBboxEditing();

  if (!request.archive_loaded || !request.frame_selected ||
      !validFrameIndex(request.query_frame)) {
    return result;
  }

  const auto total_start = std::chrono::steady_clock::now();
  const size_t frame = static_cast<size_t>(request.query_frame);

  const auto detection_start = std::chrono::steady_clock::now();
  result.detection_frame = detection_repository_.resolveFrame(frame, false);
  result.metrics.detection_repository_ms =
      durationMs(std::chrono::steady_clock::now() - detection_start);
  if (result.detection_frame.ready() &&
      result.detection_frame.frame_id != frame) {
    result.detection_frame = {};
    result.detection_frame.frame_id = frame;
    result.metrics.stale_results_discarded++;
  }
  result.detection_frame_ready = result.detection_frame.ready();

  if (request.resolve_keypoints &&
      !result.keypoint_descriptor.run_name.empty()) {
    result.keypoint_frame_requested = true;
    const auto keypoint_start = std::chrono::steady_clock::now();
    result.keypoint_frame = keypoint_repository_.resolveCameraFrame(
        request.query_frame, request.source_width, request.source_height);
    result.metrics.keypoint_repository_ms =
        durationMs(std::chrono::steady_clock::now() - keypoint_start);
    if (result.keypoint_frame.status == zarr::KeypointOverlayStatus::Mapped &&
        result.keypoint_frame.camera_frame != request.query_frame) {
      result.keypoint_frame = {};
      result.keypoint_frame.camera_frame = request.query_frame;
      result.keypoint_frame.error = "Discarded stale keypoint frame";
      result.metrics.stale_results_discarded++;
    }
  }

  const auto conversion_start = std::chrono::steady_clock::now();
  result.source_boxes = crimson::platform::nvidia::makeLegacyBoundingBoxes(
      result.detection_descriptor, result.detection_frame);
  result.metrics.bounding_box_conversion_ms =
      durationMs(std::chrono::steady_clock::now() - conversion_start);

  result.frame_is_interpolated =
      result.detection_descriptor.interpolation_available &&
      detection_repository_.isFrameInterpolated(frame);

  result.legacy_details_requested =
      request.load_legacy_details ||
      (request.load_legacy_details_when_keypoints_available &&
       !result.keypoint_descriptor.run_name.empty()) ||
      (request.load_legacy_details_for_synthetic_detections &&
       result.detection_descriptor.active_dataset_has_synthetic_observations);
  if (result.legacy_details_requested && callbacks_.resolve_legacy_details) {
    result.legacy_details.frame_id = frame;
    if (request.include_eye_masks && !request.allow_blocking_eye_mask_load &&
        callbacks_.request_eye_mask_cache) {
      callbacks_.request_eye_mask_cache(frame,
                                        request.mask_prefetch_lookahead_frames);
    }
    const auto details_start = std::chrono::steady_clock::now();
    auto details = callbacks_.resolve_legacy_details(
        frame, request.include_eye_masks, request.include_subject_shapes,
        request.allow_blocking_eye_mask_load);
    result.metrics.legacy_details_ms =
        durationMs(std::chrono::steady_clock::now() - details_start);
    if (details.frame_id == frame) {
      result.legacy_details = std::move(details);
      result.legacy_details_ready = true;
    } else {
      result.metrics.stale_results_discarded++;
    }
  }

  result.metrics.total_ms =
      durationMs(std::chrono::steady_clock::now() - total_start);
  return result;
}

} // namespace crimson::platform::nvidia
