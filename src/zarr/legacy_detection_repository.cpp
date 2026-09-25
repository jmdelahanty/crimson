#include "zarr/legacy_detection_repository.h"

#include "zarr_loader.h"

#include <utility>

namespace crimson::zarr {

LegacyDetectionRepository::LegacyDetectionRepository(
    ZarrDetectionLoader &loader)
    : loader_(loader) {}

DetectionRepositoryDescriptor LegacyDetectionRepository::descriptor() const {
  DetectionRepositoryDescriptor value;
  value.archive_path = loader_.getArchivePath();
  value.run_name = loader_.getDetectRunName();
  value.interpolation_method = loader_.getInterpolationMethod();
  value.total_frames = loader_.getTotalFrames();
  value.maximum_observations_per_frame = loader_.getMaxDetections();
  value.frames_per_second = loader_.getFPS();
  value.source_width = loader_.getImageWidth();
  value.source_height = loader_.getImageHeight();
  value.active_dataset = loader_.getActiveDetectionDataset();
  value.available = loader_.hasDetectionData();
  value.has_scores = loader_.hasScores();
  value.has_class_ids = loader_.hasClassIDs();
  value.coordinates_normalized = loader_.coordinatesAreNormalized();
  value.interpolation_available = loader_.hasInterpolation();
  value.clipped_collection = loader_.hasClippedCollection();
  value.active_dataset_has_synthetic_observations =
      loader_.activeDatasetHasSyntheticDetections();
  return value;
}

std::vector<DetectionDatasetOption>
LegacyDetectionRepository::availableDatasets() const {
  std::vector<DetectionDatasetOption> result;
  const auto legacy_options = loader_.getAvailableDetectionDatasets();
  result.reserve(legacy_options.size());
  for (const auto &[dataset, label] : legacy_options) {
    result.push_back({dataset, label});
  }
  return result;
}

bool LegacyDetectionRepository::selectDataset(DetectionDataset dataset) {
  return loader_.setActiveDetectionDataset(dataset);
}

bool LegacyDetectionRepository::isDatasetAvailable(
    DetectionDataset dataset) const {
  return loader_.isDatasetAvailable(dataset);
}

size_t LegacyDetectionRepository::observationCount(size_t frame_id) const {
  const int32_t count = loader_.getDetectionsForFrame(frame_id);
  return count > 0 ? static_cast<size_t>(count) : 0;
}

bool LegacyDetectionRepository::isFrameInterpolated(size_t frame_id) const {
  return loader_.isFrameInterpolated(frame_id);
}

DetectionFrame
LegacyDetectionRepository::resolveFrame(size_t frame_id,
                                        bool use_interpolated) const {
  DetectionFrame result;
  result.frame_id = frame_id;
  const DetectionRepositoryDescriptor current = descriptor();
  if (!current.available) {
    return result;
  }
  if (frame_id >= current.total_frames) {
    result.status = DetectionFrameStatus::OutOfRange;
    return result;
  }

  const auto legacy = loader_.getRawDetections(frame_id, use_interpolated,
                                               false, false, true, true);
  result.status = DetectionFrameStatus::Ready;
  result.interpolated = legacy.is_interpolated;
  result.observations.reserve(legacy.boxes.size());
  for (size_t index = 0; index < legacy.boxes.size(); ++index) {
    DetectionObservation observation;
    observation.ordinal = index;
    observation.box_xyxy = legacy.boxes[index];
    if (index < legacy.scores.size()) {
      observation.score = legacy.scores[index];
      observation.score_valid = true;
    }
    if (index < legacy.class_ids.size()) {
      observation.class_id = legacy.class_ids[index];
      observation.class_id_valid = true;
    }
    if (index < legacy.detection_source.size()) {
      observation.source_kind = legacy.detection_source[index];
    }
    if (index < legacy.detection_reason.size()) {
      observation.reason = legacy.detection_reason[index];
    }
    result.observations.push_back(std::move(observation));
  }
  return result;
}

} // namespace crimson::zarr
