#include "platform/nvidia/nvidia_gl_diagnostics.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <iomanip>
#include <sstream>
#include <system_error>
#include <vector>

namespace crimson::platform::nvidia {

std::filesystem::path pathWithStemSuffix(const std::filesystem::path &path,
                                         const std::string &suffix) {
  const std::filesystem::path parent = path.parent_path();
  const std::string stem = path.stem().string();
  const std::string extension = path.extension().string();
  return parent / (stem + suffix + extension);
}

std::filesystem::path pathWithExtension(const std::filesystem::path &path,
                                        const std::string &extension) {
  std::filesystem::path result = path;
  result.replace_extension(extension);
  return result;
}

GlTextureDumpResult
dumpGlTextureToPng(GLuint texture_id,
                   const std::filesystem::path &output_path) {
  GlTextureDumpResult result;
  result.raw_path = output_path;
  result.flip_y_path = pathWithStemSuffix(output_path, "_flip_y");

  if (texture_id == 0) {
    result.error = "texture id is 0";
    return result;
  }

  std::error_code ec;
  if (result.raw_path.has_parent_path()) {
    std::filesystem::create_directories(result.raw_path.parent_path(), ec);
    if (ec) {
      result.error = "failed to create output directory: " + ec.message();
      return result;
    }
  }

  GLint previous_active_texture = 0;
  GLint previous_texture0 = 0;
  GLint previous_pack_alignment = 0;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active_texture);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture0);
  glGetIntegerv(GL_PACK_ALIGNMENT, &previous_pack_alignment);

  glBindTexture(GL_TEXTURE_2D, texture_id);
  GLint texture_width = 0;
  GLint texture_height = 0;
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &texture_width);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,
                           &texture_height);
  if (texture_width <= 0 || texture_height <= 0) {
    result.error = "texture has invalid dimensions";
    glBindTexture(GL_TEXTURE_2D, previous_texture0);
    glActiveTexture(previous_active_texture);
    return result;
  }

  result.width = texture_width;
  result.height = texture_height;
  std::vector<unsigned char> rgba(static_cast<size_t>(texture_width) *
                                  static_cast<size_t>(texture_height) * 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  const GLenum gl_error = glGetError();
  glPixelStorei(GL_PACK_ALIGNMENT, previous_pack_alignment);
  glBindTexture(GL_TEXTURE_2D, previous_texture0);
  glActiveTexture(previous_active_texture);

  if (gl_error != GL_NO_ERROR) {
    std::ostringstream error;
    error << "glGetTexImage failed with GL error 0x" << std::hex << gl_error;
    result.error = error.str();
    return result;
  }

  cv::Mat rgba_image(texture_height, texture_width, CV_8UC4, rgba.data());
  cv::Mat bgra_image;
  cv::cvtColor(rgba_image, bgra_image, cv::COLOR_RGBA2BGRA);
  if (!cv::imwrite(result.raw_path.string(), bgra_image)) {
    result.error = "failed to write " + result.raw_path.string();
    return result;
  }

  cv::Mat flip_y_image;
  cv::flip(bgra_image, flip_y_image, 0);
  if (!cv::imwrite(result.flip_y_path.string(), flip_y_image)) {
    result.error = "failed to write " + result.flip_y_path.string();
    return result;
  }

  result.ok = true;
  return result;
}

GlFramebufferDumpResult
dumpGlBufferToPng(const std::filesystem::path &output_path, int width,
                  int height, GLenum read_buffer) {
  GlFramebufferDumpResult result;
  result.path = output_path;
  result.width = width;
  result.height = height;
  if (width <= 0 || height <= 0) {
    result.error = "framebuffer has invalid dimensions";
    return result;
  }

  std::error_code ec;
  if (output_path.has_parent_path()) {
    std::filesystem::create_directories(output_path.parent_path(), ec);
    if (ec) {
      result.error = "failed to create output directory: " + ec.message();
      return result;
    }
  }

  GLint previous_read_buffer = GL_BACK;
  GLint previous_pack_alignment = 4;
  GLint previous_pack_row_length = 0;
  GLint previous_pack_buffer = 0;
  glGetIntegerv(GL_READ_BUFFER, &previous_read_buffer);
  glGetIntegerv(GL_PACK_ALIGNMENT, &previous_pack_alignment);
  glGetIntegerv(GL_PACK_ROW_LENGTH, &previous_pack_row_length);
  glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previous_pack_buffer);

  std::vector<unsigned char> rgba(static_cast<size_t>(width) *
                                  static_cast<size_t>(height) * 4);
  while (glGetError() != GL_NO_ERROR) {
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glPixelStorei(GL_PACK_ROW_LENGTH, 0);
  glReadBuffer(read_buffer);
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  const GLenum gl_error = glGetError();

  glReadBuffer(previous_read_buffer);
  glPixelStorei(GL_PACK_ROW_LENGTH, previous_pack_row_length);
  glPixelStorei(GL_PACK_ALIGNMENT, previous_pack_alignment);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previous_pack_buffer));
  if (gl_error != GL_NO_ERROR) {
    std::ostringstream error;
    error << "glReadPixels failed with GL error 0x" << std::hex << gl_error;
    result.error = error.str();
    return result;
  }

  cv::Mat rgba_image(height, width, CV_8UC4, rgba.data());
  cv::Mat bgra_image;
  cv::cvtColor(rgba_image, bgra_image, cv::COLOR_RGBA2BGRA);
  cv::Mat top_down_image;
  cv::flip(bgra_image, top_down_image, 0);
  if (!cv::imwrite(output_path.string(), top_down_image)) {
    result.error = "failed to write " + output_path.string();
    return result;
  }
  result.ok = true;
  return result;
}

} // namespace crimson::platform::nvidia
