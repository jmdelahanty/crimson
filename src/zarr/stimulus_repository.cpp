#include "zarr/stimulus_repository.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace crimson::zarr {
namespace {

const std::vector<int32_t>& Values(const std::vector<int32_t>* values) {
  static const std::vector<int32_t> empty;
  return values ? *values : empty;
}

const std::vector<uint8_t>& Flags(const std::vector<uint8_t>* values) {
  static const std::vector<uint8_t> empty;
  return values ? *values : empty;
}

std::optional<int32_t> Lookup(const std::vector<int32_t>& values,
                              int32_t index) {
  if (index < 0 || static_cast<size_t>(index) >= values.size()) {
    return std::nullopt;
  }
  const int32_t value = values[static_cast<size_t>(index)];
  return value >= 0 ? std::optional<int32_t>(value) : std::nullopt;
}

bool FlagAt(const std::vector<uint8_t>& values, int32_t index) {
  return index >= 0 && static_cast<size_t>(index) < values.size() &&
         values[static_cast<size_t>(index)] != 0;
}

bool CameraFrameInterpolated(const StimulusAlignmentView& alignment,
                             int32_t camera_frame) {
  const auto& original = Flags(alignment.camera_frame_original);
  return camera_frame >= 0 &&
         static_cast<size_t>(camera_frame) < original.size() &&
         original[static_cast<size_t>(camera_frame)] == 0;
}

size_t CameraFrameCount(const StimulusAlignmentView& alignment) {
  return std::max({Values(alignment.camera_to_metadata_index).size(),
                   Values(alignment.camera_to_metadata_index_corrected).size(),
                   Values(alignment.camera_to_stimulus_frame_corrected).size()});
}

class OwnedStimulusRepository final : public StimulusRepository {
 public:
  explicit OwnedStimulusRepository(StimulusAlignmentData alignment)
      : alignment_(std::move(alignment)) {}

  const std::string& runName() const override { return alignment_.run_name; }
  const std::string& sourceVideoPath() const override {
    return alignment_.source_video_path;
  }
  const std::string& resolvedSourceVideoPath() const override {
    return alignment_.resolved_source_video_path;
  }
  size_t cameraFrameCount() const override {
    return StimulusCameraFrameCount(ViewStimulusAlignment(alignment_));
  }
  int64_t cameraFrameOffset() const override {
    return alignment_.camera_frame_offset;
  }
  bool hasMapping() const override {
    return HasStimulusMapping(ViewStimulusAlignment(alignment_));
  }
  bool hasCorrectedMapping() const override {
    return HasCorrectedStimulusMapping(ViewStimulusAlignment(alignment_));
  }
  StimulusFrameResolution resolveCameraFrame(
      int32_t camera_frame,
      StimulusMappingPreference preference) const override {
    return ResolveStimulusFrame(ViewStimulusAlignment(alignment_), camera_frame,
                                preference);
  }
  std::optional<int32_t> metadataIndexForCameraFrame(
      int32_t camera_frame,
      StimulusMappingPreference preference) const override {
    return ResolveStimulusMetadataIndex(ViewStimulusAlignment(alignment_),
                                        camera_frame, preference);
  }
  std::optional<int32_t> cameraFrameForStimulus(
      int32_t stimulus_frame,
      StimulusMappingPreference preference) const override {
    return ResolveCameraFrameForStimulus(ViewStimulusAlignment(alignment_),
                                         stimulus_frame, preference);
  }
  std::optional<int32_t> firstCameraFrameWithStimulus(
      StimulusMappingPreference preference) const override {
    return FirstCameraFrameWithStimulus(ViewStimulusAlignment(alignment_),
                                        preference);
  }
  std::optional<int32_t> firstStimulusFrame(
      StimulusMappingPreference preference) const override {
    return FirstStimulusFrame(ViewStimulusAlignment(alignment_), preference);
  }

 private:
  StimulusAlignmentData alignment_;
};

}  // namespace

StimulusAlignmentView ViewStimulusAlignment(const StimulusAlignmentData& data) {
  return {
      data.run_name,
      data.camera_frame_offset,
      &data.camera_to_metadata_index,
      &data.camera_to_metadata_index_corrected,
      &data.camera_to_stimulus_frame_corrected,
      &data.frame_metadata_stimulus_frames,
      &data.frame_metadata_stimulus_frames_corrected,
      &data.camera_frame_original,
      &data.camera_stimulus_frame_interpolated,
      data.alignment_available,
      data.direct_corrected_available,
      data.legacy_metadata_available,
      data.corrected_metadata_available,
  };
}

size_t StimulusCameraFrameCount(const StimulusAlignmentView& alignment) {
  return CameraFrameCount(alignment);
}

bool HasStimulusMapping(const StimulusAlignmentView& alignment) {
  if (!alignment.alignment_available) {
    return false;
  }
  return alignment.direct_corrected_available ||
         !Values(alignment.camera_to_metadata_index_corrected).empty() ||
         !Values(alignment.camera_to_metadata_index).empty();
}

bool HasCorrectedStimulusMapping(const StimulusAlignmentView& alignment) {
  if (!alignment.alignment_available) {
    return false;
  }
  if (alignment.direct_corrected_available &&
      !Values(alignment.camera_to_stimulus_frame_corrected).empty()) {
    return true;
  }
  return alignment.corrected_metadata_available &&
         !Values(alignment.camera_to_metadata_index_corrected).empty() &&
         !Values(alignment.frame_metadata_stimulus_frames_corrected).empty();
}

StimulusFrameResolution ResolveStimulusFrame(
    const StimulusAlignmentView& alignment,
    int32_t camera_frame,
    StimulusMappingPreference preference) {
  StimulusFrameResolution result;
  result.camera_frame = camera_frame;
  const size_t camera_frame_count = CameraFrameCount(alignment);
  if (camera_frame < 0 ||
      static_cast<size_t>(camera_frame) >= camera_frame_count) {
    result.status = StimulusMappingStatus::OutOfRange;
    return result;
  }

  if (preference == StimulusMappingPreference::PreferCorrected) {
    if (alignment.direct_corrected_available) {
      if (auto frame = Lookup(
              Values(alignment.camera_to_stimulus_frame_corrected),
              camera_frame)) {
        result.status = StimulusMappingStatus::Mapped;
        result.source = StimulusMappingSource::CorrectedDirect;
        result.stimulus_frame = frame;
        result.interpolated = FlagAt(
            Flags(alignment.camera_stimulus_frame_interpolated), camera_frame);
        return result;
      }
    }

    if (alignment.corrected_metadata_available) {
      if (auto metadata = Lookup(
              Values(alignment.camera_to_metadata_index_corrected),
              camera_frame)) {
        if (auto frame = Lookup(
                Values(alignment.frame_metadata_stimulus_frames_corrected),
                *metadata)) {
          result.status = StimulusMappingStatus::Mapped;
          result.source = StimulusMappingSource::CorrectedMetadata;
          result.metadata_index = metadata;
          result.stimulus_frame = frame;
          result.interpolated =
              CameraFrameInterpolated(alignment, camera_frame);
          return result;
        }
      }
    }
  }

  if (alignment.legacy_metadata_available) {
    if (auto metadata = Lookup(Values(alignment.camera_to_metadata_index),
                               camera_frame)) {
      if (auto frame = Lookup(
              Values(alignment.frame_metadata_stimulus_frames), *metadata)) {
        result.status = StimulusMappingStatus::Mapped;
        result.source = StimulusMappingSource::LegacyMetadata;
        result.metadata_index = metadata;
        result.stimulus_frame = frame;
        result.interpolated = CameraFrameInterpolated(alignment, camera_frame);
        return result;
      }
    }
  }

  result.status = StimulusMappingStatus::Missing;
  return result;
}

std::optional<int32_t> ResolveStimulusMetadataIndex(
    const StimulusAlignmentView& alignment,
    int32_t camera_frame,
    StimulusMappingPreference preference) {
  if (!HasStimulusMapping(alignment) || camera_frame < 0) {
    return std::nullopt;
  }
  if (preference == StimulusMappingPreference::PreferCorrected) {
    if (auto corrected = Lookup(
            Values(alignment.camera_to_metadata_index_corrected),
            camera_frame)) {
      return corrected;
    }
  }
  return Lookup(Values(alignment.camera_to_metadata_index), camera_frame);
}

std::optional<int32_t> ResolveCameraFrameForStimulus(
    const StimulusAlignmentView& alignment,
    int32_t stimulus_frame,
    StimulusMappingPreference preference) {
  if (!HasStimulusMapping(alignment) || stimulus_frame < 0) {
    return std::nullopt;
  }

  auto find_direct = [&](const std::vector<int32_t>& frames)
      -> std::optional<int32_t> {
    for (size_t camera_frame = 0; camera_frame < frames.size(); ++camera_frame) {
      if (frames[camera_frame] == stimulus_frame) {
        return static_cast<int32_t>(camera_frame);
      }
    }
    return std::nullopt;
  };
  auto find_metadata = [&](const std::vector<int32_t>& mapping,
                           const std::vector<int32_t>& frames)
      -> std::optional<int32_t> {
    for (size_t camera_frame = 0; camera_frame < mapping.size(); ++camera_frame) {
      const int32_t metadata_index = mapping[camera_frame];
      if (metadata_index >= 0 &&
          static_cast<size_t>(metadata_index) < frames.size() &&
          frames[static_cast<size_t>(metadata_index)] == stimulus_frame) {
        return static_cast<int32_t>(camera_frame);
      }
    }
    return std::nullopt;
  };

  if (preference == StimulusMappingPreference::PreferCorrected) {
    if (alignment.direct_corrected_available) {
      if (auto camera = find_direct(
              Values(alignment.camera_to_stimulus_frame_corrected))) {
        return camera;
      }
    }
    if (alignment.corrected_metadata_available) {
      if (auto camera = find_metadata(
              Values(alignment.camera_to_metadata_index_corrected),
              Values(alignment.frame_metadata_stimulus_frames_corrected))) {
        return camera;
      }
    }
  }
  return find_metadata(Values(alignment.camera_to_metadata_index),
                       Values(alignment.frame_metadata_stimulus_frames));
}

std::optional<int32_t> FirstCameraFrameWithStimulus(
    const StimulusAlignmentView& alignment,
    StimulusMappingPreference preference) {
  if (!HasStimulusMapping(alignment)) {
    return std::nullopt;
  }

  for (size_t camera_frame = 0; camera_frame < CameraFrameCount(alignment);
       ++camera_frame) {
    if (ResolveStimulusFrame(alignment, static_cast<int32_t>(camera_frame),
                             preference)
            .status == StimulusMappingStatus::Mapped) {
      return static_cast<int32_t>(camera_frame);
    }
  }
  return std::nullopt;
}

std::optional<int32_t> FirstStimulusFrame(
    const StimulusAlignmentView& alignment,
    StimulusMappingPreference preference) {
  auto camera_frame = FirstCameraFrameWithStimulus(alignment, preference);
  if (!camera_frame) {
    return std::nullopt;
  }
  return ResolveStimulusFrame(alignment, *camera_frame, preference)
      .stimulus_frame;
}

std::unique_ptr<StimulusRepository> MakeStimulusRepository(
    StimulusAlignmentData alignment) {
  return std::make_unique<OwnedStimulusRepository>(std::move(alignment));
}

}  // namespace crimson::zarr
