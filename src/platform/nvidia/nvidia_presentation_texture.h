#pragma once

#include "frame_types.h"

class NvidiaOpenGlPresentationTexture final : public PresentationTexture {
  public:
    void reset(uintptr_t handle, int width, int height,
               FramePixelFormat pixel_format = FramePixelFormat::RGBA8) {
        handle_ = handle;
        descriptor_.backend = PresentationBackend::OpenGL;
        descriptor_.pixel_format = pixel_format;
        descriptor_.width = width;
        descriptor_.height = height;
    }

    const PresentationTextureDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    uintptr_t nativeHandle() const noexcept override { return handle_; }

  private:
    uintptr_t handle_ = 0;
    PresentationTextureDescriptor descriptor_;
};
