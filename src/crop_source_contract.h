#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace crimson::crop {

enum class CropSourceKind : uint8_t {
  LiveGeometry,
  AcquisitionVideo,
  PersistedZarr,
};

enum class CropSourcePreference : uint8_t {
  PreferLiveGeometry,
  PreferAcquisitionVideo,
  PreferPersistedZarr,
};

enum class CropFallbackPolicy : uint8_t {
  WaitForPreferred,
  AllowFallback,
};

enum class CropSourceSelectionStatus : uint8_t {
  Selected,
  NoCapableSource,
  MissingFrame,
  MissingGeometry,
  AwaitingExactFrame,
  OutOfRange,
  InvalidState,
};

struct CropPoint {
  double x = 0.0;
  double y = 0.0;
};

struct CropRect {
  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;

  bool valid() const;
  bool contains(CropPoint point) const;
  bool contains(const CropRect& other) const;
};

struct CropFrameGeometry {
  int64_t camera_frame = -1;
  std::optional<int64_t> recording_frame_id;
  int source_width = 0;
  int source_height = 0;
  int output_width = 0;
  int output_height = 0;
  CropRect full_frame_crop;
  std::optional<CropRect> full_frame_detection;
  bool geometry_available = false;
  bool has_detection = false;
  bool blank_frame = false;

  bool valid() const;
  bool usableForLiveCrop() const;
  std::optional<CropPoint> fullFrameToCrop(CropPoint point) const;
  std::optional<CropRect> fullFrameToCrop(const CropRect& rect) const;
};

struct CropSourceCapabilities {
  bool live_geometry = false;
  bool acquisition_video = false;
  bool persisted_zarr = false;

  bool supports(CropSourceKind source) const;
  bool any() const;
};

struct AcquisitionCropFrameState {
  std::optional<int64_t> resolved_camera_frame;
  std::optional<int64_t> mapped_video_frame;
  std::optional<int64_t> decoded_video_frame;
  std::optional<CropFrameGeometry> geometry;
  bool blank_frame = false;
};

struct PersistedCropFrameState {
  std::optional<int64_t> resolved_camera_frame;
  std::optional<int64_t> crop_index;
  std::optional<CropFrameGeometry> geometry;
  bool pixels_available = false;
};

struct CropFrameSourceState {
  int64_t camera_frame = -1;
  std::optional<int64_t> exact_full_frame;
  std::optional<CropFrameGeometry> live_geometry;
  AcquisitionCropFrameState acquisition;
  PersistedCropFrameState persisted;
};

struct CropSourceSelection {
  CropSourceSelectionStatus status =
      CropSourceSelectionStatus::NoCapableSource;
  std::optional<CropSourceKind> source;
  int64_t camera_frame = -1;
  std::optional<int64_t> source_frame_index;
  std::optional<CropFrameGeometry> geometry;
  bool blank_frame = false;

  bool selected() const {
    return status == CropSourceSelectionStatus::Selected &&
           source.has_value();
  }
};

CropSourceSelection SelectCropSource(
    const CropSourceCapabilities& capabilities,
    const CropFrameSourceState& state,
    CropSourcePreference preference,
    CropFallbackPolicy fallback_policy,
    std::optional<int64_t> camera_frame_count = std::nullopt);

}  // namespace crimson::crop
