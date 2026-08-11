#include "platform/nvidia/nvidia_camera_frame_data_adapter.h"

#include <iostream>
#include <utility>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                    \
      return false;                                                            \
    }                                                                          \
  } while (false)

class FakeDetectionRepository final
    : public crimson::zarr::DetectionRepository {
public:
  crimson::zarr::DetectionRepositoryDescriptor descriptor_value;
  mutable size_t resolved_frame = 0;
  mutable int resolve_count = 0;
  bool stale = false;
  bool empty = false;

  crimson::zarr::DetectionRepositoryDescriptor descriptor() const override {
    return descriptor_value;
  }

  std::vector<crimson::zarr::DetectionDatasetOption>
  availableDatasets() const override {
    return {};
  }

  bool selectDataset(crimson::zarr::DetectionDataset) override { return true; }

  bool isDatasetAvailable(crimson::zarr::DetectionDataset) const override {
    return true;
  }

  size_t observationCount(size_t) const override { return empty ? 0 : 3; }

  bool isFrameInterpolated(size_t frame_id) const override {
    return frame_id == 42;
  }

  crimson::zarr::DetectionFrame resolveFrame(size_t frame_id,
                                             bool) const override {
    resolved_frame = frame_id;
    resolve_count++;
    crimson::zarr::DetectionFrame frame;
    frame.status = crimson::zarr::DetectionFrameStatus::Ready;
    frame.frame_id = stale ? frame_id + 1 : frame_id;
    if (!empty) {
      frame.observations.resize(3);
      for (size_t index = 0; index < frame.observations.size(); ++index) {
        auto &observation = frame.observations[index];
        observation.ordinal = index;
        observation.box_xyxy = {float(index), float(index + 1),
                                float(index + 4), float(index + 6)};
        observation.score = 0.9f;
        observation.class_id = static_cast<int32_t>(index);
      }
    }
    return frame;
  }
};

class FakeKeypointRepository final
    : public crimson::zarr::KeypointOverlayRepository {
public:
  crimson::zarr::KeypointOverlayDescriptor descriptor_value;
  mutable int64_t resolved_frame = -1;
  mutable int resolved_width = 0;
  mutable int resolved_height = 0;
  mutable int resolve_count = 0;
  bool stale = false;

  const crimson::zarr::KeypointOverlayDescriptor &descriptor() const override {
    return descriptor_value;
  }

  crimson::zarr::KeypointOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int full_frame_width,
                     int full_frame_height) const override {
    resolved_frame = camera_frame;
    resolved_width = full_frame_width;
    resolved_height = full_frame_height;
    resolve_count++;
    crimson::zarr::KeypointOverlayResolution result;
    result.status = crimson::zarr::KeypointOverlayStatus::Mapped;
    result.camera_frame = stale ? camera_frame + 1 : camera_frame;
    result.detections.resize(2);
    return result;
  }
};

bool testFrameQuerySelection() {
  auto query = crimson::platform::nvidia::selectCameraFrameQuery(
      {80, 100, false, false, -1, -1, false});
  CHECK(query.has_presented_frame);
  CHECK(query.query_frame == 100);
  CHECK(!query.retained_previous_clip_frame);

  query = crimson::platform::nvidia::selectCameraFrameQuery(
      {80, 100, true, true, 90, 89, false});
  CHECK(query.query_frame == 89);
  CHECK(query.retained_previous_clip_frame);

  query = crimson::platform::nvidia::selectCameraFrameQuery(
      {80, 100, true, true, 90, 89, true});
  CHECK(query.query_frame == 100);
  CHECK(!query.retained_previous_clip_frame);

  query = crimson::platform::nvidia::selectCameraFrameQuery(
      {80, -1, false, false, -1, -1, false});
  CHECK(!query.has_presented_frame);
  CHECK(query.query_frame == 80);
  return true;
}

bool testExactMultiRowAndNonblockingLoad() {
  FakeDetectionRepository detections;
  detections.descriptor_value.available = true;
  detections.descriptor_value.frames_per_second = 100.0;
  detections.descriptor_value.interpolation_available = true;
  detections.descriptor_value.active_dataset =
      crimson::zarr::DetectionDataset::RefinedRoot;
  FakeKeypointRepository keypoints;
  keypoints.descriptor_value.run_name = "keypoints";

  size_t prefetched_frame = 0;
  size_t prefetched_lookahead = 0;
  int prefetch_count = 0;
  int details_count = 0;
  bool details_blocking = true;
  bool details_include_eye_masks = false;
  bool details_include_subject_shapes = false;
  crimson::platform::nvidia::CameraFrameDataAdapter adapter(
      detections, keypoints,
      {[&](size_t frame, size_t lookahead) {
         prefetched_frame = frame;
         prefetched_lookahead = lookahead;
         prefetch_count++;
       },
       [&](size_t frame, bool include_eye_masks, bool include_subject_shapes,
           bool allow_blocking) {
         details_count++;
         details_blocking = allow_blocking;
         details_include_eye_masks = include_eye_masks;
         details_include_subject_shapes = include_subject_shapes;
         ZarrDetectionLoader::FrameDetections details{};
         details.frame_id = frame;
         details.boxes.resize(3);
         return details;
       }});

  crimson::platform::nvidia::CameraFrameDataRequest request;
  request.archive_loaded = true;
  request.frame_selected = true;
  request.query_frame = 42;
  request.source_width = 4512;
  request.source_height = 4512;
  request.resolve_keypoints = true;
  request.load_legacy_details = true;
  request.include_eye_masks = true;
  request.include_subject_shapes = true;
  request.allow_blocking_eye_mask_load = false;
  request.mask_prefetch_lookahead_frames = 700;
  const auto result = adapter.resolve(request);
  CHECK(result.query_frame == 42);
  CHECK(result.detection_frame_ready);
  CHECK(result.detection_frame.observations.size() == 3);
  CHECK(result.source_boxes.size() == 3);
  CHECK(result.keypoint_frame_requested);
  CHECK(result.keypoint_frame.detections.size() == 2);
  CHECK(result.legacy_details_requested);
  CHECK(result.legacy_details_ready);
  CHECK(result.legacy_details.boxes.size() == 3);
  CHECK(result.frame_is_interpolated);
  CHECK(result.dataset_allows_bbox_edit);
  CHECK(result.metrics.stale_results_discarded == 0);
  CHECK(detections.resolved_frame == 42);
  CHECK(keypoints.resolved_frame == 42);
  CHECK(keypoints.resolved_width == 4512);
  CHECK(keypoints.resolved_height == 4512);
  CHECK(prefetch_count == 1);
  CHECK(prefetched_frame == 42);
  CHECK(prefetched_lookahead == 700);
  CHECK(details_count == 1);
  CHECK(!details_blocking);
  CHECK(details_include_eye_masks);
  CHECK(details_include_subject_shapes);

  detections.empty = true;
  request.query_frame = 43;
  request.load_legacy_details = false;
  request.include_eye_masks = false;
  request.include_subject_shapes = false;
  request.allow_blocking_eye_mask_load = true;
  request.mask_prefetch_lookahead_frames = 0;
  const auto empty = adapter.resolve(request);
  CHECK(empty.detection_frame_ready);
  CHECK(empty.detection_frame.observations.empty());
  CHECK(empty.source_boxes.empty());
  CHECK(!empty.legacy_details_ready);
  return true;
}

bool testStaleResultsAreDiscarded() {
  FakeDetectionRepository detections;
  detections.descriptor_value.available = true;
  detections.stale = true;
  FakeKeypointRepository keypoints;
  keypoints.descriptor_value.run_name = "keypoints";
  keypoints.stale = true;
  crimson::platform::nvidia::CameraFrameDataAdapter adapter(
      detections, keypoints, {{}, [](size_t frame, bool, bool, bool) {
                                ZarrDetectionLoader::FrameDetections details{};
                                details.frame_id = frame + 1;
                                details.boxes.resize(1);
                                return details;
                              }});

  crimson::platform::nvidia::CameraFrameDataRequest request;
  request.archive_loaded = true;
  request.frame_selected = true;
  request.query_frame = 75;
  request.source_width = 100;
  request.source_height = 100;
  request.resolve_keypoints = true;
  request.load_legacy_details = true;
  const auto result = adapter.resolve(request);
  CHECK(!result.detection_frame_ready);
  CHECK(result.source_boxes.empty());
  CHECK(result.keypoint_frame.status ==
        crimson::zarr::KeypointOverlayStatus::Missing);
  CHECK(result.keypoint_frame.camera_frame == 75);
  CHECK(!result.legacy_details_ready);
  CHECK(result.legacy_details.frame_id == 75);
  CHECK(result.legacy_details.boxes.empty());
  CHECK(result.metrics.stale_results_discarded == 3);
  return true;
}

bool testInactiveRequestDoesNoPayloadWork() {
  FakeDetectionRepository detections;
  FakeKeypointRepository keypoints;
  keypoints.descriptor_value.run_name = "keypoints";
  int details_count = 0;
  crimson::platform::nvidia::CameraFrameDataAdapter adapter(
      detections, keypoints, {{}, [&](size_t, bool, bool, bool) {
                                details_count++;
                                return ZarrDetectionLoader::FrameDetections{};
                              }});
  crimson::platform::nvidia::CameraFrameDataRequest request;
  request.query_frame = 4;
  request.source_width = 100;
  request.source_height = 100;
  request.resolve_keypoints = true;
  request.load_legacy_details = true;
  request.load_legacy_details_when_keypoints_available = true;
  request.load_legacy_details_for_synthetic_detections = true;
  request.include_eye_masks = true;
  request.include_subject_shapes = true;
  request.allow_blocking_eye_mask_load = false;
  request.mask_prefetch_lookahead_frames = 10;
  auto result = adapter.resolve(request);
  CHECK(!result.detection_frame_ready);
  CHECK(!result.keypoint_frame_requested);
  CHECK(!result.legacy_details_ready);
  CHECK(detections.resolve_count == 0);
  CHECK(keypoints.resolve_count == 0);
  CHECK(details_count == 0);

  request.archive_loaded = true;
  result = adapter.resolve(request);
  CHECK(!result.detection_frame_ready);
  CHECK(detections.resolve_count == 0);
  CHECK(keypoints.resolve_count == 0);
  CHECK(details_count == 0);

  request.frame_selected = true;
  request.query_frame = -1;
  result = adapter.resolve(request);
  CHECK(!result.detection_frame_ready);
  CHECK(detections.resolve_count == 0);
  CHECK(keypoints.resolve_count == 0);
  CHECK(details_count == 0);
  return true;
}

bool testConditionalLegacyDetailPolicy() {
  FakeDetectionRepository detections;
  detections.descriptor_value.available = true;
  FakeKeypointRepository keypoints;
  int details_count = 0;
  crimson::platform::nvidia::CameraFrameDataAdapter adapter(
      detections, keypoints, {{}, [&](size_t frame, bool, bool, bool) {
                                details_count++;
                                ZarrDetectionLoader::FrameDetections details{};
                                details.frame_id = frame;
                                return details;
                              }});

  crimson::platform::nvidia::CameraFrameDataRequest request;
  request.archive_loaded = true;
  request.frame_selected = true;
  request.query_frame = 8;
  request.resolve_keypoints = false;

  auto result = adapter.resolve(request);
  CHECK(!result.legacy_details_requested);
  CHECK(!result.legacy_details_ready);
  CHECK(details_count == 0);

  request.load_legacy_details_when_keypoints_available = true;
  result = adapter.resolve(request);
  CHECK(!result.legacy_details_requested);
  CHECK(details_count == 0);

  keypoints.descriptor_value.run_name = "keypoints";
  result = adapter.resolve(request);
  CHECK(result.legacy_details_requested);
  CHECK(result.legacy_details_ready);
  CHECK(details_count == 1);

  keypoints.descriptor_value.run_name.clear();
  request.load_legacy_details_when_keypoints_available = false;
  request.load_legacy_details_for_synthetic_detections = true;
  result = adapter.resolve(request);
  CHECK(!result.legacy_details_requested);
  CHECK(details_count == 1);

  detections.descriptor_value.active_dataset_has_synthetic_observations = true;
  result = adapter.resolve(request);
  CHECK(result.legacy_details_requested);
  CHECK(result.legacy_details_ready);
  CHECK(details_count == 2);
  return true;
}

} // namespace

int main() {
  if (!testFrameQuerySelection() || !testExactMultiRowAndNonblockingLoad() ||
      !testStaleResultsAreDiscarded() ||
      !testInactiveRequestDoesNoPayloadWork() ||
      !testConditionalLegacyDetailPolicy()) {
    return 1;
  }
  std::cout << "nvidia_camera_frame_data_adapter_tests: PASS\n";
  return 0;
}
