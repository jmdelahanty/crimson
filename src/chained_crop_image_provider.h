#pragma once

#include "crop_image_provider.h"

class ChainedCropImageProvider : public CropImageProvider {
public:
    ChainedCropImageProvider(const CropImageProvider& primary,
                             const CropImageProvider& fallback)
        : primary_(primary), fallback_(fallback) {}

    bool getCropTextureForSpec(const CropSpec& crop_spec,
                               CropTextureView& out_view) const override {
        if (primary_.getCropTextureForSpec(crop_spec, out_view)) {
            return true;
        }
        return fallback_.getCropTextureForSpec(crop_spec, out_view);
    }

    bool getCropTextureForIndex(int32_t roi_index,
                                CropTextureView& out_view) const override {
        if (primary_.getCropTextureForIndex(roi_index, out_view)) {
            return true;
        }
        return fallback_.getCropTextureForIndex(roi_index, out_view);
    }

    bool getCropImageForSpec(const CropSpec& crop_spec,
                             CropImageView& out_view) const override {
        if (primary_.getCropImageForSpec(crop_spec, out_view)) {
            return true;
        }
        return fallback_.getCropImageForSpec(crop_spec, out_view);
    }

    bool getCropImageForIndex(int32_t roi_index,
                              CropImageView& out_view) const override {
        if (primary_.getCropImageForIndex(roi_index, out_view)) {
            return true;
        }
        return fallback_.getCropImageForIndex(roi_index, out_view);
    }

    const std::vector<int32_t>& getCropFrameIndices() const override {
        const auto& primary_frames = primary_.getCropFrameIndices();
        if (!primary_frames.empty()) {
            return primary_frames;
        }
        return fallback_.getCropFrameIndices();
    }

private:
    const CropImageProvider& primary_;
    const CropImageProvider& fallback_;
};
