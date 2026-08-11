#pragma once

#include "h5_loader.h"
#include "zarr/detection_repository.h"
#include "zarr/keypoint_overlay_repository.h"
#include "zarr_loader.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace crimson::platform::nvidia {

struct CameraFrameQueryInput {
  int current_frame = -1;
  int presented_frame = -1;
  bool mapped_media = false;
  bool clip_switch_in_progress = false;
  int64_t pending_switch_parent_frame = -1;
  int64_t last_presented_parent_frame = -1;
  bool presented_frame_uses_selected_clip = false;
};

struct CameraFrameQuery {
  bool has_presented_frame = false;
  int query_frame = -1;
  bool retained_previous_clip_frame = false;
};

CameraFrameQuery selectCameraFrameQuery(const CameraFrameQueryInput &input);

struct CameraFrameDataRequest {
  bool archive_loaded = false;
  bool has_presented_frame = false;
  int query_frame = -1;
  int source_width = 0;
  int source_height = 0;
  bool resolve_keypoints = true;
  bool load_legacy_details = true;
  bool include_eye_masks = false;
  bool include_subject_shapes = false;
  bool allow_blocking_eye_mask_load = true;
  size_t mask_prefetch_lookahead_frames = 0;
};

struct CameraFrameDataMetrics {
  double total_ms = 0.0;
  double detection_repository_ms = 0.0;
  double keypoint_repository_ms = 0.0;
  double bounding_box_conversion_ms = 0.0;
  double legacy_details_ms = 0.0;
  uint64_t stale_results_discarded = 0;
};

struct CameraFrameData {
  int query_frame = -1;
  zarr::DetectionRepositoryDescriptor detection_descriptor;
  zarr::DetectionFrame detection_frame;
  bool detection_frame_ready = false;
  zarr::KeypointOverlayDescriptor keypoint_descriptor;
  zarr::KeypointOverlayResolution keypoint_frame;
  bool keypoint_frame_requested = false;
  std::vector<LoggedBoundingBox> source_boxes;
  ZarrDetectionLoader::FrameDetections legacy_details{};
  bool legacy_details_ready = false;
  bool frame_is_interpolated = false;
  bool dataset_allows_bbox_edit = false;
  CameraFrameDataMetrics metrics;
};

struct CameraFrameDataCallbacks {
  std::function<void(size_t frame, size_t lookahead_frames)>
      request_eye_mask_cache;
  std::function<ZarrDetectionLoader::FrameDetections(
      size_t frame, bool include_eye_masks, bool include_subject_shapes,
      bool allow_blocking_eye_mask_load)>
      resolve_legacy_details;
};

class CameraFrameDataAdapter {
public:
  CameraFrameDataAdapter(zarr::DetectionRepository &detection_repository,
                         zarr::KeypointOverlayRepository &keypoint_repository,
                         CameraFrameDataCallbacks callbacks = {});

  CameraFrameData resolve(const CameraFrameDataRequest &request) const;

private:
  zarr::DetectionRepository &detection_repository_;
  zarr::KeypointOverlayRepository &keypoint_repository_;
  CameraFrameDataCallbacks callbacks_;
};

} // namespace crimson::platform::nvidia
