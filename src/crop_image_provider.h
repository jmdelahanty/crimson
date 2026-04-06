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

enum class CropFrameStorage {
    None = 0,
    HostRGBA32,
    DeviceRGBA32,
    DeviceNV12,
};

struct CropFrameSource {
    const uint8_t* frame = nullptr;
    int frame_number = -1;
    int width = 0;
    int height = 0;
    int pitch_bytes = 0;
    int color_matrix = 0;
    CropFrameStorage storage = CropFrameStorage::None;

    bool valid() const {
        return frame != nullptr && frame_number >= 0 && width > 0 &&
               height > 0 && storage != CropFrameStorage::None;
    }
};

class CropImageProvider {
public:
    virtual ~CropImageProvider() = default;

    virtual bool getCropImageForIndex(int32_t roi_index,
                                      CropImageView& out_view) const = 0;
    virtual const std::vector<int32_t>& getCropFrameIndices() const = 0;
};
