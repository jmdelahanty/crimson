#pragma once
#include "gui/canonical_overlay_session.h"
#include "read_only_overlay_controls.h"

namespace crimson::gui {
struct CanonicalOverlayPresentation {
  CanonicalOverlaySnapshot snapshot;
  zarr::KeypointOverlayResolution keypoints;
  bool keypoints_ready = false;
  overlay::ReadOnlyOverlayScene masks;
  overlay::ReadOnlyOverlayScene shapes;
  overlay::ReadOnlyOverlayScene eyes;
};
CanonicalOverlayPresentation makeCanonicalOverlayPresentation(
    CanonicalOverlaySnapshot snapshot, int view, int64_t presented_frame,
    int source_width, int source_height,
    const overlay::ReadOnlyOverlayControlState& controls);
} // namespace crimson::gui
