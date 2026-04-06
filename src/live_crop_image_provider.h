#pragma once

#include "crop_image_provider.h"
#include "zarr_loader.h"

#include <vector>

class LiveCropImageProvider : public CropImageProvider {
public:
    LiveCropImageProvider(const ZarrDetectionLoader& loader,
                          const CropFrameSource& frame_source);
    ~LiveCropImageProvider();

    bool getCropTextureForIndex(int32_t roi_index,
                                CropTextureView& out_view) const override;
    bool getCropImageForIndex(int32_t roi_index,
                              CropImageView& out_view) const override;
    const std::vector<int32_t>& getCropFrameIndices() const override;

private:
    bool resolveCropSpec(int32_t roi_index, CropSpec& out_spec) const;
    bool ensureDeviceBuffer(uint8_t*& buffer,
                            size_t& capacity,
                            size_t required_bytes) const;
    bool fillCropFromHostRgba(const CropRect& crop_rect) const;
    bool fillCropFromDeviceRgba(const CropRect& crop_rect) const;
    bool fillCropFromDeviceNv12(const CropRect& crop_rect) const;

    const ZarrDetectionLoader& loader_;
    CropFrameSource frame_source_;
    mutable std::vector<uint8_t> crop_rgba_buffer_;
    mutable uint8_t* device_rgba_buffer_ = nullptr;
    mutable size_t device_rgba_capacity_ = 0;
    mutable uint8_t* device_nv12_buffer_ = nullptr;
    mutable size_t device_nv12_capacity_ = 0;
};
