#pragma once

#include "zarr/keypoint_overlay_repository.h"

class ZarrDetectionLoader;

namespace crimson::zarr {

// Compatibility adapter for the eagerly loaded NVIDIA archive session. New
// read-only presentation code should depend on KeypointOverlayRepository.
class LegacyKeypointOverlayRepository final : public KeypointOverlayRepository {
public:
  explicit LegacyKeypointOverlayRepository(const ZarrDetectionLoader &loader);

  const KeypointOverlayDescriptor &descriptor() const override;
  KeypointOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int full_frame_width,
                     int full_frame_height) const override;

private:
  void refreshDescriptor() const;

  const ZarrDetectionLoader &loader_;
  mutable KeypointOverlayDescriptor descriptor_;
};

} // namespace crimson::zarr
