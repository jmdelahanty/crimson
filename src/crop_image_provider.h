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

struct CropTextureView {
    unsigned int source_texture_id = 0;
    int source_texture_width = 0;
    int source_texture_height = 0;
    CropRect source_rect;
    CropRect destination_rect;
    int output_width = 0;
    int output_height = 0;
    CropImageView::Origin origin = CropImageView::Origin::Unknown;

    bool valid() const {
        return source_texture_id != 0 && source_texture_width > 0 &&
               source_texture_height > 0 && source_rect.valid() &&
               destination_rect.valid() && output_width > 0 &&
               output_height > 0;
    }
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
    unsigned int texture_id = 0;
    int texture_width = 0;
    int texture_height = 0;
    int texture_frame_number = -1;

    bool valid() const {
        return frame != nullptr && frame_number >= 0 && width > 0 &&
               height > 0 && storage != CropFrameStorage::None;
    }

    bool textureValid() const {
        return texture_id != 0 && texture_width > 0 && texture_height > 0 &&
               texture_frame_number == frame_number;
    }
};

class CropImageProvider {
public:
    virtual ~CropImageProvider() = default;

    virtual bool getCropTextureForIndex(int32_t roi_index,
                                        CropTextureView& out_view) const {
        out_view = {};
        return false;
    }
    virtual bool getCropImageForIndex(int32_t roi_index,
                                      CropImageView& out_view) const = 0;
    virtual const std::vector<int32_t>& getCropFrameIndices() const = 0;
};
