#pragma once

#include "platform/macos/apple_video_frame_provider.h"

#include <cstdint>
#include <memory>
#include <string>

struct AppleMetalVideoViewport {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

class AppleVideoMetalRenderer {
  public:
    AppleVideoMetalRenderer();
    ~AppleVideoMetalRenderer();

    AppleVideoMetalRenderer(const AppleVideoMetalRenderer&) = delete;
    AppleVideoMetalRenderer& operator=(const AppleVideoMetalRenderer&) = delete;

    bool initialize(uintptr_t metal_device, uint64_t drawable_pixel_format,
                    std::string* error = nullptr);
    void reset();
    bool isInitialized() const;

    bool encode(const AppleDecodedVideoFrame& frame,
                uintptr_t metal_command_buffer,
                uintptr_t metal_render_encoder,
                const AppleMetalVideoViewport& viewport,
                std::string* error = nullptr);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
