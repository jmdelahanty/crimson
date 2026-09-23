#include "zarr/legacy_stimulus_repository.h"

#include "zarr_loader.h"

namespace crimson::zarr {

LegacyStimulusRepository::LegacyStimulusRepository(
    const ZarrDetectionLoader &loader)
    : loader_(loader) {
  refreshStrings();
}

void LegacyStimulusRepository::refreshStrings() const {
  run_name_ = loader_.getStimulusRunName();
  source_video_path_ = loader_.getStimulusVideoPath();
  resolved_source_video_path_.clear();
}

const std::string &LegacyStimulusRepository::runName() const {
  refreshStrings();
  return run_name_;
}

const std::string &LegacyStimulusRepository::sourceVideoPath() const {
  refreshStrings();
  return source_video_path_;
}

const std::string &LegacyStimulusRepository::resolvedSourceVideoPath() const {
  refreshStrings();
  return resolved_source_video_path_;
}

size_t LegacyStimulusRepository::cameraFrameCount() const {
  return loader_.getStimulusFrameMappingCount();
}

int64_t LegacyStimulusRepository::cameraFrameOffset() const {
  return loader_.getStimulusCameraFrameOffset();
}

bool LegacyStimulusRepository::hasMapping() const {
  return loader_.hasStimulusFrameMapping();
}

bool LegacyStimulusRepository::hasCorrectedMapping() const {
  return loader_.hasCorrectedStimulusFrameMapping();
}

StimulusFrameResolution LegacyStimulusRepository::resolveCameraFrame(
    int32_t camera_frame, StimulusMappingPreference preference) const {
  return loader_.getStimulusFrameResolution(camera_frame, preference);
}

std::optional<int32_t> LegacyStimulusRepository::metadataIndexForCameraFrame(
    int32_t camera_frame, StimulusMappingPreference preference) const {
  return loader_.getStimulusMetadataIndexForCameraFrame(
      camera_frame, preference == StimulusMappingPreference::PreferCorrected);
}

std::optional<int32_t> LegacyStimulusRepository::cameraFrameForStimulus(
    int32_t stimulus_frame, StimulusMappingPreference preference) const {
  return loader_.getCameraFrameForStimulusFrame(
      stimulus_frame, preference == StimulusMappingPreference::PreferCorrected);
}

std::optional<int32_t> LegacyStimulusRepository::firstCameraFrameWithStimulus(
    StimulusMappingPreference preference) const {
  return loader_.getFirstCameraFrameWithStimulus(
      preference == StimulusMappingPreference::PreferCorrected);
}

std::optional<int32_t> LegacyStimulusRepository::firstStimulusFrame(
    StimulusMappingPreference preference) const {
  return loader_.getFirstStimulusFrameNumber(
      preference == StimulusMappingPreference::PreferCorrected);
}

} // namespace crimson::zarr
