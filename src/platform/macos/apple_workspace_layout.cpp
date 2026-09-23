#include "platform/macos/apple_workspace_layout.h"

#include <algorithm>
#include <cmath>

namespace crimson::macos::workspace {
namespace {

constexpr double kReferenceWidth = 1920.0;
constexpr double kReferenceHeight = 1080.0;

Rect scaled(double scale, double x, double y, double width, double height) {
  return {x * scale, y * scale, width * scale, height * scale};
}

} // namespace

bool Rect::valid() const {
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(width) &&
         std::isfinite(height) && width > 0.0 && height > 0.0;
}

MaintainedWorkspaceLayout makeMaintainedWorkspaceLayout(double width,
                                                        double height) {
  return makeMaintainedWorkspaceLayout(width, height, LayoutProfile::Standard);
}

MaintainedWorkspaceLayout makeMaintainedWorkspaceLayout(
    double width, double height, LayoutProfile profile) {
  MaintainedWorkspaceLayout result;
  if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0 ||
      height <= 0.0) {
    result.scale = 0.0;
    return result;
  }

  result.scale = std::min(width / kReferenceWidth, height / kReferenceHeight);
  result.file_browser = scaled(result.scale, 10.0, 10.0, 560.0, 250.0);
  result.frame_inspect = scaled(result.scale, 10.0, 270.0, 760.0, 800.0);
  result.diagnostics = scaled(result.scale, 580.0, 10.0, 305.0, 250.0);
  result.frames_in_buffer =
      scaled(result.scale, 895.0, 10.0, 500.0, 300.0);
  result.camera = scaled(result.scale, 895.0, 320.0, 500.0, 400.0);
  result.stimulus_event_timeline =
      scaled(result.scale, 1405.0, 10.0, 500.0, 480.0);
  result.analysis_timeline =
      scaled(result.scale, 1405.0, 500.0, 500.0, 560.0);

  // Optional windows retain the maintained sizes but start over their owning
  // workflow rather than becoming permanent layout columns.
  result.advanced_crop_preview =
      scaled(result.scale, 60.0, 60.0, 300.0, 300.0);
  result.stimulus = scaled(result.scale, 60.0, 60.0, 480.0, 360.0);
  result.stimulus_frames_in_buffer =
      scaled(result.scale, 60.0, 60.0, 500.0, 440.0);

  if (profile == LayoutProfile::CropReference) {
    result.diagnostics = scaled(result.scale, 1410.0, 10.0, 500.0, 250.0);
    result.frames_in_buffer =
        scaled(result.scale, 1410.0, 270.0, 500.0, 300.0);
    result.camera = scaled(result.scale, 780.0, 10.0, 620.0, 460.0);
    result.advanced_crop_preview =
        scaled(result.scale, 780.0, 480.0, 420.0, 580.0);
    result.stimulus_event_timeline =
        scaled(result.scale, 1410.0, 580.0, 500.0, 230.0);
    result.analysis_timeline =
        scaled(result.scale, 1410.0, 820.0, 500.0, 240.0);
  } else if (profile == LayoutProfile::StimulusReference) {
    result.diagnostics = scaled(result.scale, 1410.0, 10.0, 500.0, 250.0);
    result.frames_in_buffer =
        scaled(result.scale, 1410.0, 270.0, 500.0, 200.0);
    result.camera = scaled(result.scale, 780.0, 10.0, 620.0, 460.0);
    result.stimulus = scaled(result.scale, 780.0, 480.0, 480.0, 360.0);
    result.stimulus_frames_in_buffer =
        scaled(result.scale, 1270.0, 480.0, 500.0, 440.0);
    result.stimulus_event_timeline =
        scaled(result.scale, 1410.0, 930.0, 500.0, 130.0);
    result.analysis_timeline =
        scaled(result.scale, 780.0, 850.0, 480.0, 210.0);
  }
  return result;
}

Rect fitMedia(Rect bounds, double media_width, double media_height) {
  if (!bounds.valid() || !std::isfinite(media_width) ||
      !std::isfinite(media_height) || media_width <= 0.0 ||
      media_height <= 0.0) {
    return {};
  }
  const double scale =
      std::min(bounds.width / media_width, bounds.height / media_height);
  const double fitted_width = media_width * scale;
  const double fitted_height = media_height * scale;
  return {bounds.x + (bounds.width - fitted_width) * 0.5,
          bounds.y + (bounds.height - fitted_height) * 0.5, fitted_width,
          fitted_height};
}

Rect constrainToBounds(Rect rect, Rect bounds) {
  if (!rect.valid() || !bounds.valid()) {
    return {};
  }
  rect.width = std::min(rect.width, bounds.width);
  rect.height = std::min(rect.height, bounds.height);
  rect.x = std::clamp(rect.x, bounds.x, bounds.right() - rect.width);
  rect.y = std::clamp(rect.y, bounds.y, bounds.bottom() - rect.height);
  return rect;
}

CameraInsetLayout makeCameraInsetLayout(Rect camera_media,
                                        bool include_crop,
                                        bool include_stimulus) {
  CameraInsetLayout result;
  if (!camera_media.valid() || (!include_crop && !include_stimulus)) {
    return result;
  }

  const double short_edge = std::min(camera_media.width, camera_media.height);
  const double margin = std::max(4.0, short_edge * 0.02);
  const double inset_edge = std::max(1.0, short_edge * 0.34);
  const double y = camera_media.bottom() - margin - inset_edge;
  if (include_stimulus) {
    result.stimulus = {camera_media.x + margin, y, inset_edge, inset_edge};
  }
  if (include_crop) {
    result.crop = {camera_media.right() - margin - inset_edge, y, inset_edge,
                   inset_edge};
  }
  return result;
}

} // namespace crimson::macos::workspace
