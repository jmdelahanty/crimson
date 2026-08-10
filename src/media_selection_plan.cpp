#include "media_selection_plan.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>

namespace crimson::media {
namespace {

std::string lowerExtension(const std::filesystem::path &path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return extension;
}

bool isVideoExtension(const std::string &extension) {
  return extension == ".mp4";
}

bool isImageExtension(const std::string &extension) {
  return extension == ".tiff" || extension == ".jpeg" || extension == ".jpg" ||
         extension == ".png";
}

std::string selectionFileName(const CameraMediaSelection &selection) {
  std::string name = selection.display_name;
  const size_t separator = name.find_last_of("/\\");
  if (separator != std::string::npos) {
    name = name.substr(separator + 1);
  }
  if (name.empty()) {
    name = selection.path.filename().string();
  }
  return name;
}

} // namespace

bool BuildCameraMediaOpenPlan(
    const std::vector<CameraMediaSelection> &selections,
    CameraMediaOpenPlan &plan, std::string &error_message) {
  plan = CameraMediaOpenPlan{};
  error_message.clear();
  if (selections.empty()) {
    error_message = "No camera media was selected";
    return false;
  }

  const std::string first_extension = lowerExtension(selections.front().path);
  if (isVideoExtension(first_extension)) {
    plan.kind = CameraMediaKind::VideoFiles;
  } else if (isImageExtension(first_extension)) {
    plan.kind = CameraMediaKind::ImageSequence;
  } else {
    error_message = "Unsupported camera media extension: " + first_extension;
    return false;
  }

  std::set<std::string> camera_names;
  std::set<std::string> frame_names;
  std::set<std::pair<std::string, std::string>> image_cells;
  for (const auto &selection : selections) {
    if (selection.path.empty()) {
      error_message = "Camera media selection has an empty path";
      return false;
    }
    const std::string extension = lowerExtension(selection.path);
    const bool selection_is_video = isVideoExtension(extension);
    const bool selection_is_image = isImageExtension(extension);
    if ((plan.kind == CameraMediaKind::VideoFiles && !selection_is_video) ||
        (plan.kind == CameraMediaKind::ImageSequence && !selection_is_image)) {
      error_message = "Camera media selections cannot mix videos and images";
      return false;
    }

    const std::string file_name = selectionFileName(selection);
    if (plan.kind == CameraMediaKind::VideoFiles) {
      std::string camera_name =
          std::filesystem::path(file_name).stem().string();
      if (camera_name.empty()) {
        error_message = "Camera video has no usable camera name: " + file_name;
        return false;
      }
      if (!camera_names.insert(camera_name).second) {
        error_message = "Duplicate camera video name: " + camera_name;
        return false;
      }
      plan.camera_names.push_back(std::move(camera_name));
      plan.video_paths.push_back(selection.path);
      continue;
    }

    const size_t separator = file_name.find('_');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 >= file_name.size()) {
      error_message =
          "Image sequence files must use <camera>_<frame> names: " + file_name;
      return false;
    }
    const std::string camera_name = file_name.substr(0, separator);
    const std::string frame_name = file_name.substr(separator + 1);
    if (!image_cells.insert({camera_name, frame_name}).second) {
      error_message = "Duplicate image sequence entry: " + file_name;
      return false;
    }
    if (camera_names.insert(camera_name).second) {
      plan.camera_names.push_back(camera_name);
    }
    if (frame_names.insert(frame_name).second) {
      plan.image_frame_names.push_back(frame_name);
    }
  }

  if (plan.kind == CameraMediaKind::ImageSequence) {
    const size_t expected_cells =
        plan.camera_names.size() * plan.image_frame_names.size();
    if (image_cells.size() != expected_cells) {
      error_message =
          "Image sequence selection must include every camera/frame pair";
      return false;
    }
  }
  return true;
}

} // namespace crimson::media
