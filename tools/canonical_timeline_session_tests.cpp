#include "gui/canonical_timeline_session.h"

#include <nlohmann/json.hpp>
#include <tensorstore/array.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace ts = tensorstore;
using json = nlohmann::json;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__          \
                << ": " #condition << '\n';                                   \
      return false;                                                            \
    }                                                                          \
  } while (false)

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    const auto seed =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("crimson-canonical-timeline-session-" + std::to_string(seed));
    std::error_code error;
    std::filesystem::create_directory(path_, error);
    if (error) path_.clear();
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
  for (const auto extent : shape) {
    count *= static_cast<size_t>(extent);
    shape_json.push_back(extent);
    chunk_json.push_back(std::max<ts::Index>(1, extent));
  }
  if (count != values.size()) return false;
  json bytes = {{"name", "bytes"}};
  if (sizeof(T) > 1) bytes["configuration"] = {{"endian", "little"}};
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
  auto store = ts::Open<T, Rank>(
                   spec, ts::OpenMode::open | ts::OpenMode::create,
                   ts::ReadWriteMode::read_write)
                   .result();
  if (!store.ok()) return false;
  auto source = ts::AllocateArray<T>(shape);
  std::copy(values.begin(), values.end(), source.data());
  return ts::Write(source, *store).commit_future.result().ok();
}

std::vector<uint8_t> FixedName(const std::string& value, size_t width) {
  std::vector<uint8_t> result(width, 0);
  std::copy_n(value.begin(), std::min(width, value.size()), result.begin());
  return result;
}

bool WriteFixture(const std::filesystem::path& root,
                  bool incomplete_motion = false) {
  CHECK(WriteJson(root / "zarr.json",
                  {{"zarr_format", 3}, {"node_type", "group"}}));
  CHECK(WriteJson(
      root / "analysis/eye_angle_runs/zarr.json",
      {{"zarr_format", 3},
       {"node_type", "group"},
       {"attributes",
        {{"latest", "eye_fixture"}, {"latest_complete", "eye_fixture"}}}}));
  CHECK(WriteJson(
      root / "analysis/eye_angle_runs/eye_fixture/zarr.json",
      {{"zarr_format", 3},
       {"node_type", "group"},
       {"attributes",
        {{"schema_id", "analysis.eye_angle_runs"},
         {"schema_version", 5},
         {"layout", "compact_dense_v2"},
         {"palette_run_completion_status", "complete"},
         {"stage_selector_eligible", true},
         {"eye_angle_variant_schema",
          {{"default_representation", "eye"},
           {"representation_order", {"eye"}},
           {"representations",
            {{"eye",
              {{"display_name", "Eye"},
               {"units", "deg"},
               {"default_plot_fields", {"left_eye_angle_deg"}}}}}},
           {"fields", json::object()}}}}}}));
  CHECK((WriteArray<uint8_t, 2>(
      root, "analysis/eye_angle_runs/eye_fixture/angle_channel_index/name",
      "uint8", {1, 32}, FixedName("left_eye_angle_deg", 32))));
  CHECK((WriteArray<uint8_t, 1>(
      root,
      "analysis/eye_angle_runs/eye_fixture/angle_channel_index/"
      "frame_available",
      "uint8", {1}, {1})));
  std::vector<float> eye_values(16);
  std::vector<float> times(16);
  std::vector<int64_t> frames(16);
  std::vector<int64_t> sample_keys(32);
  for (size_t index = 0; index < 16; ++index) {
    eye_values[index] = static_cast<float>(index);
    times[index] = static_cast<float>(index) / 30.0f;
    frames[index] = static_cast<int64_t>(index);
    sample_keys[index * 2] = 0;
    sample_keys[index * 2 + 1] = static_cast<int64_t>(index);
  }
  CHECK((WriteArray<float, 2>(
      root, "analysis/eye_angle_runs/eye_fixture/frame_angles", "float32",
      {16, 1}, eye_values)));
  CHECK((WriteArray<float, 2>(
      root, "analysis/eye_angle_runs/eye_fixture/roi_angles", "float32",
      {1, 1}, {0.0f})));
  CHECK((WriteArray<float, 1>(
      root,
      "analysis/eye_angle_runs/eye_fixture/support/frame_time_seconds",
      "float32", {16}, times)));

  CHECK(WriteJson(
      root / "analysis/track_kinematics_runs/zarr.json",
      {{"zarr_format", 3},
       {"node_type", "group"},
       {"attributes",
        {{"latest", "offline/motion_fixture"},
         {"latest_complete", "offline/motion_fixture"}}}}));
  const std::string motion =
      "analysis/track_kinematics_runs/offline/motion_fixture";
  CHECK(WriteJson(root / motion / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes",
                    {{"schema_id", "analysis.track_kinematics_runs"},
                     {"schema_version", 1},
                     {"palette_run_completion_status",
                      incomplete_motion ? "running" : "complete"},
                     {"stage_selector_eligible", true}}}}));
  CHECK((WriteArray<int64_t, 1>(root, motion + "/track_ids", "int64", {1},
                                {0})));
  const std::string track = motion + "/tracks/id_0";
  CHECK((WriteArray<int64_t, 1>(
      root, track + "/source_acquisition_frame_index", "int64", {16},
      frames)));
  CHECK((WriteArray<int64_t, 2>(root, track + "/track_sample_key", "int64",
                                {16, 2}, sample_keys)));
  CHECK((WriteArray<int64_t, 1>(root, track + "/source_instance_key",
                                "int64", {16}, frames)));
  CHECK((WriteArray<float, 1>(root, track + "/time_seconds", "float32", {16},
                              times)));
  CHECK((WriteArray<float, 1>(root, track + "/speed_filtered_mm", "float32",
                              {16}, eye_values)));

  CHECK(WriteJson(
      root / "analysis/swim_bout_runs/zarr.json",
      {{"zarr_format", 3},
       {"node_type", "group"},
       {"attributes",
        {{"latest", "bad_bout"}, {"latest_complete", "bad_bout"}}}}));
  CHECK(WriteJson(
      root / "analysis/swim_bout_runs/bad_bout/zarr.json",
      {{"zarr_format", 3},
       {"node_type", "group"},
       {"attributes",
        {{"schema_id", "palette.swim_bout_runs"},
         {"schema_version", 7},
         {"layout", "compact_tabular_v2"},
         {"source_track_kinematics_scope", "offline"},
         {"source_track_kinematics_run", "motion_fixture"},
         {"track_id", 0}}}}));
  return true;
}

crimson::gui::CanonicalTimelineOpenRequest Request(
    const std::filesystem::path& root) {
  crimson::gui::CanonicalTimelineOpenRequest request;
  request.archive_path = root.string();
  request.expected_frame_count = 16;
  request.frames_per_second = 30.0;
  request.page_span_frames = 16;
  request.page_step_frames = 8;
  request.max_points_per_trace = 16;
  request.cache_pages = 2;
  return request;
}

bool WaitForProducts(crimson::gui::CanonicalTimelineSession* session,
                     int64_t frame) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    const auto snapshot = session->snapshot(frame);
    if (snapshot.eye_angles.state ==
            crimson::gui::CanonicalTimelineProductState::Ready &&
        snapshot.motion.state ==
            crimson::gui::CanonicalTimelineProductState::Ready &&
        snapshot.swim_bouts.state ==
            crimson::gui::CanonicalTimelineProductState::Error) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

bool TestInvalidAndSeparatedErrors(const std::filesystem::path& root) {
  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(32, 2, 1, 1);
  crimson::gui::CanonicalTimelineSession session(scheduler);
  std::string error;
  CHECK(!session.beginOpen({}, &error));
  CHECK(!error.empty());

  auto request = Request(root);
  CHECK(session.beginOpen(request, &error));
  CHECK(session.waitUntilOpen(std::chrono::seconds(2)));
  CHECK(session.state() == crimson::gui::CanonicalTimelineSessionState::Ready);
  CHECK(session.requestFrame(8, false, &error));
  CHECK(WaitForProducts(&session, 8));
  auto snapshot = session.snapshot(8);
  CHECK(snapshot.eye_angles.error.empty());
  CHECK(snapshot.motion.error.empty());
  CHECK(snapshot.swim_bouts.error.find("schema 8") != std::string::npos);

  const uint64_t generation = snapshot.generation;
  session.close();
  CHECK(session.state() == crimson::gui::CanonicalTimelineSessionState::Closed);
  request.eye_angle_run = "wrong_eye";
  CHECK(session.beginOpen(request, &error));
  CHECK(session.waitUntilOpen(std::chrono::seconds(2)));
  CHECK(session.state() == crimson::gui::CanonicalTimelineSessionState::Ready);
  snapshot = session.snapshot(8);
  CHECK(snapshot.generation > generation);
  CHECK(snapshot.eye_angles.state ==
        crimson::gui::CanonicalTimelineProductState::Error);
  CHECK(snapshot.eye_angles.error.find("disagrees") != std::string::npos);
  CHECK(snapshot.motion.error.empty());
  session.close();
  scheduler->shutdown();
  return true;
}

bool TestIncompleteMotionIsolated(const std::filesystem::path& root) {
  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(32, 2, 1, 1);
  crimson::gui::CanonicalTimelineSession session(scheduler);
  std::string error;
  CHECK(session.beginOpen(Request(root), &error));
  CHECK(session.waitUntilOpen(std::chrono::seconds(2)));
  CHECK(session.state() == crimson::gui::CanonicalTimelineSessionState::Ready);
  CHECK(session.requestFrame(8, false, &error));
  for (int attempt = 0; attempt < 200; ++attempt) {
    const auto snapshot = session.snapshot(8);
    if (snapshot.eye_angles.state ==
            crimson::gui::CanonicalTimelineProductState::Ready &&
        snapshot.motion.state ==
            crimson::gui::CanonicalTimelineProductState::Error) {
      CHECK(snapshot.eye_angles.error.empty());
      CHECK(!snapshot.motion.error.empty());
      session.close();
      scheduler->shutdown();
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

}  // namespace

int main() {
  TemporaryDirectory fixture;
  TemporaryDirectory incomplete;
  if (fixture.path().empty() || incomplete.path().empty() ||
      !WriteFixture(fixture.path()) ||
      !WriteFixture(incomplete.path(), true) ||
      !TestInvalidAndSeparatedErrors(fixture.path()) ||
      !TestIncompleteMotionIsolated(incomplete.path())) {
    return 1;
  }
  std::cout << "canonical_timeline_session_tests: PASS\n";
  return 0;
}
