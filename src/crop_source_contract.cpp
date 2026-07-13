#include "crop_source_contract.h"

#include <array>
#include <cmath>

namespace crimson::crop {
namespace {

bool Finite(double value) {
  return std::isfinite(value);
}

CropSourceSelection MakeSelection(CropSourceSelectionStatus status,
                                  CropSourceKind source,
                                  int64_t camera_frame) {
  CropSourceSelection selection;
  selection.status = status;
  selection.source = source;
  selection.camera_frame = camera_frame;
  return selection;
}

bool GeometryMatchesFrame(const std::optional<CropFrameGeometry>& geometry,
                          int64_t camera_frame) {
  return !geometry || geometry->camera_frame == camera_frame;
}

CropSourceSelection EvaluateLiveGeometry(
    const CropFrameSourceState& state) {
  auto selection = MakeSelection(CropSourceSelectionStatus::MissingGeometry,
                                 CropSourceKind::LiveGeometry,
                                 state.camera_frame);
  if (!GeometryMatchesFrame(state.live_geometry, state.camera_frame)) {
    selection.status = CropSourceSelectionStatus::InvalidState;
    return selection;
  }
  if (!state.live_geometry || !state.live_geometry->usableForLiveCrop()) {
    return selection;
  }
  if (!state.exact_full_frame ||
      *state.exact_full_frame != state.camera_frame) {
    selection.status = CropSourceSelectionStatus::AwaitingExactFrame;
    return selection;
  }
  selection.status = CropSourceSelectionStatus::Selected;
  selection.source_frame_index = state.camera_frame;
  selection.geometry = state.live_geometry;
  return selection;
}

CropSourceSelection EvaluateAcquisitionVideo(
    const CropFrameSourceState& state) {
  auto selection = MakeSelection(CropSourceSelectionStatus::MissingFrame,
                                 CropSourceKind::AcquisitionVideo,
                                 state.camera_frame);
  if (!GeometryMatchesFrame(state.acquisition.geometry,
                            state.camera_frame)) {
    selection.status = CropSourceSelectionStatus::InvalidState;
    return selection;
  }
  if (!state.acquisition.mapped_video_frame) {
    return selection;
  }
  if (!state.acquisition.resolved_camera_frame ||
      *state.acquisition.resolved_camera_frame != state.camera_frame) {
    selection.status = CropSourceSelectionStatus::InvalidState;
    return selection;
  }
  if (*state.acquisition.mapped_video_frame < 0) {
    selection.status = CropSourceSelectionStatus::InvalidState;
    return selection;
  }
  selection.source_frame_index = state.acquisition.mapped_video_frame;
  if (!state.acquisition.decoded_video_frame ||
      *state.acquisition.decoded_video_frame !=
          *state.acquisition.mapped_video_frame) {
    selection.status = CropSourceSelectionStatus::AwaitingExactFrame;
    return selection;
  }
  if (state.acquisition.geometry &&
      !state.acquisition.geometry->valid()) {
    selection.status = CropSourceSelectionStatus::InvalidState;
    return selection;
  }
  if (state.acquisition.geometry &&
      state.acquisition.geometry->blank_frame !=
          state.acquisition.blank_frame) {
    selection.status = CropSourceSelectionStatus::InvalidState;
    return selection;
  }
  selection.status = CropSourceSelectionStatus::Selected;
  selection.geometry = state.acquisition.geometry;
  selection.blank_frame = state.acquisition.blank_frame;
  return selection;
}

CropSourceSelection EvaluatePersistedZarr(
    const CropFrameSourceState& state) {
  auto selection = MakeSelection(CropSourceSelectionStatus::MissingFrame,
                                 CropSourceKind::PersistedZarr,
                                 state.camera_frame);
  if (!GeometryMatchesFrame(state.persisted.geometry, state.camera_frame)) {
    selection.status = CropSourceSelectionStatus::InvalidState;
    return selection;
  }
  if (!state.persisted.crop_index) {
    return selection;
  }
  if (!state.persisted.resolved_camera_frame ||
      *state.persisted.resolved_camera_frame != state.camera_frame) {
    selection.status = CropSourceSelectionStatus::InvalidState;
    return selection;
  }
  if (*state.persisted.crop_index < 0) {
    selection.status = CropSourceSelectionStatus::InvalidState;
    return selection;
  }
  selection.source_frame_index = state.persisted.crop_index;
  if (!state.persisted.pixels_available) {
    selection.status = CropSourceSelectionStatus::AwaitingExactFrame;
    return selection;
  }
  if (state.persisted.geometry && !state.persisted.geometry->valid()) {
    selection.status = CropSourceSelectionStatus::InvalidState;
    return selection;
  }
  selection.status = CropSourceSelectionStatus::Selected;
  selection.geometry = state.persisted.geometry;
  return selection;
}

CropSourceSelection Evaluate(CropSourceKind source,
                             const CropFrameSourceState& state) {
  switch (source) {
    case CropSourceKind::LiveGeometry:
      return EvaluateLiveGeometry(state);
    case CropSourceKind::AcquisitionVideo:
      return EvaluateAcquisitionVideo(state);
    case CropSourceKind::PersistedZarr:
      return EvaluatePersistedZarr(state);
  }
  return {};
}

std::array<CropSourceKind, 3> SourceOrder(
    CropSourcePreference preference) {
  if (preference == CropSourcePreference::PreferAcquisitionVideo) {
    return {CropSourceKind::AcquisitionVideo,
            CropSourceKind::LiveGeometry,
            CropSourceKind::PersistedZarr};
  }
  if (preference == CropSourcePreference::PreferPersistedZarr) {
    return {CropSourceKind::PersistedZarr,
            CropSourceKind::LiveGeometry,
            CropSourceKind::AcquisitionVideo};
  }
  return {CropSourceKind::LiveGeometry,
          CropSourceKind::AcquisitionVideo,
          CropSourceKind::PersistedZarr};
}

}  // namespace

bool CropRect::valid() const {
  return Finite(x) && Finite(y) && Finite(width) && Finite(height) &&
         width > 0.0 && height > 0.0;
}

bool CropRect::contains(CropPoint point) const {
  return valid() && Finite(point.x) && Finite(point.y) && point.x >= x &&
         point.y >= y && point.x <= x + width && point.y <= y + height;
}

bool CropRect::contains(const CropRect& other) const {
  return valid() && other.valid() && other.x >= x && other.y >= y &&
         other.x + other.width <= x + width &&
         other.y + other.height <= y + height;
}

bool CropFrameGeometry::valid() const {
  if (camera_frame < 0 || source_width <= 0 || source_height <= 0 ||
      output_width <= 0 || output_height <= 0) {
    return false;
  }
  if (recording_frame_id && *recording_frame_id <= 0) {
    return false;
  }
  if ((blank_frame && has_detection) ||
      (blank_frame && geometry_available) ||
      (has_detection && !geometry_available)) {
    return false;
  }
  if (!geometry_available) {
    return true;
  }
  const CropRect source_bounds{0.0, 0.0,
                               static_cast<double>(source_width),
                               static_cast<double>(source_height)};
  if (!full_frame_crop.valid() ||
      !source_bounds.contains(full_frame_crop)) {
    return false;
  }
  if (full_frame_detection &&
      (!full_frame_detection->valid() ||
       !full_frame_crop.contains(*full_frame_detection))) {
    return false;
  }
  return true;
}

bool CropFrameGeometry::usableForLiveCrop() const {
  return valid() && geometry_available && !blank_frame;
}

std::optional<CropPoint> CropFrameGeometry::fullFrameToCrop(
    CropPoint point) const {
  if (!usableForLiveCrop() || !Finite(point.x) || !Finite(point.y)) {
    return std::nullopt;
  }
  const double scale_x =
      static_cast<double>(output_width) / full_frame_crop.width;
  const double scale_y =
      static_cast<double>(output_height) / full_frame_crop.height;
  return CropPoint{(point.x - full_frame_crop.x) * scale_x,
                   (point.y - full_frame_crop.y) * scale_y};
}

std::optional<CropRect> CropFrameGeometry::fullFrameToCrop(
    const CropRect& rect) const {
  if (!usableForLiveCrop() || !rect.valid()) {
    return std::nullopt;
  }
  const auto origin = fullFrameToCrop(CropPoint{rect.x, rect.y});
  if (!origin) {
    return std::nullopt;
  }
  const double scale_x =
      static_cast<double>(output_width) / full_frame_crop.width;
  const double scale_y =
      static_cast<double>(output_height) / full_frame_crop.height;
  return CropRect{origin->x, origin->y,
                  rect.width * scale_x, rect.height * scale_y};
}

bool CropSourceCapabilities::supports(CropSourceKind source) const {
  switch (source) {
    case CropSourceKind::LiveGeometry:
      return live_geometry;
    case CropSourceKind::AcquisitionVideo:
      return acquisition_video;
    case CropSourceKind::PersistedZarr:
      return persisted_zarr;
  }
  return false;
}

bool CropSourceCapabilities::any() const {
  return live_geometry || acquisition_video || persisted_zarr;
}

CropSourceSelection SelectCropSource(
    const CropSourceCapabilities& capabilities,
    const CropFrameSourceState& state,
    CropSourcePreference preference,
    CropFallbackPolicy fallback_policy,
    std::optional<int64_t> camera_frame_count) {
  CropSourceSelection result;
  result.camera_frame = state.camera_frame;
  if (camera_frame_count && *camera_frame_count < 0) {
    result.status = CropSourceSelectionStatus::InvalidState;
    return result;
  }
  if (state.camera_frame < 0 ||
      (camera_frame_count &&
       state.camera_frame >= *camera_frame_count)) {
    result.status = CropSourceSelectionStatus::OutOfRange;
    return result;
  }
  if (!capabilities.any()) {
    return result;
  }

  std::optional<CropSourceSelection> first_capable;
  for (CropSourceKind source : SourceOrder(preference)) {
    if (!capabilities.supports(source)) {
      continue;
    }
    CropSourceSelection candidate = Evaluate(source, state);
    if (!first_capable) {
      first_capable = candidate;
    }
    if (candidate.status == CropSourceSelectionStatus::InvalidState) {
      return candidate;
    }
    if (candidate.selected()) {
      return candidate;
    }
    if (fallback_policy == CropFallbackPolicy::WaitForPreferred) {
      return candidate;
    }
  }
  return first_capable.value_or(result);
}

}  // namespace crimson::crop
