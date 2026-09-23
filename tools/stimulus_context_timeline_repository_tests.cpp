#include "stimulus_context_timeline.h"
#include "zarr/archive_context.h"
#include "zarr/tensorstore_stimulus_context_timeline_repository.h"

#include <nlohmann/json.hpp>
#include <tensorstore/array.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
namespace ts = tensorstore;
using json = nlohmann::json;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                    \
      return false;                                                            \
    }                                                                          \
  } while (false)

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    const auto seed =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              ("crimson-stimulus-context-" + std::to_string(seed) + "-" +
               std::to_string(attempt));
      std::error_code error;
      if (std::filesystem::create_directory(path_, error)) {
        return;
      }
    }
    path_.clear();
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

bool WriteJson(const std::filesystem::path& path, const json& value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << value.dump(2) << '\n';
  return output.good();
}

template <typename T, size_t Rank>
bool WriteArray(const std::filesystem::path& root, const std::string& path,
                const std::string& data_type,
                const std::array<ts::Index, Rank>& shape,
                const std::vector<T>& values) {
  size_t count = 1;
  json shape_json = json::array();
  json chunk_json = json::array();
  for (const ts::Index extent : shape) {
    count *= static_cast<size_t>(extent);
    shape_json.push_back(extent);
    chunk_json.push_back(std::max<ts::Index>(1, extent));
  }
  if (count != values.size()) {
    return false;
  }
  json bytes = {{"name", "bytes"}};
  if (sizeof(T) > 1) {
    bytes["configuration"] = {{"endian", "little"}};
  }
  const json spec = {
      {"driver", "zarr3"},
      {"kvstore", {{"driver", "file"}, {"path", root.string() + "/"}}},
      {"path", path},
      {"metadata",
       {{"shape", shape_json},
        {"data_type", data_type},
        {"chunk_grid",
         {{"name", "regular"},
          {"configuration", {{"chunk_shape", chunk_json}}}}},
        {"chunk_key_encoding",
         {{"name", "default"}, {"configuration", {{"separator", "/"}}}}},
        {"fill_value", 0},
        {"codecs", json::array({bytes})}}}};
  auto store =
      ts::Open<T, Rank>(spec, ts::OpenMode::open | ts::OpenMode::create,
                        ts::ReadWriteMode::read_write)
          .result();
  if (!store.ok()) {
    std::cerr << "Failed to create " << path << ": " << store.status() << '\n';
    return false;
  }
  auto source = ts::AllocateArray<T>(shape);
  std::copy(values.begin(), values.end(), source.data());
  const auto write = ts::Write(source, *store).commit_future.result();
  if (!write.ok()) {
    std::cerr << "Failed to write " << path << ": " << write.status() << '\n';
    return false;
  }
  return true;
}

std::vector<uint8_t> FixedStrings(const std::vector<std::string>& values,
                                  size_t width) {
  std::vector<uint8_t> result(values.size() * width, 0);
  for (size_t row = 0; row < values.size(); ++row) {
    const size_t count = std::min(width, values[row].size());
    std::copy_n(values[row].begin(), count, result.begin() + row * width);
  }
  return result;
}

bool WriteFixture(const std::filesystem::path& root,
                  const json& run_attributes =
                      {{"latest_complete", "stimulus_context_fixture"}}) {
  CHECK(WriteJson(root / "zarr.json",
                  {{"zarr_format", 3}, {"node_type", "group"}}));
  const std::string group = "analysis/stimulus_runs";
  const std::string run_name = "stimulus_context_fixture";
  const std::string run = group + "/" + run_name;
  CHECK(WriteJson(root / group / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", run_attributes}}));
  CHECK(WriteJson(root / run / "zarr.json",
                  {{"zarr_format", 3}, {"node_type", "group"}}));

  const std::vector<uint64_t> stimulus_frames = {10, 20, 30};
  const std::vector<uint64_t> camera_frames = {
      5, std::numeric_limits<uint64_t>::max(), 15};
  const std::vector<int32_t> event_types = {3, 5, 3};
  const std::vector<int64_t> timestamps = {1000, 2000, 3000};
  const std::vector<std::string> names = {"warmup", "target", "Trial end"};
  const std::vector<std::string> details = {
      R"({"direction":"left"})", "manual", "{}"};
  CHECK((WriteArray<uint64_t, 1>(root, run + "/events/stimulus_frame_num",
                                 "uint64", {3}, stimulus_frames)));
  CHECK((WriteArray<uint64_t, 1>(root, run + "/events/camera_frame_id",
                                 "uint64", {3}, camera_frames)));
  CHECK((WriteArray<int32_t, 1>(root, run + "/events/event_type_id", "int32",
                                {3}, event_types)));
  CHECK((WriteArray<int64_t, 1>(root, run + "/events/timestamp_ns_session",
                                "int64", {3}, timestamps)));
  const auto names_bytes = FixedStrings(names, 24);
  const auto details_bytes = FixedStrings(details, 64);
  CHECK((WriteArray<uint8_t, 2>(root, run + "/events/name_or_context",
                                "uint8", {3, 24}, names_bytes)));
  CHECK((WriteArray<uint8_t, 2>(root, run + "/events/details_json", "uint8",
                                {3, 64}, details_bytes)));

  const std::vector<int32_t> enum_ids = {3, 5};
  const auto enum_names = FixedStrings({"Trial start", "Pulse"}, 32);
  CHECK((WriteArray<int32_t, 1>(root, "analysis/enums/events/id", "int32",
                                {2}, enum_ids)));
  CHECK((WriteArray<uint8_t, 2>(root, "analysis/enums/events/name", "uint8",
                                {2, 32}, enum_names)));

  std::vector<int32_t> direct_mapping(40, -1);
  direct_mapping[8] = 20;
  CHECK((WriteArray<int32_t, 1>(
      root, run + "/frame_alignment/camera_to_stimulus_frame_corrected",
      "int32", {static_cast<ts::Index>(direct_mapping.size())},
      direct_mapping)));

  const std::string step0 = run + "/steps/step_0";
  CHECK(WriteJson(
      root / step0 / "zarr.json",
      {{"zarr_format", 3},
       {"node_type", "group"},
       {"attributes",
        {{"step_index", 0},
         {"step_name", "Approach"},
         {"stimulus_mode_id", 7},
         {"stimulus_mode", "MOVING_GRATING"},
         {"start_camera_frame", 0},
         {"end_camera_frame", 10},
         {"duration_s", 1.0},
         {"raw_protocol_params_json", R"({"direction":90})"}}}}));
  CHECK(WriteJson(
      root / step0 / "moving_grating/zarr.json",
      {{"zarr_format", 3},
       {"node_type", "group"},
       {"attributes",
        {{"grating_direction_camera_deg", 90.0},
         {"orientation_degrees_authored", 45.0},
         {"camera_to_projector_offset_deg", 45.0},
         {"direction_mapping_status", "validated"},
         {"direction_mapping_validated", true},
         {"speed_mm_s", 12.5},
         {"temporal_frequency_hz", 2.0}}}}));

  const std::string step1 = run + "/steps/step_1";
  CHECK(WriteJson(
      root / step1 / "zarr.json",
      {{"zarr_format", 3},
       {"node_type", "group"},
       {"attributes",
        {{"step_index", 1},
         {"step_name", "Retreat"},
         {"stimulus_mode_id", 8},
         {"stimulus_mode", "CONCENTRIC_GRATING"},
         {"start_camera_frame", 10},
         {"end_camera_frame", 30},
         {"duration_s", 2.0},
         {"raw_protocol_params_json", R"({"polarity":"expand"})"}}}}));
  CHECK(WriteJson(
      root / step1 / "concentric_grating/zarr.json",
      {{"zarr_format", 3},
       {"node_type", "group"},
       {"attributes",
        {{"stimulus_role", "distractor"},
         {"radial_polarity_authored", "expanding"},
         {"radial_sign_authored", 1.0},
         {"radial_polarity_validated", true},
         {"center_x_px", 320.0},
         {"center_y_px", 240.0},
         {"center_x_mm", 16.0},
         {"center_y_mm", 12.0},
         {"target_radius_min_mm", 1.5},
         {"target_radius_max_mm", 9.0},
         {"speed_mm_s", 4.0},
         {"temporal_frequency_hz", 1.25}}}}));
  return true;
}

bool RunTest() {
  using namespace crimson::timeline;

  TemporaryDirectory temporary;
  CHECK(!temporary.path().empty());
  CHECK(WriteFixture(temporary.path()));

  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(temporary.path(), &error);
  CHECK(archive != nullptr);
  auto repository = crimson::zarr::OpenStimulusContextTimelineRepository(
      archive, 40, {}, &error);
  if (!repository) {
    std::cerr << error << '\n';
  }
  CHECK(repository != nullptr);
  const auto snapshot = repository->snapshot();
  CHECK(snapshot != nullptr);
  CHECK(repository->descriptor().run_name == "stimulus_context_fixture");
  CHECK(repository->descriptor().frame_count == 40);
  CHECK(repository->descriptor().event_count == 3);
  CHECK(repository->descriptor().step_count == 2);
  CHECK(repository->descriptor().event_types.size() == 2);
  CHECK(repository->descriptor().event_types[0].id == 3);
  CHECK(repository->descriptor().event_types[0].display_name == "Trial start");
  CHECK(repository->descriptor().event_types[0].event_count == 2);
  CHECK(repository->descriptor().event_types[1].id == 5);
  CHECK(repository->descriptor().event_types[1].event_count == 1);

  CHECK(snapshot->events.size() == 3);
  CHECK(snapshot->events[0].stimulus_frame == 10);
  CHECK(snapshot->events[0].camera_frame == 5);
  CHECK(snapshot->events[0].label == "Trial start - warmup");
  CHECK(snapshot->events[1].stimulus_frame == 20);
  CHECK(snapshot->events[1].camera_frame == 8);
  CHECK(snapshot->events[1].label == "Pulse - target [manual]");
  CHECK(snapshot->events[2].camera_frame == 15);
  CHECK(snapshot->events[2].label == "Trial start - Trial end");

  CHECK(snapshot->steps.size() == 2);
  CHECK(snapshot->steps[0].kind == StimulusStepKind::MovingGrating);
  CHECK(snapshot->steps[0].moving_grating.present);
  CHECK(snapshot->steps[0].moving_grating.direction_mapping_validated);
  CHECK(std::fabs(snapshot->steps[0].moving_grating.speed_mm_s - 12.5) <
        1e-12);
  CHECK(snapshot->steps[1].kind == StimulusStepKind::ConcentricGrating);
  CHECK(snapshot->steps[1].concentric_grating.present);
  CHECK(snapshot->steps[1].concentric_grating.stimulus_role == "distractor");
  CHECK(std::fabs(snapshot->steps[1]
                      .concentric_grating.target_radius_max_mm -
                  9.0) < 1e-12);

  const auto* handoff = findStimulusContextStepForFrame(*snapshot, 10);
  CHECK(handoff != nullptr);
  CHECK(handoff->step_index == 1);
  const auto frame_events = stimulusContextEventsForFrame(*snapshot, 8);
  CHECK(frame_events.size() == 1);
  CHECK(frame_events.front()->event_type_id == 5);
  const auto window = stimulusContextTimelineWindow(*snapshot, 7, 12);
  CHECK(window.valid());
  CHECK(window.event_indices.size() == 1);
  CHECK(window.step_indices.size() == 2);
  return true;
}

bool RunSelectionTests() {
  constexpr const char* run_name = "stimulus_context_fixture";
  {
    TemporaryDirectory temporary;
    CHECK(!temporary.path().empty());
    CHECK(WriteFixture(temporary.path(),
                       {{"latest", "incomplete_run"},
                        {"latest_success", run_name}}));
    std::string error;
    auto archive = crimson::zarr::ArchiveContext::Open(temporary.path(),
                                                        &error);
    CHECK(archive != nullptr);
    auto repository =
        crimson::zarr::OpenStimulusContextTimelineRepository(archive, 40, {},
                                                              &error);
    CHECK(repository != nullptr);
    CHECK(repository->descriptor().run_name == run_name);
  }
  {
    TemporaryDirectory temporary;
    CHECK(!temporary.path().empty());
    CHECK(WriteFixture(temporary.path(), json::object()));
    std::string error;
    auto archive = crimson::zarr::ArchiveContext::Open(temporary.path(),
                                                        &error);
    CHECK(archive != nullptr);
    auto repository =
        crimson::zarr::OpenStimulusContextTimelineRepository(archive, 40, {},
                                                              &error);
    CHECK(repository != nullptr);
    CHECK(repository->descriptor().run_name == run_name);
  }
  return true;
}

}  // namespace

int main() {
  if (!RunTest() || !RunSelectionTests()) {
    return 1;
  }
  std::cout << "stimulus_context_timeline_repository_tests: PASS\n";
  return 0;
}
