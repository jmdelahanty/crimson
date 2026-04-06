#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct CropImageView {
    const uint8_t* data = nullptr;
    size_t width = 0;
    size_t height = 0;
    size_t channels = 0;
};

class CropImageProvider {
public:
    virtual ~CropImageProvider() = default;

    virtual bool getCropImageForIndex(int32_t roi_index,
                                      CropImageView& out_view) const = 0;
    virtual const std::vector<int32_t>& getCropFrameIndices() const = 0;
};
