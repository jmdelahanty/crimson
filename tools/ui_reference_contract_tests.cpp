#include "ui_reference_contract.h"

#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition      \
                << '\n';                                                       \
      return false;                                                            \
    }                                                                          \
  } while (false)

using crimson::ui_reference::ArgumentParseStatus;
using crimson::ui_reference::LaunchOptions;
using crimson::ui_reference::LaunchParsePolicy;
using crimson::ui_reference::State;

constexpr auto kTestStates = crimson::ui_reference::stateBit(State::Empty) |
                             crimson::ui_reference::stateBit(State::Workspace) |
                             crimson::ui_reference::stateBit(State::Overlays);

struct ParseResult {
  bool ok = false;
  LaunchOptions options;
  std::string error;
};

ParseResult parse(const std::vector<std::string> &arguments,
                  const LaunchParsePolicy &policy) {
  ParseResult result;
  std::vector<const char *> argv;
  for (const auto &argument : arguments) {
    argv.push_back(argument.c_str());
  }
  for (int index = 1; index < static_cast<int>(argv.size()); ++index) {
    const auto parsed = crimson::ui_reference::consumeLaunchArgument(
        static_cast<int>(argv.size()), argv.data(), index, result.options,
        policy);
    if (parsed.status == ArgumentParseStatus::Error) {
      result.error = parsed.error;
      return result;
    }
    if (parsed.status == ArgumentParseStatus::NotHandled) {
      result.error = "unexpected argument";
      return result;
    }
  }
  if (const auto error = crimson::ui_reference::validateLaunchOptions(
          result.options, policy)) {
    result.error = *error;
    return result;
  }
  result.ok = true;
  return result;
}

bool testStateVocabularyAndCapabilities() {
  CHECK(std::string(crimson::ui_reference::stateName(State::StimulusOverlay)) ==
        "stimulus-overlay");
  CHECK(crimson::ui_reference::parseState("analysis-tail-stimulus") ==
        State::AnalysisTailStimulus);
  CHECK(!crimson::ui_reference::parseState("future-state"));
  CHECK(crimson::ui_reference::stateNames(kTestStates) ==
        "empty, workspace, overlays");

  const LaunchParsePolicy policy{kTestStates, true, 4096};
  const auto unsupported = parse({"Crimson", "--ui-reference-state",
                                  "keypoints", "--ui-reference-frame", "0",
                                  "--ui-reference-ready-file", "ready.json"},
                                 policy);
  CHECK(!unsupported.ok);
  CHECK(unsupported.error.find("expected empty, workspace, overlays") !=
        std::string::npos);
  return true;
}

bool testCompleteLaunchSurface() {
  const LaunchParsePolicy policy{kTestStates, true, 4096};
  const auto parsed = parse(
      {"Crimson", "--ui-reference-state", "overlays", "--ui-reference-frame",
       "52", "--ui-reference-ready-file", "ready.json", "--ui-reference-size",
       "1280X720", "--ui-reference-timeout", "15.5"},
      policy);
  CHECK(parsed.ok);
  CHECK(parsed.options.enabled);
  CHECK(parsed.options.state == State::Overlays);
  CHECK(parsed.options.target_frame == 52);
  CHECK(parsed.options.ready_file == "ready.json");
  CHECK(parsed.options.logical_width == 1280);
  CHECK(parsed.options.logical_height == 720);
  CHECK(parsed.options.timeout_seconds == 15.5);
  return true;
}

bool testFailClosedValidation() {
  const LaunchParsePolicy policy{kTestStates, true, 4096};
  const std::vector<std::vector<std::string>> invalid = {
      {"Crimson", "--ui-reference-frame", "1"},
      {"Crimson", "--ui-reference-state", "workspace"},
      {"Crimson", "--ui-reference-state", "workspace", "--ui-reference-frame",
       "-1", "--ui-reference-ready-file", "ready"},
      {"Crimson", "--ui-reference-state", "workspace", "--ui-reference-frame",
       "1", "--ui-reference-ready-file", "ready", "--ui-reference-timeout",
       "nan"},
      {"Crimson", "--ui-reference-state", "workspace", "--ui-reference-frame",
       "1", "--ui-reference-ready-file", "ready", "--ui-reference-size",
       "4097x1"},
  };
  for (const auto &arguments : invalid) {
    CHECK(!parse(arguments, policy).ok);
  }
  return true;
}

bool testMarkerEnvelope() {
  const crimson::ui_reference::MarkerEnvelope envelope{
      "macos-metal",
      State::Keypoints,
      "/archive.zarr",
      123,
      123,
      60,
      {1280, 720},
      {2560, 1440},
      {"/tmp/reference.json.png", 2560, 1440, "metal_drawable_pre_present"},
  };
  const nlohmann::json marker =
      crimson::ui_reference::makeMarkerEnvelope(envelope);
  CHECK(marker.at("format") == "crimson_ui_reference_v1");
  CHECK(marker.at("platform") == "macos-metal");
  CHECK(marker.at("state") == "keypoints");
  CHECK(marker.at("write_contract") == "read-only");
  CHECK(marker.at("target_frame") == 123);
  CHECK(marker.at("presented_frame") == 123);
  CHECK(marker.at("stable_frames") == 60);
  CHECK(marker.at("client_size").at("width") == 1280);
  CHECK(marker.at("framebuffer_size").at("height") == 1440);
  CHECK(marker.at("rendered_image").at("surface") ==
        "metal_drawable_pre_present");
  return true;
}

} // namespace

int main() {
  if (!testStateVocabularyAndCapabilities() || !testCompleteLaunchSurface() ||
      !testFailClosedValidation() || !testMarkerEnvelope()) {
    return 1;
  }
  std::cout << "ui_reference_contract_tests: PASS\n";
  return 0;
}
