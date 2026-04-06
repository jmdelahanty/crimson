#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <vector>

struct CropRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool valid() const { return width > 0 && height > 0; }
};

struct CropSpec {
    int32_t roi_index = -1;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float width_px = 0.0f;
    float height_px = 0.0f;
    bool valid = false;

    CropRect toPixelRect() const {
        CropRect rect;
        if (!valid) {
            return rect;
        }
        rect.x = static_cast<int>(std::lround(static_cast<double>(offset_x)));
        rect.y = static_cast<int>(std::lround(static_cast<double>(offset_y)));
        rect.width =
            std::max(1, static_cast<int>(std::lround(static_cast<double>(width_px))));
        rect.height =
            std::max(1, static_cast<int>(std::lround(static_cast<double>(height_px))));
        return rect;
    }
};

struct CropImageView {
    const uint8_t* data = nullptr;
    size_t width = 0;
    size_t height = 0;
    size_t channels = 0;
    enum class Origin {
        Unknown = 0,
        LiveFrame,
        PersistedZarr,
    } origin = Origin::Unknown;
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
