#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace crimson::ui_reference {

enum class State : uint8_t {
  Empty,
  Workspace,
  Keypoints,
  Overlays,
  Polar,
  StimulusOverlay,
  StimulusDebug,
  CropPreview,
  AnalysisEye,
  AnalysisTailStimulus,
  Count,
};

using StateMask = uint32_t;

constexpr StateMask stateBit(State state) {
  return StateMask{1} << static_cast<uint8_t>(state);
}

const char *stateName(State state);
std::optional<State> parseState(const std::string &value);
std::string stateNames(StateMask states);

struct LaunchOptions {
  bool enabled = false;
  bool state_set = false;
  bool frame_set = false;
  bool ready_file_set = false;
  State state = State::Workspace;
  int target_frame = -1;
  std::filesystem::path ready_file;
  int logical_width = 1920;
  int logical_height = 1080;
  double timeout_seconds = 60.0;
};

struct LaunchParsePolicy {
  StateMask allowed_states = 0;
  bool allow_size = false;
  int maximum_dimension = 4096;
};

enum class ArgumentParseStatus : uint8_t { NotHandled, Consumed, Error };

struct ArgumentParseResult {
  ArgumentParseStatus status = ArgumentParseStatus::NotHandled;
  std::string error;
};

ArgumentParseResult consumeLaunchArgument(int argc, const char *const *argv,
                                          int &index, LaunchOptions &options,
                                          const LaunchParsePolicy &policy);

std::optional<std::string>
validateLaunchOptions(const LaunchOptions &options,
                      const LaunchParsePolicy &policy);

struct PixelSize {
  int width = 0;
  int height = 0;
};

struct RenderedImage {
  std::filesystem::path path;
  int width = 0;
  int height = 0;
  std::string surface;
};

struct MarkerEnvelope {
  std::string platform;
  State state = State::Workspace;
  std::string archive;
  int target_frame = -1;
  int64_t presented_frame = -1;
  int stable_frames = 0;
  PixelSize client_size;
  PixelSize framebuffer_size;
  RenderedImage rendered_image;
};

nlohmann::json makeMarkerEnvelope(const MarkerEnvelope &envelope);

} // namespace crimson::ui_reference
