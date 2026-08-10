#include "gui/camera_view_overlay_renderer.h"

#include "imgui.h"
#include "implot.h"

#include <GL/glew.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

double durationMs(std::chrono::steady_clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

struct TextureEntry {
  std::string key;
  size_t width = 0;
  size_t height = 0;
  GLuint texture_id = 0;
  uint64_t last_used = 0;
};

class ReadOnlyMaskTextureCache {
public:
  GLuint getOrCreate(const crimson::overlay::RasterMask &mask,
                     CameraViewMaskPerfMetrics *perf) {
    if (mask.cache_key.empty() || mask.width == 0 || mask.height == 0 ||
        !mask.alpha ||
        mask.width > std::numeric_limits<size_t>::max() / mask.height ||
        mask.width >
            static_cast<size_t>(std::numeric_limits<GLsizei>::max()) ||
        mask.height >
            static_cast<size_t>(std::numeric_limits<GLsizei>::max()) ||
        mask.width * mask.height >
            std::numeric_limits<size_t>::max() / size_t{4} ||
        mask.alpha->size() != mask.width * mask.height) {
      return 0;
    }
    ++clock_;
    const auto lookup_start = std::chrono::steady_clock::now();
    for (auto &entry : entries_) {
      if (entry.key == mask.cache_key && entry.width == mask.width &&
          entry.height == mask.height) {
        entry.last_used = clock_;
        if (perf != nullptr) {
          perf->texture_cache_hits++;
          perf->texture_lookup_ms +=
              durationMs(std::chrono::steady_clock::now() - lookup_start);
        }
        return entry.texture_id;
      }
    }

    if (perf != nullptr) {
      perf->texture_cache_misses++;
      perf->texture_lookup_ms +=
          durationMs(std::chrono::steady_clock::now() - lookup_start);
    }

    const auto upload_start = std::chrono::steady_clock::now();
    const GLuint texture = createTexture(mask);
    if (perf != nullptr) {
      perf->texture_upload_ms +=
          durationMs(std::chrono::steady_clock::now() - upload_start);
    }
    if (texture == 0) {
      return 0;
    }
    if (perf != nullptr) {
      perf->texture_uploads++;
    }
    evictIfNeeded();
    entries_.push_back(
        {mask.cache_key, mask.width, mask.height, texture, clock_});
    return texture;
  }

private:
  static constexpr size_t kCapacity = 64;

  static uint8_t colorComponent(float value) {
    return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
  }

  static GLuint createTexture(const crimson::overlay::RasterMask &mask) {
    std::vector<uint8_t> rgba(mask.width * mask.height * 4, 0);
    const uint8_t red = colorComponent(mask.color.red);
    const uint8_t green = colorComponent(mask.color.green);
    const uint8_t blue = colorComponent(mask.color.blue);
    const uint8_t alpha = colorComponent(mask.color.alpha);
    for (size_t index = 0; index < mask.alpha->size(); ++index) {
      if ((*mask.alpha)[index] == 0) {
        continue;
      }
      const size_t base = index * 4;
      rgba[base] = red;
      rgba[base + 1] = green;
      rgba[base + 2] = blue;
      rgba[base + 3] = alpha;
    }

    GLint previous_texture = 0;
    GLint previous_unpack_alignment = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_unpack_alignment);
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(mask.width),
                 static_cast<GLsizei>(mask.height), 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba.data());
    const GLenum error = glGetError();
    glPixelStorei(GL_UNPACK_ALIGNMENT, previous_unpack_alignment);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
    if (error != GL_NO_ERROR) {
      glDeleteTextures(1, &texture);
      return 0;
    }
    return texture;
  }

  void evictIfNeeded() {
    while (entries_.size() >= kCapacity) {
      const auto oldest =
          std::min_element(entries_.begin(), entries_.end(),
                           [](const auto &left, const auto &right) {
                             return left.last_used < right.last_used;
                           });
      if (oldest == entries_.end()) {
        return;
      }
      if (oldest->texture_id != 0) {
        glDeleteTextures(1, &oldest->texture_id);
      }
      entries_.erase(oldest);
    }
  }

  std::vector<TextureEntry> entries_;
  uint64_t clock_ = 0;
};

ReadOnlyMaskTextureCache &maskTextureCache() {
  static ReadOnlyMaskTextureCache cache;
  return cache;
}

} // namespace

CameraViewMaskPerfMetrics drawCameraViewReadOnlyRasterMasks(
    const crimson::overlay::ReadOnlyOverlayScene &scene,
    float image_height_px) {
  CameraViewMaskPerfMetrics perf;
  perf.attempted = true;
  const auto total_start = std::chrono::steady_clock::now();
  if (!scene.ready()) {
    return perf;
  }
  for (const auto &mask : scene.raster_masks) {
    if (!mask.source_rect.valid()) {
      continue;
    }
    const GLuint texture = maskTextureCache().getOrCreate(mask, &perf);
    if (texture == 0) {
      continue;
    }
    const double x_min = mask.source_rect.x;
    const double x_max = mask.source_rect.x + mask.source_rect.width;
    const double y_min = static_cast<double>(image_height_px) -
                         (mask.source_rect.y + mask.source_rect.height);
    const double y_max =
        static_cast<double>(image_height_px) - mask.source_rect.y;
    const std::string plot_id = "##read_only_mask_" + mask.cache_key;
    const auto draw_start = std::chrono::steady_clock::now();
    ImPlot::PlotImage(plot_id.c_str(), (ImTextureID)(intptr_t)texture,
                      ImPlotPoint(x_min, y_min), ImPlotPoint(x_max, y_max),
                      ImVec2(0, 0), ImVec2(1, 1), ImVec4(1, 1, 1, 1));
    perf.fill_draw_ms +=
        durationMs(std::chrono::steady_clock::now() - draw_start);
    perf.component_fill_count++;
  }
  perf.total_draw_ms =
      durationMs(std::chrono::steady_clock::now() - total_start);
  return perf;
}
