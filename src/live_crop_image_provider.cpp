#include "live_crop_image_provider.h"

#include "ColorSpace.h"
#include "NvCodecUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

int roundToPixel(float value) {
    return static_cast<int>(std::lround(static_cast<double>(value)));
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
}

bool LiveCropImageProvider::ensureFrameRgba() const {
    if (!frame_source_.valid()) {
        return false;
    }

    const size_t rgba_bytes =
        static_cast<size_t>(frame_source_.width) *
        static_cast<size_t>(frame_source_.height) * 4;

    if (frame_source_.storage == CropFrameStorage::HostRGBA32) {
        return true;
    }

    host_rgba_buffer_.resize(rgba_bytes);
    if (frame_source_.storage == CropFrameStorage::DeviceRGBA32) {
        ck(cudaMemcpy2D(host_rgba_buffer_.data(),
                        static_cast<size_t>(frame_source_.width) * 4,
                        frame_source_.frame,
                        frame_source_.pitch_bytes > 0
                            ? frame_source_.pitch_bytes
                            : frame_source_.width * 4,
                        static_cast<size_t>(frame_source_.width) * 4,
                        frame_source_.height,
                        cudaMemcpyDeviceToHost));
        return true;
    }

    if (frame_source_.storage != CropFrameStorage::DeviceNV12) {
        return false;
    }

    if (device_rgba_capacity_ < rgba_bytes) {
        if (device_rgba_buffer_ != nullptr) {
            cudaFree(device_rgba_buffer_);
            device_rgba_buffer_ = nullptr;
            device_rgba_capacity_ = 0;
        }
        ck(cudaMalloc(reinterpret_cast<void**>(&device_rgba_buffer_), rgba_bytes));
        device_rgba_capacity_ = rgba_bytes;
    }

    Nv12ToColor32<RGBA32>(
        const_cast<uint8_t*>(frame_source_.frame),
        frame_source_.pitch_bytes > 0 ? frame_source_.pitch_bytes
                                      : frame_source_.width,
        device_rgba_buffer_,
        frame_source_.width * 4,
        frame_source_.width,
        frame_source_.height,
        frame_source_.color_matrix);
    ck(cudaMemcpy(host_rgba_buffer_.data(),
                  device_rgba_buffer_,
                  rgba_bytes,
                  cudaMemcpyDeviceToHost));
    return true;
}

bool LiveCropImageProvider::getCropImageForIndex(int32_t roi_index,
                                                 CropImageView& out_view) const {
    out_view = {};
    if (!frame_source_.valid() ||
        frame_source_.frame_number < 0) {
        return false;
    }

    const auto metadata = loader_.getCropRoiMetadataForRoiIndex(roi_index);
    if (!metadata.valid || !metadata.has_crop_metadata) {
        return false;
    }

    if (!ensureFrameRgba()) {
        return false;
    }

    const uint8_t* frame_rgba =
        frame_source_.storage == CropFrameStorage::HostRGBA32
            ? frame_source_.frame
            : host_rgba_buffer_.data();
    if (frame_rgba == nullptr) {
        return false;
    }

    const int crop_x = roundToPixel(metadata.offset_x);
    const int crop_y = roundToPixel(metadata.offset_y);
    const int crop_width = std::max(1, roundToPixel(metadata.roi_width));
    const int crop_height = std::max(1, roundToPixel(metadata.roi_height));
    const size_t crop_bytes =
        static_cast<size_t>(crop_width) * static_cast<size_t>(crop_height) * 4;
    crop_rgba_buffer_.assign(crop_bytes, 0);

    const int frame_stride =
        frame_source_.storage == CropFrameStorage::HostRGBA32
            ? (frame_source_.pitch_bytes > 0 ? frame_source_.pitch_bytes
                                             : frame_source_.width * 4)
            : frame_source_.width * 4;

    const int copy_x0 = std::max(0, crop_x);
    const int copy_y0 = std::max(0, crop_y);
    const int copy_x1 = std::min(frame_source_.width, crop_x + crop_width);
    const int copy_y1 = std::min(frame_source_.height, crop_y + crop_height);
    if (copy_x0 >= copy_x1 || copy_y0 >= copy_y1) {
        out_view.data = crop_rgba_buffer_.data();
        out_view.width = static_cast<size_t>(crop_width);
        out_view.height = static_cast<size_t>(crop_height);
        out_view.channels = 4;
        return true;
    }

    const int copy_width = copy_x1 - copy_x0;
    for (int row = copy_y0; row < copy_y1; ++row) {
        const int src_row = row;
        const int dst_row = row - crop_y;
        const int dst_x = copy_x0 - crop_x;
        const uint8_t* src =
            frame_rgba + static_cast<size_t>(src_row) * frame_stride +
            static_cast<size_t>(copy_x0) * 4;
        uint8_t* dst = crop_rgba_buffer_.data() +
                       static_cast<size_t>(dst_row) * crop_width * 4 +
                       static_cast<size_t>(dst_x) * 4;
        std::memcpy(dst, src, static_cast<size_t>(copy_width) * 4);
    }

    out_view.data = crop_rgba_buffer_.data();
    out_view.width = static_cast<size_t>(crop_width);
    out_view.height = static_cast<size_t>(crop_height);
    out_view.channels = 4;
    return true;
}

const std::vector<int32_t>& LiveCropImageProvider::getCropFrameIndices() const {
    static const std::vector<int32_t> kEmpty;
    return kEmpty;
}
