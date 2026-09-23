#pragma once

#include "zarr/stimulus_repository.h"

class ZarrDetectionLoader;

namespace crimson::zarr {

// Compatibility adapter for the eagerly loaded NVIDIA archive session. New
// alignment consumers should depend on StimulusRepository.
class LegacyStimulusRepository final : public StimulusRepository {
public:
  explicit LegacyStimulusRepository(const ZarrDetectionLoader &loader);

  const std::string &runName() const override;
  const std::string &sourceVideoPath() const override;
  const std::string &resolvedSourceVideoPath() const override;
  size_t cameraFrameCount() const override;
  int64_t cameraFrameOffset() const override;
  bool hasMapping() const override;
  bool hasCorrectedMapping() const override;
  StimulusFrameResolution
  resolveCameraFrame(int32_t camera_frame,
                     StimulusMappingPreference preference) const override;
  std::optional<int32_t> metadataIndexForCameraFrame(
      int32_t camera_frame,
      StimulusMappingPreference preference) const override;
  std::optional<int32_t>
  cameraFrameForStimulus(int32_t stimulus_frame,
                         StimulusMappingPreference preference) const override;
  std::optional<int32_t> firstCameraFrameWithStimulus(
      StimulusMappingPreference preference) const override;
  std::optional<int32_t>
  firstStimulusFrame(StimulusMappingPreference preference) const override;

private:
  void refreshStrings() const;

  const ZarrDetectionLoader &loader_;
  mutable std::string run_name_;
  mutable std::string source_video_path_;
  mutable std::string resolved_source_video_path_;
};

} // namespace crimson::zarr
