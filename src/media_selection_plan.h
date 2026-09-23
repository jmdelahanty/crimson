#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace crimson::media {

enum class CameraMediaKind {
  VideoFiles,
  ImageSequence,
};

struct CameraMediaSelection {
  std::string display_name;
  std::filesystem::path path;
};

struct CameraMediaOpenPlan {
  CameraMediaKind kind = CameraMediaKind::VideoFiles;
  std::vector<std::string> camera_names;
  std::vector<std::filesystem::path> video_paths;
  std::vector<std::string> image_frame_names;
};

bool BuildCameraMediaOpenPlan(
    const std::vector<CameraMediaSelection> &selections,
    CameraMediaOpenPlan &plan, std::string &error_message);

} // namespace crimson::media
