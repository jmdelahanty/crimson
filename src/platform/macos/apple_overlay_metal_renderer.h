#pragma once

#include "read_only_overlay_scene.h"

#include <cstdint>
#include <memory>
#include <string>

class AppleOverlayMetalRenderer {
  public:
    AppleOverlayMetalRenderer();
    ~AppleOverlayMetalRenderer();

    AppleOverlayMetalRenderer(const AppleOverlayMetalRenderer&) = delete;
    AppleOverlayMetalRenderer& operator=(const AppleOverlayMetalRenderer&) =
        delete;

    bool initialize(uintptr_t metal_device, uint64_t drawable_pixel_format,
                    std::string* error = nullptr);
    void reset();
    bool isInitialized() const;

    // A non-ready scene is intentionally withheld and succeeds without drawing.
    bool encode(const crimson::overlay::ReadOnlyOverlayScene& scene,
                const crimson::overlay::SourceViewportTransform& transform,
                uintptr_t metal_render_encoder,
                uint32_t drawable_width,
                uint32_t drawable_height,
                std::string* error = nullptr);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
