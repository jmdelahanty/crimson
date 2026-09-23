#include "ui_reference_contract.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>

#include <nlohmann/json.hpp>

namespace crimson::ui_reference {
namespace {

constexpr std::array<State, static_cast<size_t>(State::Count)> kStates = {
    State::Empty,         State::Workspace,
    State::Keypoints,     State::Overlays,
    State::Polar,         State::StimulusOverlay,
    State::StimulusDebug, State::CropPreview,
    State::AnalysisEye,   State::AnalysisTailStimulus,
};

bool parseNonNegativeInteger(const char *text, int &value) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char *end = nullptr;
  const long parsed = std::strtol(text, &end, 10);
  if (end == text || end == nullptr || *end != '\0' || parsed < 0 ||
      parsed > std::numeric_limits<int>::max()) {
    return false;
  }
  value = static_cast<int>(parsed);
  return true;
}

bool parsePositiveDouble(const char *text, double &value) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char *end = nullptr;
  const double parsed = std::strtod(text, &end);
  if (end == text || end == nullptr || *end != '\0' || !std::isfinite(parsed) ||
      parsed <= 0.0) {
    return false;
  }
  value = parsed;
  return true;
}

bool parsePositiveSize(const char *text, int maximum_dimension, int &width,
                       int &height) {
  if (text == nullptr || *text == '\0' || maximum_dimension < 1) {
    return false;
  }
  const std::string value(text);
  const size_t separator = value.find_first_of("xX");
  if (separator == std::string::npos || separator == 0 ||
      separator + 1 >= value.size()) {
    return false;
  }
  int parsed_width = 0;
  int parsed_height = 0;
  if (!parseNonNegativeInteger(value.substr(0, separator).c_str(),
                               parsed_width) ||
      !parseNonNegativeInteger(value.substr(separator + 1).c_str(),
                               parsed_height) ||
      parsed_width < 1 || parsed_height < 1 ||
      parsed_width > maximum_dimension || parsed_height > maximum_dimension) {
    return false;
  }
  width = parsed_width;
  height = parsed_height;
  return true;
}

} // namespace

const char *stateName(State state) {
  switch (state) {
  case State::Empty:
    return "empty";
  case State::Workspace:
    return "workspace";
  case State::Keypoints:
    return "keypoints";
  case State::Overlays:
    return "overlays";
  case State::Polar:
    return "polar";
  case State::StimulusOverlay:
    return "stimulus-overlay";
  case State::StimulusDebug:
    return "stimulus-debug";
  case State::CropPreview:
    return "crop-preview";
  case State::AnalysisEye:
    return "analysis-eye";
  case State::AnalysisTailStimulus:
    return "analysis-tail-stimulus";
  case State::Count:
    break;
  }
  return "unknown";
}

std::optional<State> parseState(const std::string &value) {
  for (const State state : kStates) {
    if (value == stateName(state)) {
      return state;
    }
  }
  return std::nullopt;
}

std::string stateNames(StateMask states) {
  std::ostringstream names;
  bool first = true;
  for (const State state : kStates) {
    if ((states & stateBit(state)) == 0) {
      continue;
    }
    if (!first) {
      names << ", ";
    }
    names << stateName(state);
    first = false;
  }
  return names.str();
}

ArgumentParseResult consumeLaunchArgument(int argc, const char *const *argv,
                                          int &index, LaunchOptions &options,
                                          const LaunchParsePolicy &policy) {
  if (index < 0 || index >= argc || argv == nullptr || argv[index] == nullptr) {
    return {ArgumentParseStatus::NotHandled, {}};
  }
  const std::string argument(argv[index]);
  const bool recognized =
      argument == "--ui-reference-state" ||
      argument == "--ui-reference-frame" ||
      argument == "--ui-reference-ready-file" ||
      argument == "--ui-reference-timeout" ||
      (policy.allow_size && argument == "--ui-reference-size");
  if (!recognized) {
    return {ArgumentParseStatus::NotHandled, {}};
  }
  if (index + 1 >= argc || argv[index + 1] == nullptr ||
      argv[index + 1][0] == '\0') {
    return {ArgumentParseStatus::Error, "Missing value for " + argument};
  }

  const char *value = argv[++index];
  if (argument == "--ui-reference-state") {
    const auto parsed = parseState(value);
    if (!parsed || (policy.allowed_states & stateBit(*parsed)) == 0) {
      return {ArgumentParseStatus::Error,
              "Invalid --ui-reference-state value '" + std::string(value) +
                  "'; expected " + stateNames(policy.allowed_states)};
    }
    options.state = *parsed;
    options.state_set = true;
  } else if (argument == "--ui-reference-frame") {
    int frame = -1;
    if (!parseNonNegativeInteger(value, frame)) {
      return {ArgumentParseStatus::Error,
              "Invalid --ui-reference-frame value; expected an integer >= 0"};
    }
    options.target_frame = frame;
    options.frame_set = true;
  } else if (argument == "--ui-reference-ready-file") {
    options.ready_file = value;
    options.ready_file_set = true;
  } else if (argument == "--ui-reference-size") {
    if (!parsePositiveSize(value, policy.maximum_dimension,
                           options.logical_width, options.logical_height)) {
      return {ArgumentParseStatus::Error,
              "Invalid --ui-reference-size value; expected WIDTHxHEIGHT up "
              "to " +
                  std::to_string(policy.maximum_dimension) + "x" +
                  std::to_string(policy.maximum_dimension)};
    }
  } else {
    double timeout = 0.0;
    if (!parsePositiveDouble(value, timeout)) {
      return {ArgumentParseStatus::Error,
              "Invalid --ui-reference-timeout value; expected a positive "
              "number of seconds"};
    }
    options.timeout_seconds = timeout;
  }
  options.enabled = true;
  return {ArgumentParseStatus::Consumed, {}};
}

std::optional<std::string>
validateLaunchOptions(const LaunchOptions &options,
                      const LaunchParsePolicy &policy) {
  if (!options.enabled) {
    return std::nullopt;
  }
  if (!options.state_set || !options.frame_set || !options.ready_file_set) {
    return "UI reference capture requires --ui-reference-state, "
           "--ui-reference-frame, and --ui-reference-ready-file";
  }
  if ((policy.allowed_states & stateBit(options.state)) == 0) {
    return "UI reference state is not supported by this backend";
  }
  if (options.target_frame < 0 || options.ready_file.empty() ||
      !std::isfinite(options.timeout_seconds) ||
      options.timeout_seconds <= 0.0) {
    return "UI reference launch options are invalid";
  }
  if (policy.allow_size &&
      (options.logical_width < 1 || options.logical_height < 1 ||
       options.logical_width > policy.maximum_dimension ||
       options.logical_height > policy.maximum_dimension)) {
    return "UI reference logical size is invalid";
  }
  return std::nullopt;
}

nlohmann::json makeMarkerEnvelope(const MarkerEnvelope &envelope) {
  return {{"format", "crimson_ui_reference_v1"},
          {"platform", envelope.platform},
          {"state", stateName(envelope.state)},
          {"archive", envelope.archive},
          {"write_contract", "read-only"},
          {"target_frame", envelope.target_frame},
          {"presented_frame", envelope.presented_frame},
          {"stable_frames", envelope.stable_frames},
          {"client_size",
           {{"width", envelope.client_size.width},
            {"height", envelope.client_size.height}}},
          {"framebuffer_size",
           {{"width", envelope.framebuffer_size.width},
            {"height", envelope.framebuffer_size.height}}},
          {"rendered_image",
           {{"path", envelope.rendered_image.path.string()},
            {"width", envelope.rendered_image.width},
            {"height", envelope.rendered_image.height},
            {"surface", envelope.rendered_image.surface}}}};
}

} // namespace crimson::ui_reference
