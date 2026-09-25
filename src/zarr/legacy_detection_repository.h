#pragma once

#include "zarr/detection_repository.h"

class ZarrDetectionLoader;

namespace crimson::zarr {

// Compatibility adapter for the eagerly loaded NVIDIA archive session. New
// presentation and review code should depend on DetectionRepository instead.
class LegacyDetectionRepository final : public DetectionRepository {
public:
  explicit LegacyDetectionRepository(ZarrDetectionLoader &loader);

  DetectionRepositoryDescriptor descriptor() const override;
  std::vector<DetectionDatasetOption> availableDatasets() const override;
  bool selectDataset(DetectionDataset dataset) override;
  bool isDatasetAvailable(DetectionDataset dataset) const override;
  size_t observationCount(size_t frame_id) const override;
  bool isFrameInterpolated(size_t frame_id) const override;
  DetectionFrame resolveFrame(size_t frame_id,
                              bool use_interpolated = false) const override;

private:
  ZarrDetectionLoader &loader_;
};

} // namespace crimson::zarr
