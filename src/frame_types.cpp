#include "frame_types.h"

#include <algorithm>

std::array<FramePlaneLayout, kMaxFramePlanes>
describeFramePlanes(FramePixelFormat pixel_format, int width, int height,
                    int row_stride_bytes, size_t* plane_count) {
    std::array<FramePlaneLayout, kMaxFramePlanes> planes{};
    if (plane_count != nullptr) {
        *plane_count = 0;
    }
    if (width <= 0 || height <= 0 || row_stride_bytes <= 0) {
        return planes;
    }

    if (pixel_format == FramePixelFormat::NV12) {
        planes[0] = {0, row_stride_bytes, width, height, 1};
        planes[1] = {static_cast<size_t>(row_stride_bytes) *
                         static_cast<size_t>(height),
                     row_stride_bytes, (width + 1) / 2, (height + 1) / 2, 2};
        if (plane_count != nullptr) {
            *plane_count = 2;
        }
        return planes;
    }

    if (pixel_format == FramePixelFormat::RGBA8 ||
        pixel_format == FramePixelFormat::BGRA8) {
        planes[0] = {0, row_stride_bytes, width, height, 4};
        if (plane_count != nullptr) {
            *plane_count = 1;
        }
    }
    return planes;
}

bool frameMetadataHasValidLayout(const DecodedFrameMetadata& metadata) {
    if (metadata.width <= 0 || metadata.height <= 0 ||
        metadata.plane_count == 0 || metadata.plane_count > kMaxFramePlanes ||
        metadata.pixel_format == FramePixelFormat::Unknown) {
        return false;
    }

    size_t required_bytes = 0;
    for (size_t i = 0; i < metadata.plane_count; ++i) {
        const FramePlaneLayout& plane = metadata.planes[i];
        if (plane.row_stride_bytes <= 0 || plane.width_pixels <= 0 ||
            plane.height_pixels <= 0 || plane.bytes_per_element <= 0 ||
            plane.row_stride_bytes <
                plane.width_pixels * plane.bytes_per_element) {
            return false;
        }
        const size_t plane_end =
            plane.offset_bytes + static_cast<size_t>(plane.row_stride_bytes) *
                                     static_cast<size_t>(plane.height_pixels);
        required_bytes = std::max(required_bytes, plane_end);
    }
    return metadata.frame_bytes == 0 || metadata.frame_bytes >= required_bytes;
}

FrameSurfaceDescriptor
frameSurfaceDescriptorFromMetadata(const DecodedFrameMetadata& metadata) {
    FrameSurfaceDescriptor descriptor;
    descriptor.backend = metadata.surface_backend;
    descriptor.pixel_format = metadata.pixel_format;
    descriptor.width = metadata.width;
    descriptor.height = metadata.height;
    descriptor.planes = metadata.planes;
    descriptor.plane_count = metadata.plane_count;
    descriptor.allocation_bytes = metadata.frame_bytes;
    descriptor.ownership = metadata.ownership;
    descriptor.lifetime = metadata.lifetime;
    return descriptor;
}
