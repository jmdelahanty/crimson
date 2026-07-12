#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace crimson::zarr {

enum class StimulusMappingPreference : uint8_t {
  PreferCorrected,
  LegacyOnly,
};

enum class StimulusMappingStatus : uint8_t {
  Mapped,
  Missing,
  OutOfRange,
};

enum class StimulusMappingSource : uint8_t {
  None,
  CorrectedDirect,
  CorrectedMetadata,
  LegacyMetadata,
};

struct StimulusFrameResolution {
  StimulusMappingStatus status = StimulusMappingStatus::Missing;
  StimulusMappingSource source = StimulusMappingSource::None;
  int32_t camera_frame = -1;
  std::optional<int32_t> metadata_index;
  std::optional<int32_t> stimulus_frame;
  bool interpolated = false;
};

struct StimulusAlignmentData {
  std::string run_name;
  std::string source_video_path;
  std::string resolved_source_video_path;
  int64_t camera_frame_offset = 0;
  std::vector<int32_t> camera_to_metadata_index;
  std::vector<int32_t> camera_to_metadata_index_corrected;
  std::vector<int32_t> camera_to_stimulus_frame_corrected;
  std::vector<int32_t> frame_metadata_stimulus_frames;
  std::vector<int32_t> frame_metadata_stimulus_frames_corrected;
  std::vector<uint8_t> camera_frame_original;
  std::vector<uint8_t> camera_stimulus_frame_interpolated;
  bool alignment_available = false;
  bool direct_corrected_available = false;
  bool legacy_metadata_available = false;
  bool corrected_metadata_available = false;
};

struct StimulusAlignmentView {
  std::string run_name;
  int64_t camera_frame_offset = 0;
  const std::vector<int32_t>* camera_to_metadata_index = nullptr;
  const std::vector<int32_t>* camera_to_metadata_index_corrected = nullptr;
  const std::vector<int32_t>* camera_to_stimulus_frame_corrected = nullptr;
  const std::vector<int32_t>* frame_metadata_stimulus_frames = nullptr;
  const std::vector<int32_t>* frame_metadata_stimulus_frames_corrected = nullptr;
  const std::vector<uint8_t>* camera_frame_original = nullptr;
  const std::vector<uint8_t>* camera_stimulus_frame_interpolated = nullptr;
  bool alignment_available = false;
  bool direct_corrected_available = false;
  bool legacy_metadata_available = false;
  bool corrected_metadata_available = false;
};

StimulusAlignmentView ViewStimulusAlignment(const StimulusAlignmentData& data);
size_t StimulusCameraFrameCount(const StimulusAlignmentView& alignment);

bool HasStimulusMapping(const StimulusAlignmentView& alignment);
bool HasCorrectedStimulusMapping(const StimulusAlignmentView& alignment);

StimulusFrameResolution ResolveStimulusFrame(
    const StimulusAlignmentView& alignment,
    int32_t camera_frame,
    StimulusMappingPreference preference =
        StimulusMappingPreference::PreferCorrected);

std::optional<int32_t> ResolveStimulusMetadataIndex(
    const StimulusAlignmentView& alignment,
    int32_t camera_frame,
    StimulusMappingPreference preference =
        StimulusMappingPreference::PreferCorrected);

std::optional<int32_t> ResolveCameraFrameForStimulus(
    const StimulusAlignmentView& alignment,
    int32_t stimulus_frame,
    StimulusMappingPreference preference =
        StimulusMappingPreference::PreferCorrected);

std::optional<int32_t> FirstCameraFrameWithStimulus(
    const StimulusAlignmentView& alignment,
    StimulusMappingPreference preference =
        StimulusMappingPreference::PreferCorrected);

std::optional<int32_t> FirstStimulusFrame(
    const StimulusAlignmentView& alignment,
    StimulusMappingPreference preference =
        StimulusMappingPreference::PreferCorrected);

class StimulusRepository {
 public:
  virtual ~StimulusRepository() = default;

  virtual const std::string& runName() const = 0;
  virtual const std::string& sourceVideoPath() const = 0;
  virtual const std::string& resolvedSourceVideoPath() const = 0;
  virtual size_t cameraFrameCount() const = 0;
  virtual int64_t cameraFrameOffset() const = 0;
  virtual bool hasMapping() const = 0;
  virtual bool hasCorrectedMapping() const = 0;
  virtual StimulusFrameResolution resolveCameraFrame(
      int32_t camera_frame,
      StimulusMappingPreference preference =
          StimulusMappingPreference::PreferCorrected) const = 0;
  virtual std::optional<int32_t> metadataIndexForCameraFrame(
      int32_t camera_frame,
      StimulusMappingPreference preference =
          StimulusMappingPreference::PreferCorrected) const = 0;
  virtual std::optional<int32_t> cameraFrameForStimulus(
      int32_t stimulus_frame,
      StimulusMappingPreference preference =
          StimulusMappingPreference::PreferCorrected) const = 0;
  virtual std::optional<int32_t> firstCameraFrameWithStimulus(
      StimulusMappingPreference preference =
          StimulusMappingPreference::PreferCorrected) const = 0;
  virtual std::optional<int32_t> firstStimulusFrame(
      StimulusMappingPreference preference =
          StimulusMappingPreference::PreferCorrected) const = 0;
};

std::unique_ptr<StimulusRepository> MakeStimulusRepository(
    StimulusAlignmentData alignment);

}  // namespace crimson::zarr
