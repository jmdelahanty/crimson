#pragma once

#include "frame_types.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct AppleVideoAssetInfo {
    std::string path;
    std::string stream_id;
    int width = 0;
    int height = 0;
    int64_t frame_count = 0;
    double nominal_frame_rate = 0.0;
    double duration_seconds = 0.0;
    int color_matrix = ColorSpaceStandard_BT709;
    int color_range = ColorRange_Unspecified;
};

struct AppleDecodedVideoFrame {
    DecodedFrameMetadata metadata;
    std::shared_ptr<const FrameSurface> surface;

    explicit operator bool() const {
        return surface != nullptr && metadata.frame_number >= 0;
    }
};

class AppleVideoFrameProvider {
  public:
    AppleVideoFrameProvider();
    ~AppleVideoFrameProvider();

    AppleVideoFrameProvider(const AppleVideoFrameProvider&) = delete;
    AppleVideoFrameProvider& operator=(const AppleVideoFrameProvider&) = delete;
    AppleVideoFrameProvider(AppleVideoFrameProvider&&) noexcept;
    AppleVideoFrameProvider& operator=(AppleVideoFrameProvider&&) noexcept;

    bool open(const std::string& path, const std::string& stream_id,
              std::string* error = nullptr);
    void close();
    void suspendDecoding();

    bool isOpen() const;
    const AppleVideoAssetInfo& info() const;

    bool seekToFrame(int64_t frame_number, std::string* error = nullptr);
    std::optional<AppleDecodedVideoFrame>
    readNext(std::string* error = nullptr);
    std::optional<AppleDecodedVideoFrame>
    readFrame(int64_t frame_number, std::string* error = nullptr);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
