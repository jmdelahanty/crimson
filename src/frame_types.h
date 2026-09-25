#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

typedef enum ColorSpaceStandard {
    ColorSpaceStandard_BT709 = 1,
    ColorSpaceStandard_Unspecified = 2,
    ColorSpaceStandard_Reserved = 3,
    ColorSpaceStandard_FCC = 4,
    ColorSpaceStandard_BT470 = 5,
    ColorSpaceStandard_BT601 = 6,
    ColorSpaceStandard_SMPTE240M = 7,
    ColorSpaceStandard_YCgCo = 8,
    ColorSpaceStandard_BT2020 = 9,
    ColorSpaceStandard_BT2020C = 10
} ColorSpaceStandard;

// Values intentionally match FFmpeg AVColorRange without importing FFmpeg.
typedef enum ColorRange {
    ColorRange_Unspecified = 0,
    ColorRange_MPEG = 1,
    ColorRange_JPEG = 2
} ColorRange;

inline bool colorRangeIsFull(int color_range) {
    return color_range == ColorRange_JPEG;
}

enum class FramePixelFormat : uint8_t {
    Unknown = 0,
    NV12,
    RGBA8,
    BGRA8,

    // Compatibility name for the existing NVIDIA code.
    RGBA32 = RGBA8,
};

using PictureBufferFormat = FramePixelFormat;

enum class FrameSurfaceBackend : uint8_t {
    Unknown = 0,
    Cpu,
    NvidiaCuda,
    AppleVideoToolbox,
};

enum class PresentationBackend : uint8_t {
    Unknown = 0,
    OpenGL,
    Metal,
};

enum class FrameSurfaceOwnership : uint8_t {
    Unspecified = 0,
    Borrowed,
    SlotOwned,
    ReferenceCounted,
};

enum class FrameSurfaceLifetime : uint8_t {
    Unspecified = 0,
    External,
    UntilReadLeaseReleased,
    ReferenceCounted,
};

struct FrameTimeBase {
    int32_t numerator = 0;
    int32_t denominator = 1;

    bool isValid() const { return numerator > 0 && denominator > 0; }
};

struct FramePlaneLayout {
    size_t offset_bytes = 0;
    int row_stride_bytes = 0;
    int width_pixels = 0;
    int height_pixels = 0;
    int bytes_per_element = 0;
};

constexpr size_t kMaxFramePlanes = 3;

struct DecodedFrameMetadata {
    std::string stream_id;

    // frame_number is the recording/global identity. local_frame_number is the
    // decoder or clipped-media identity.
    int frame_number = -1;
    int local_frame_number = -1;
    int64_t frame_pts = -1;
    FrameTimeBase time_base;
    int frame_source_code = 0;

    int width = 0;
    int height = 0;
    FramePixelFormat pixel_format = FramePixelFormat::Unknown;
    std::array<FramePlaneLayout, kMaxFramePlanes> planes{};
    size_t plane_count = 0;
    int pitch_bytes = 0;
    size_t frame_bytes = 0;

    int color_matrix = ColorSpaceStandard_BT709;
    int color_range = ColorRange_Unspecified;

    FrameSurfaceBackend surface_backend = FrameSurfaceBackend::Unknown;
    FrameSurfaceOwnership ownership = FrameSurfaceOwnership::Unspecified;
    FrameSurfaceLifetime lifetime = FrameSurfaceLifetime::Unspecified;
};

using FrameSlotMetadata = DecodedFrameMetadata;

struct FrameSurfaceDescriptor {
    FrameSurfaceBackend backend = FrameSurfaceBackend::Unknown;
    FramePixelFormat pixel_format = FramePixelFormat::Unknown;
    int width = 0;
    int height = 0;
    std::array<FramePlaneLayout, kMaxFramePlanes> planes{};
    size_t plane_count = 0;
    size_t allocation_bytes = 0;
    FrameSurfaceOwnership ownership = FrameSurfaceOwnership::Unspecified;
    FrameSurfaceLifetime lifetime = FrameSurfaceLifetime::Unspecified;
};

class FrameSurface {
  public:
    virtual ~FrameSurface() = default;
    virtual const FrameSurfaceDescriptor& descriptor() const noexcept = 0;
    virtual uintptr_t nativeHandle(size_t plane_index = 0) const noexcept = 0;
};

struct PresentationTextureDescriptor {
    PresentationBackend backend = PresentationBackend::Unknown;
    FramePixelFormat pixel_format = FramePixelFormat::Unknown;
    int width = 0;
    int height = 0;
};

class PresentationTexture {
  public:
    virtual ~PresentationTexture() = default;
    virtual const PresentationTextureDescriptor&
    descriptor() const noexcept = 0;
    virtual uintptr_t nativeHandle() const noexcept = 0;
};

std::array<FramePlaneLayout, kMaxFramePlanes>
describeFramePlanes(FramePixelFormat pixel_format, int width, int height,
                    int row_stride_bytes, size_t* plane_count);

bool frameMetadataHasValidLayout(const DecodedFrameMetadata& metadata);

FrameSurfaceDescriptor
frameSurfaceDescriptorFromMetadata(const DecodedFrameMetadata& metadata);

struct FrameSlotState;

// Legacy storage object retained by the NVIDIA implementation. Its public
// fields remain mirrored for existing diagnostics while publication and
// lifetime are governed by frame_slot.h.
struct PictureBuffer {
    unsigned char* frame;
    int frame_number;
    int local_frame_number;
    int64_t frame_pts;
    int frame_source_code;
    bool available_to_write;
    int pitch_bytes;
    size_t frame_bytes;
    int color_matrix;
    int color_range;
    PictureBufferFormat format;
    FrameSlotState* frame_slot_state;
};

static_assert(
    std::is_trivial<PictureBuffer>::value,
    "PictureBuffer must remain compatible with legacy malloc storage");
