#include "live_crop_image_provider.h"

#include "ColorSpace.h"
#include "NvCodecUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

int alignDownEven(int value) {
    return value & ~1;
}

int alignUpEven(int value) {
    return (value + 1) & ~1;
}

struct CropCopyRegion {
    int copy_x0 = 0;
    int copy_y0 = 0;
    int copy_x1 = 0;
    int copy_y1 = 0;
    int dst_x = 0;
    int dst_y = 0;

    int width() const { return copy_x1 - copy_x0; }
    int height() const { return copy_y1 - copy_y0; }
    bool valid() const { return width() > 0 && height() > 0; }
};

CropCopyRegion resolveCopyRegion(const CropRect& crop_rect,
                                 int frame_width,
                                 int frame_height) {
    CropCopyRegion region;
    region.copy_x0 = std::max(0, crop_rect.x);
    region.copy_y0 = std::max(0, crop_rect.y);
    region.copy_x1 = std::min(frame_width, crop_rect.x + crop_rect.width);
    region.copy_y1 = std::min(frame_height, crop_rect.y + crop_rect.height);
    region.dst_x = region.copy_x0 - crop_rect.x;
    region.dst_y = region.copy_y0 - crop_rect.y;
    return region;
}

}  // namespace

LiveCropImageProvider::LiveCropImageProvider(const ZarrDetectionLoader& loader,
                                             const CropFrameSource& frame_source)
    : loader_(loader), frame_source_(frame_source) {}

LiveCropImageProvider::~LiveCropImageProvider() {
    if (device_rgba_buffer_ != nullptr) {
        cudaFree(device_rgba_buffer_);
        device_rgba_buffer_ = nullptr;
        device_rgba_capacity_ = 0;
    }
    if (device_nv12_buffer_ != nullptr) {
        cudaFree(device_nv12_buffer_);
        device_nv12_buffer_ = nullptr;
        device_nv12_capacity_ = 0;
    }
}

bool LiveCropImageProvider::ensureDeviceBuffer(uint8_t*& buffer,
                                               size_t& capacity,
                                               size_t required_bytes) const {
    if (capacity >= required_bytes && buffer != nullptr) {
        return true;
    }
    if (buffer != nullptr) {
        cudaFree(buffer);
        buffer = nullptr;
        capacity = 0;
    }
    ck(cudaMalloc(reinterpret_cast<void**>(&buffer), required_bytes));
    capacity = required_bytes;
    return true;
}

bool LiveCropImageProvider::resolveCropSpec(int32_t roi_index,
                                            CropSpec& out_spec) const {
    out_spec = {};
    const auto metadata = loader_.getCropRoiMetadataForRoiIndex(roi_index);
    if (!metadata.valid || !metadata.has_crop_metadata) {
        return false;
    }
    out_spec.roi_index = roi_index;
    out_spec.offset_x = metadata.offset_x;
    out_spec.offset_y = metadata.offset_y;
    out_spec.width_px = metadata.roi_width;
    out_spec.height_px = metadata.roi_height;
    out_spec.valid = true;
    return true;
}

bool LiveCropImageProvider::getCropTextureForIndex(
    int32_t roi_index,
    CropTextureView& out_view) const {
    out_view = {};
    if (!frame_source_.textureValid()) {
        return false;
    }

    CropSpec crop_spec;
    if (!resolveCropSpec(roi_index, crop_spec)) {
        return false;
    }
    const CropRect crop_rect = crop_spec.toPixelRect();
    if (!crop_rect.valid()) {
        return false;
    }

    const CropCopyRegion overlap =
        resolveCopyRegion(crop_rect, frame_source_.width, frame_source_.height);
    if (!overlap.valid()) {
        return false;
    }

    const double scale_x =
        static_cast<double>(frame_source_.texture_width) / frame_source_.width;
    const double scale_y =
        static_cast<double>(frame_source_.texture_height) / frame_source_.height;

    CropRect source_rect;
    source_rect.x = std::clamp(
        static_cast<int>(std::floor(static_cast<double>(overlap.copy_x0) * scale_x)),
        0,
        std::max(0, frame_source_.texture_width - 1));
    source_rect.y = std::clamp(
        static_cast<int>(std::floor(static_cast<double>(overlap.copy_y0) * scale_y)),
        0,
        std::max(0, frame_source_.texture_height - 1));
    const int source_x1 = std::clamp(
        static_cast<int>(std::ceil(static_cast<double>(overlap.copy_x1) * scale_x)),
        source_rect.x + 1,
        frame_source_.texture_width);
    const int source_y1 = std::clamp(
        static_cast<int>(std::ceil(static_cast<double>(overlap.copy_y1) * scale_y)),
        source_rect.y + 1,
        frame_source_.texture_height);
    source_rect.width = source_x1 - source_rect.x;
    source_rect.height = source_y1 - source_rect.y;

    out_view.source_texture_id = frame_source_.texture_id;
    out_view.source_texture_width = frame_source_.texture_width;
    out_view.source_texture_height = frame_source_.texture_height;
    out_view.source_rect = source_rect;
    out_view.destination_rect = {
        overlap.dst_x,
        overlap.dst_y,
        overlap.width(),
        overlap.height(),
    };
    out_view.output_width = crop_rect.width;
    out_view.output_height = crop_rect.height;
    out_view.origin = CropImageView::Origin::LiveFrame;
    return out_view.valid();
}

bool LiveCropImageProvider::fillCropFromHostRgba(
    const CropRect& crop_rect) const {
    const size_t crop_bytes = static_cast<size_t>(crop_rect.width) *
                              static_cast<size_t>(crop_rect.height) * 4;
    crop_rgba_buffer_.assign(crop_bytes, 0);

    const CropCopyRegion region =
        resolveCopyRegion(crop_rect, frame_source_.width, frame_source_.height);
    if (!region.valid()) {
        return true;
    }

    const int frame_stride =
        frame_source_.pitch_bytes > 0 ? frame_source_.pitch_bytes
                                      : frame_source_.width * 4;
    for (int row = 0; row < region.height(); ++row) {
        const uint8_t* src =
            frame_source_.frame +
            static_cast<size_t>(region.copy_y0 + row) * frame_stride +
            static_cast<size_t>(region.copy_x0) * 4;
        uint8_t* dst =
            crop_rgba_buffer_.data() +
            (static_cast<size_t>(region.dst_y + row) * crop_rect.width +
             static_cast<size_t>(region.dst_x)) *
                4;
        std::memcpy(dst, src, static_cast<size_t>(region.width()) * 4);
    }
    return true;
}

bool LiveCropImageProvider::fillCropFromDeviceRgba(
    const CropRect& crop_rect) const {
    const size_t crop_bytes = static_cast<size_t>(crop_rect.width) *
                              static_cast<size_t>(crop_rect.height) * 4;
    crop_rgba_buffer_.assign(crop_bytes, 0);

    const CropCopyRegion region =
        resolveCopyRegion(crop_rect, frame_source_.width, frame_source_.height);
    if (!region.valid()) {
        return true;
    }

    const int frame_stride =
        frame_source_.pitch_bytes > 0 ? frame_source_.pitch_bytes
                                      : frame_source_.width * 4;
    const uint8_t* src =
        frame_source_.frame +
        static_cast<size_t>(region.copy_y0) * frame_stride +
        static_cast<size_t>(region.copy_x0) * 4;
    uint8_t* dst =
        crop_rgba_buffer_.data() +
        (static_cast<size_t>(region.dst_y) * crop_rect.width +
         static_cast<size_t>(region.dst_x)) *
            4;
    ck(cudaMemcpy2D(dst,
                    static_cast<size_t>(crop_rect.width) * 4,
                    src,
                    frame_stride,
                    static_cast<size_t>(region.width()) * 4,
                    region.height(),
                    cudaMemcpyDeviceToHost));
    return true;
}

bool LiveCropImageProvider::fillCropFromDeviceNv12(
    const CropRect& crop_rect) const {
    const size_t crop_bytes = static_cast<size_t>(crop_rect.width) *
                              static_cast<size_t>(crop_rect.height) * 4;
    crop_rgba_buffer_.assign(crop_bytes, 0);

    const CropCopyRegion region =
        resolveCopyRegion(crop_rect, frame_source_.width, frame_source_.height);
    if (!region.valid()) {
        return true;
    }

    const int source_pitch =
        frame_source_.pitch_bytes > 0 ? frame_source_.pitch_bytes
                                      : frame_source_.width;

    int aligned_x0 = alignDownEven(region.copy_x0);
    int aligned_y0 = alignDownEven(region.copy_y0);
    int aligned_x1 = alignUpEven(region.copy_x1);
    int aligned_y1 = alignUpEven(region.copy_y1);

    if (aligned_x1 > frame_source_.width) {
        aligned_x1 = frame_source_.width;
        if ((aligned_x1 - aligned_x0) % 2 != 0 && aligned_x0 > 0) {
            --aligned_x0;
        }
    }
    if (aligned_y1 > frame_source_.height) {
        aligned_y1 = frame_source_.height;
        if ((aligned_y1 - aligned_y0) % 2 != 0 && aligned_y0 > 0) {
            --aligned_y0;
        }
    }

    const int aligned_width = aligned_x1 - aligned_x0;
    const int aligned_height = aligned_y1 - aligned_y0;
    if (aligned_width <= 0 || aligned_height <= 0 ||
        (aligned_width % 2) != 0 || (aligned_height % 2) != 0) {
        return false;
    }

    const size_t nv12_bytes = static_cast<size_t>(aligned_width) *
                              static_cast<size_t>(aligned_height +
                                                  aligned_height / 2);
    const size_t rgba_bytes = static_cast<size_t>(aligned_width) *
                              static_cast<size_t>(aligned_height) * 4;
    ensureDeviceBuffer(device_nv12_buffer_, device_nv12_capacity_, nv12_bytes);
    ensureDeviceBuffer(device_rgba_buffer_, device_rgba_capacity_, rgba_bytes);

    ck(cudaMemset(device_nv12_buffer_, 0, nv12_bytes));

    const uint8_t* luma_src =
        frame_source_.frame +
        static_cast<size_t>(aligned_y0) * source_pitch +
        static_cast<size_t>(aligned_x0);
    ck(cudaMemcpy2D(device_nv12_buffer_,
                    aligned_width,
                    luma_src,
                    source_pitch,
                    aligned_width,
                    aligned_height,
                    cudaMemcpyDeviceToDevice));

    const uint8_t* chroma_base =
        frame_source_.frame +
        static_cast<size_t>(source_pitch) * frame_source_.height;
    const uint8_t* chroma_src =
        chroma_base +
        static_cast<size_t>(aligned_y0 / 2) * source_pitch +
        static_cast<size_t>(aligned_x0);
    uint8_t* chroma_dst =
        device_nv12_buffer_ +
        static_cast<size_t>(aligned_width) * aligned_height;
    ck(cudaMemcpy2D(chroma_dst,
                    aligned_width,
                    chroma_src,
                    source_pitch,
                    aligned_width,
                    aligned_height / 2,
                    cudaMemcpyDeviceToDevice));

    Nv12ToColor32<RGBA32>(device_nv12_buffer_,
                          aligned_width,
                          device_rgba_buffer_,
                          aligned_width * 4,
                          aligned_width,
                          aligned_height,
                          frame_source_.color_matrix);

    const int sub_x = region.copy_x0 - aligned_x0;
    const int sub_y = region.copy_y0 - aligned_y0;
    const uint8_t* rgba_src =
        device_rgba_buffer_ +
        static_cast<size_t>(sub_y) * aligned_width * 4 +
        static_cast<size_t>(sub_x) * 4;
    uint8_t* dst =
        crop_rgba_buffer_.data() +
        (static_cast<size_t>(region.dst_y) * crop_rect.width +
         static_cast<size_t>(region.dst_x)) *
            4;
    ck(cudaMemcpy2D(dst,
                    static_cast<size_t>(crop_rect.width) * 4,
                    rgba_src,
                    static_cast<size_t>(aligned_width) * 4,
                    static_cast<size_t>(region.width()) * 4,
                    region.height(),
                    cudaMemcpyDeviceToHost));
    return true;
}

bool LiveCropImageProvider::getCropImageForIndex(int32_t roi_index,
                                                 CropImageView& out_view) const {
    out_view = {};
    if (!frame_source_.valid() || frame_source_.frame_number < 0) {
        return false;
    }

    CropSpec crop_spec;
    if (!resolveCropSpec(roi_index, crop_spec)) {
        return false;
    }
    const CropRect crop_rect = crop_spec.toPixelRect();
    if (!crop_rect.valid()) {
        return false;
    }

    switch (frame_source_.storage) {
    case CropFrameStorage::HostRGBA32:
        if (!fillCropFromHostRgba(crop_rect)) {
            return false;
        }
        break;
    case CropFrameStorage::DeviceRGBA32:
        if (!fillCropFromDeviceRgba(crop_rect)) {
            return false;
        }
        break;
    case CropFrameStorage::DeviceNV12:
        if (!fillCropFromDeviceNv12(crop_rect)) {
            return false;
        }
        break;
    case CropFrameStorage::None:
    default:
        return false;
    }

    out_view.data = crop_rgba_buffer_.data();
    out_view.width = static_cast<size_t>(crop_rect.width);
    out_view.height = static_cast<size_t>(crop_rect.height);
    out_view.channels = 4;
    out_view.origin = CropImageView::Origin::LiveFrame;
    return true;
}

const std::vector<int32_t>& LiveCropImageProvider::getCropFrameIndices() const {
    static const std::vector<int32_t> kEmpty;
    return kEmpty;
}
