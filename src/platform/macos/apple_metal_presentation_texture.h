#pragma once

#include "frame_types.h"

#import <Metal/Metal.h>

class AppleMetalPresentationTexture final : public PresentationTexture {
public:
  void reset(id<MTLTexture> texture) {
    texture_ = texture;
    descriptor_.backend = PresentationBackend::Metal;
    descriptor_.width = texture == nil ? 0 : static_cast<int>(texture.width);
    descriptor_.height = texture == nil ? 0 : static_cast<int>(texture.height);
    descriptor_.pixel_format = texture == nil
                                   ? FramePixelFormat::Unknown
                                   : framePixelFormat(texture.pixelFormat);
  }

  const PresentationTextureDescriptor &descriptor() const noexcept override {
    return descriptor_;
  }

  uintptr_t nativeHandle() const noexcept override {
    return reinterpret_cast<uintptr_t>((__bridge void *)texture_);
  }

private:
  static FramePixelFormat framePixelFormat(MTLPixelFormat pixel_format) {
    switch (pixel_format) {
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB:
      return FramePixelFormat::RGBA8;
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatBGRA8Unorm_sRGB:
      return FramePixelFormat::BGRA8;
    default:
      return FramePixelFormat::Unknown;
    }
  }

  id<MTLTexture> texture_ = nil;
  PresentationTextureDescriptor descriptor_;
};
