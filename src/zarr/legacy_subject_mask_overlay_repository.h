#pragma once

#include "zarr/subject_mask_overlay_repository.h"

#include <functional>

class ZarrDetectionLoader;

namespace crimson::zarr {

// Compatibility adapter for the NVIDIA archive session. New read-only
// presentation code should depend on SubjectMaskOverlayRepository.
class LegacySubjectMaskOverlayRepository final
    : public SubjectMaskOverlayRepository {
public:
  using AllowBlockingLoad = std::function<bool()>;

  explicit LegacySubjectMaskOverlayRepository(
      const ZarrDetectionLoader &loader,
      AllowBlockingLoad allow_blocking_load = {});

  const SubjectMaskOverlayDescriptor &descriptor() const override;
  SubjectMaskOverlayResolution
  resolveCameraFrame(int64_t camera_frame, int full_frame_width,
                     int full_frame_height) const override;

private:
  void refreshDescriptor() const;

  const ZarrDetectionLoader &loader_;
  AllowBlockingLoad allow_blocking_load_;
  mutable SubjectMaskOverlayDescriptor descriptor_;
};

} // namespace crimson::zarr
