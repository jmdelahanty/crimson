#pragma once

#include <GL/glew.h>

#include <filesystem>
#include <string>

namespace crimson::platform::nvidia {

struct ClippedTextureDumpConfig {
  bool enabled = false;
  int parent_frame = -1;
  std::filesystem::path output_path;
  bool dumped = false;
};

struct GlTextureDumpResult {
  bool ok = false;
  int width = 0;
  int height = 0;
  std::filesystem::path raw_path;
  std::filesystem::path flip_y_path;
  std::string error;
};

struct GlFramebufferDumpResult {
  bool ok = false;
  int width = 0;
  int height = 0;
  std::filesystem::path path;
  std::string error;
};

std::filesystem::path pathWithStemSuffix(const std::filesystem::path &path,
                                         const std::string &suffix);

std::filesystem::path pathWithExtension(const std::filesystem::path &path,
                                        const std::string &extension);

// Captures a GL_TEXTURE_2D texture at level zero. The raw image preserves the
// OpenGL row order; a second vertically flipped image is emitted for display.
// The active texture unit, texture binding, and pack alignment are restored.
GlTextureDumpResult
dumpGlTextureToPng(GLuint texture_id, const std::filesystem::path &output_path);

// Captures the current framebuffer. The output PNG is vertically flipped into
// conventional top-down image order. The read buffer and pixel-pack state are
// restored before this function returns.
GlFramebufferDumpResult
dumpGlBufferToPng(const std::filesystem::path &output_path, int width,
                  int height, GLenum read_buffer);

} // namespace crimson::platform::nvidia
