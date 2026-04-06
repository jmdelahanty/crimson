#pragma once

#include "crop_image_provider.h"
#include "zarr_loader.h"

#include <vector>

class LiveCropImageProvider : public CropImageProvider {
public:
    LiveCropImageProvider(const ZarrDetectionLoader& loader,
                          const CropFrameSource& frame_source);
    ~LiveCropImageProvider();

    bool getCropImageForIndex(int32_t roi_index,
                              CropImageView& out_view) const override;
    const std::vector<int32_t>& getCropFrameIndices() const override;

private:
    bool ensureFrameRgba() const;

    const ZarrDetectionLoader& loader_;
    CropFrameSource frame_source_;
    mutable std::vector<uint8_t> host_rgba_buffer_;
    mutable std::vector<uint8_t> crop_rgba_buffer_;
    mutable uint8_t* device_rgba_buffer_ = nullptr;
    mutable size_t device_rgba_capacity_ = 0;
};
