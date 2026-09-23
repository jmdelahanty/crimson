#include "zarr/stimulus_repository.h"

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

bool testCorrectedAndLegacyResolution() {
  crimson::zarr::StimulusAlignmentData data;
  data.run_name = "fixture";
  data.alignment_available = true;
  data.direct_corrected_available = true;
  data.legacy_metadata_available = true;
  data.camera_to_stimulus_frame_corrected = {10, 11, -1, 13};
  data.camera_stimulus_frame_interpolated = {0, 1, 0, 0};
  data.camera_to_metadata_index = {0, 1, 2, 3};
  data.frame_metadata_stimulus_frames = {20, 21, 22, 23};

  auto repository = crimson::zarr::MakeStimulusRepository(std::move(data));
  CHECK(repository != nullptr);
  CHECK(repository->hasMapping());
  CHECK(repository->hasCorrectedMapping());

  auto corrected = repository->resolveCameraFrame(
      1, crimson::zarr::StimulusMappingPreference::PreferCorrected);
  CHECK(corrected.status == crimson::zarr::StimulusMappingStatus::Mapped);
  CHECK(corrected.source ==
        crimson::zarr::StimulusMappingSource::CorrectedDirect);
  CHECK(corrected.stimulus_frame == 11);
  CHECK(corrected.interpolated);

  auto fallback = repository->resolveCameraFrame(
      2, crimson::zarr::StimulusMappingPreference::PreferCorrected);
  CHECK(fallback.status == crimson::zarr::StimulusMappingStatus::Mapped);
  CHECK(fallback.source ==
        crimson::zarr::StimulusMappingSource::LegacyMetadata);
  CHECK(fallback.stimulus_frame == 22);

  auto legacy = repository->resolveCameraFrame(
      1, crimson::zarr::StimulusMappingPreference::LegacyOnly);
  CHECK(legacy.source == crimson::zarr::StimulusMappingSource::LegacyMetadata);
  CHECK(legacy.stimulus_frame == 21);
  return true;
}

bool testOptionalFrameHelper() {
  CHECK(!crimson::zarr::StimulusFrameForCamera(nullptr, 0).has_value());

  crimson::zarr::StimulusAlignmentData data;
  data.alignment_available = true;
  data.direct_corrected_available = true;
  data.camera_to_stimulus_frame_corrected = {5, -1, 7};
  auto repository = crimson::zarr::MakeStimulusRepository(std::move(data));
  CHECK(crimson::zarr::StimulusFrameForCamera(repository.get(), 0) == 5);
  CHECK(
      !crimson::zarr::StimulusFrameForCamera(repository.get(), 1).has_value());
  CHECK(
      !crimson::zarr::StimulusFrameForCamera(repository.get(), 8).has_value());
  return true;
}

} // namespace

int main() {
  if (!testCorrectedAndLegacyResolution() || !testOptionalFrameHelper()) {
    return 1;
  }
  std::cout << "stimulus_repository_contract_tests: PASS\n";
  return 0;
}
