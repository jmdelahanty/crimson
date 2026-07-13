#include "crop_source_contract.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace {
using crimson::crop::AcquisitionCropFrameState;
using crimson::crop::CropFallbackPolicy;
using crimson::crop::CropFrameGeometry;
using crimson::crop::CropFrameSourceState;
using crimson::crop::CropPoint;
using crimson::crop::CropRect;
using crimson::crop::CropSourceCapabilities;
using crimson::crop::CropSourceKind;
using crimson::crop::CropSourcePreference;
using crimson::crop::CropSourceSelectionStatus;
using crimson::crop::PersistedCropFrameState;
using crimson::crop::SelectCropSource;

#define CHECK(condition)                                                      \
  do {                                                                        \
    if (!(condition)) {                                                       \
      std::cerr << "CHECK failed: " #condition << " at " << __FILE__ << ':' \
                << __LINE__ << '\n';                                         \
      return false;                                                           \
    }                                                                         \
  } while (false)

bool Near(double actual, double expected) {
  return std::abs(actual - expected) < 1e-9;
}

CropFrameGeometry MakeGeometry(int64_t camera_frame) {
  CropFrameGeometry geometry;
  geometry.camera_frame = camera_frame;
  geometry.recording_frame_id = camera_frame + 1;
  geometry.source_width = 4512;
  geometry.source_height = 4512;
  geometry.output_width = 256;
  geometry.output_height = 128;
  geometry.full_frame_crop = CropRect{100.0, 200.0, 512.0, 256.0};
  geometry.full_frame_detection =
      CropRect{228.0, 264.0, 128.0, 64.0};
  geometry.geometry_available = true;
  geometry.has_detection = true;
  return geometry;
}

bool TestGeometryAndScaledTransform() {
  const CropFrameGeometry geometry = MakeGeometry(12);
  CHECK(geometry.valid());
  CHECK(geometry.usableForLiveCrop());
  const auto point = geometry.fullFrameToCrop(CropPoint{356.0, 328.0});
  CHECK(point.has_value());
  CHECK(Near(point->x, 128.0));
  CHECK(Near(point->y, 64.0));
  const auto detection =
      geometry.fullFrameToCrop(*geometry.full_frame_detection);
  CHECK(detection.has_value());
  CHECK(Near(detection->x, 64.0));
  CHECK(Near(detection->y, 32.0));
  CHECK(Near(detection->width, 64.0));
  CHECK(Near(detection->height, 32.0));

  CropFrameGeometry outside = geometry;
  outside.full_frame_crop.x = 4300.0;
  CHECK(!outside.valid());

  CropFrameGeometry escaped_detection = geometry;
  escaped_detection.full_frame_detection =
      CropRect{50.0, 200.0, 128.0, 64.0};
  CHECK(!escaped_detection.valid());

  CropFrameGeometry non_finite = geometry;
  non_finite.full_frame_crop.x =
      std::numeric_limits<double>::quiet_NaN();
  CHECK(!non_finite.valid());

  CropFrameGeometry blank = geometry;
  blank.blank_frame = true;
  blank.geometry_available = false;
  blank.has_detection = false;
  blank.full_frame_detection.reset();
  CHECK(blank.valid());
  CHECK(!blank.usableForLiveCrop());
  CHECK(!blank.fullFrameToCrop(CropPoint{100.0, 200.0}).has_value());

  CropFrameGeometry contradictory = blank;
  contradictory.has_detection = true;
  CHECK(!contradictory.valid());
  return true;
}

bool TestGeometryOnlySelection() {
  CropSourceCapabilities capabilities;
  capabilities.live_geometry = true;
  CropFrameSourceState state;
  state.camera_frame = 12;
  state.exact_full_frame = 12;
  state.live_geometry = MakeGeometry(12);

  auto selected = SelectCropSource(
      capabilities, state, CropSourcePreference::PreferLiveGeometry,
      CropFallbackPolicy::WaitForPreferred, 100);
  CHECK(selected.selected());
  CHECK(selected.source == CropSourceKind::LiveGeometry);
  CHECK(selected.source_frame_index == 12);
  CHECK(selected.geometry.has_value());

  state.exact_full_frame = 11;
  auto waiting = SelectCropSource(
      capabilities, state, CropSourcePreference::PreferLiveGeometry,
      CropFallbackPolicy::WaitForPreferred, 100);
  CHECK(waiting.status ==
        CropSourceSelectionStatus::AwaitingExactFrame);
  CHECK(waiting.source == CropSourceKind::LiveGeometry);

  state.exact_full_frame = 12;
  state.live_geometry.reset();
  auto missing = SelectCropSource(
      capabilities, state, CropSourcePreference::PreferLiveGeometry,
      CropFallbackPolicy::WaitForPreferred, 100);
  CHECK(missing.status == CropSourceSelectionStatus::MissingGeometry);
  return true;
}

bool TestAcquisitionBlankFrameSelection() {
  CropSourceCapabilities capabilities;
  capabilities.acquisition_video = true;
  CropFrameSourceState state;
  state.camera_frame = 47;
  state.acquisition.resolved_camera_frame = 47;
  state.acquisition.mapped_video_frame = 47;
  state.acquisition.decoded_video_frame = 47;
  state.acquisition.blank_frame = true;

  const auto selected = SelectCropSource(
      capabilities, state, CropSourcePreference::PreferAcquisitionVideo,
      CropFallbackPolicy::WaitForPreferred, 100);
  CHECK(selected.selected());
  CHECK(selected.source == CropSourceKind::AcquisitionVideo);
  CHECK(selected.source_frame_index == 47);
  CHECK(selected.blank_frame);
  CHECK(!selected.geometry.has_value());

  state.acquisition.decoded_video_frame = 46;
  const auto waiting = SelectCropSource(
      capabilities, state, CropSourcePreference::PreferAcquisitionVideo,
      CropFallbackPolicy::WaitForPreferred, 100);
  CHECK(waiting.status ==
        CropSourceSelectionStatus::AwaitingExactFrame);
  CHECK(waiting.source_frame_index == 47);
  return true;
}

bool TestPreferenceAndFallbackPolicy() {
  CropSourceCapabilities capabilities;
  capabilities.live_geometry = true;
  capabilities.acquisition_video = true;
  capabilities.persisted_zarr = true;
  CropFrameSourceState state;
  state.camera_frame = 8;
  state.exact_full_frame = 8;
  state.live_geometry = MakeGeometry(8);
  state.acquisition.resolved_camera_frame = 8;
  state.acquisition.mapped_video_frame = 8;
  state.acquisition.decoded_video_frame = 7;
  state.persisted.resolved_camera_frame = 8;
  state.persisted.crop_index = 3;
  state.persisted.pixels_available = true;

  const auto wait_for_video = SelectCropSource(
      capabilities, state, CropSourcePreference::PreferAcquisitionVideo,
      CropFallbackPolicy::WaitForPreferred);
  CHECK(wait_for_video.status ==
        CropSourceSelectionStatus::AwaitingExactFrame);
  CHECK(wait_for_video.source == CropSourceKind::AcquisitionVideo);

  const auto live_fallback = SelectCropSource(
      capabilities, state, CropSourcePreference::PreferAcquisitionVideo,
      CropFallbackPolicy::AllowFallback);
  CHECK(live_fallback.selected());
  CHECK(live_fallback.source == CropSourceKind::LiveGeometry);

  state.acquisition.decoded_video_frame = 8;
  const auto preferred_video = SelectCropSource(
      capabilities, state, CropSourcePreference::PreferAcquisitionVideo,
      CropFallbackPolicy::WaitForPreferred);
  CHECK(preferred_video.selected());
  CHECK(preferred_video.source == CropSourceKind::AcquisitionVideo);

  const auto preferred_live = SelectCropSource(
      capabilities, state, CropSourcePreference::PreferLiveGeometry,
      CropFallbackPolicy::WaitForPreferred);
  CHECK(preferred_live.selected());
  CHECK(preferred_live.source == CropSourceKind::LiveGeometry);

  const auto preferred_persisted = SelectCropSource(
      capabilities, state, CropSourcePreference::PreferPersistedZarr,
      CropFallbackPolicy::WaitForPreferred);
  CHECK(preferred_persisted.selected());
  CHECK(preferred_persisted.source == CropSourceKind::PersistedZarr);
  return true;
}

bool TestPersistedFallbackAndFailures() {
  CropSourceCapabilities capabilities;
  capabilities.persisted_zarr = true;
  CropFrameSourceState state;
  state.camera_frame = 20;
  state.persisted.resolved_camera_frame = 20;
  state.persisted.crop_index = 4;
  state.persisted.pixels_available = true;

  auto selected = SelectCropSource(
      capabilities, state, CropSourcePreference::PreferLiveGeometry,
      CropFallbackPolicy::WaitForPreferred, 21);
  CHECK(selected.selected());
  CHECK(selected.source == CropSourceKind::PersistedZarr);
  CHECK(selected.source_frame_index == 4);

  state.persisted.pixels_available = false;
  auto waiting = SelectCropSource(
      capabilities, state, CropSourcePreference::PreferLiveGeometry,
      CropFallbackPolicy::WaitForPreferred, 21);
  CHECK(waiting.status ==
        CropSourceSelectionStatus::AwaitingExactFrame);

  state.persisted.crop_index.reset();
  auto missing = SelectCropSource(
      capabilities, state, CropSourcePreference::PreferLiveGeometry,
      CropFallbackPolicy::WaitForPreferred, 21);
  CHECK(missing.status == CropSourceSelectionStatus::MissingFrame);

  state.camera_frame = 21;
  auto out_of_range = SelectCropSource(
      capabilities, state, CropSourcePreference::PreferLiveGeometry,
      CropFallbackPolicy::WaitForPreferred, 21);
  CHECK(out_of_range.status == CropSourceSelectionStatus::OutOfRange);

  state.camera_frame = 0;
  auto invalid_count = SelectCropSource(
      capabilities, state, CropSourcePreference::PreferLiveGeometry,
      CropFallbackPolicy::WaitForPreferred, -1);
  CHECK(invalid_count.status == CropSourceSelectionStatus::InvalidState);

  CropSourceCapabilities none;
  auto unavailable = SelectCropSource(
      none, state, CropSourcePreference::PreferLiveGeometry,
      CropFallbackPolicy::WaitForPreferred);
  CHECK(unavailable.status ==
        CropSourceSelectionStatus::NoCapableSource);
  CHECK(!unavailable.source.has_value());
  return true;
}

bool TestInvalidFrameIdentity() {
  CropSourceCapabilities capabilities;
  capabilities.live_geometry = true;
  capabilities.acquisition_video = true;
  capabilities.persisted_zarr = true;
  CropFrameSourceState state;
  state.camera_frame = 5;
  state.exact_full_frame = 5;
  state.live_geometry = MakeGeometry(4);

  auto invalid_live = SelectCropSource(
      capabilities, state, CropSourcePreference::PreferLiveGeometry,
      CropFallbackPolicy::WaitForPreferred);
  CHECK(invalid_live.status == CropSourceSelectionStatus::InvalidState);

  state.live_geometry = MakeGeometry(5);
  state.acquisition.resolved_camera_frame = 5;
  state.acquisition.mapped_video_frame = -1;
  auto invalid_video = SelectCropSource(
      capabilities, state, CropSourcePreference::PreferAcquisitionVideo,
      CropFallbackPolicy::WaitForPreferred);
  CHECK(invalid_video.status == CropSourceSelectionStatus::InvalidState);

  state.acquisition.mapped_video_frame = 5;
  state.acquisition.decoded_video_frame = 5;
  state.acquisition.blank_frame = true;
  state.acquisition.geometry = MakeGeometry(5);
  auto contradictory_blank = SelectCropSource(
      capabilities, state, CropSourcePreference::PreferAcquisitionVideo,
      CropFallbackPolicy::AllowFallback);
  CHECK(contradictory_blank.status ==
        CropSourceSelectionStatus::InvalidState);
  CHECK(contradictory_blank.source ==
        CropSourceKind::AcquisitionVideo);

  state.acquisition.geometry.reset();
  state.acquisition.blank_frame = false;
  state.acquisition.resolved_camera_frame = 4;
  auto stale_resolution = SelectCropSource(
      capabilities, state, CropSourcePreference::PreferAcquisitionVideo,
      CropFallbackPolicy::AllowFallback);
  CHECK(stale_resolution.status ==
        CropSourceSelectionStatus::InvalidState);
  CHECK(stale_resolution.source ==
        CropSourceKind::AcquisitionVideo);
  return true;
}

}  // namespace

int main() {
  if (!TestGeometryAndScaledTransform() ||
      !TestGeometryOnlySelection() ||
      !TestAcquisitionBlankFrameSelection() ||
      !TestPreferenceAndFallbackPolicy() ||
      !TestPersistedFallbackAndFailures() ||
      !TestInvalidFrameIdentity()) {
    return 1;
  }
  std::cout << "crop_source_contract_tests: PASS\n";
  return 0;
}
