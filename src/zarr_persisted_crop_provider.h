#pragma once

#include "crop_image_provider.h"
#include "zarr_loader.h"

class ZarrPersistedCropProvider : public CropImageProvider {
public:
    explicit ZarrPersistedCropProvider(const ZarrDetectionLoader& loader)
        : loader_(loader) {}

    bool getCropImageForIndex(int32_t roi_index,
                              CropImageView& out_view) const override;
    const std::vector<int32_t>& getCropFrameIndices() const override;

private:
    const ZarrDetectionLoader& loader_;
};
