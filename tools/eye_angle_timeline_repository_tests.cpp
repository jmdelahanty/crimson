#include "eye_angle_timeline.h"
#include "zarr/archive_context.h"
#include "zarr/tensorstore_eye_angle_timeline_repository.h"

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
#include <string>
#include <vector>

namespace {
namespace ts = tensorstore;
using json = nlohmann::json;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__         \
                << ": " #condition << '\n';                                  \
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
              ("crimson-eye-angle-timeline-" + std::to_string(seed) + "-" +
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
  for (const auto extent : shape) {
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
    return false;
  }
  auto source = ts::AllocateArray<T>(shape);
  std::copy(values.begin(), values.end(), source.data());
  return ts::Write(source, *store).commit_future.result().ok();
}

std::vector<uint8_t> Names(const std::vector<std::string>& names) {
  std::vector<uint8_t> bytes(names.size() * 64, 0);
  for (size_t row = 0; row < names.size(); ++row) {
    const size_t count = std::min<size_t>(63, names[row].size());
    std::copy_n(names[row].begin(), count, bytes.begin() + row * 64);
  }
  return bytes;
}

json VariantSchema() {
  return {
      {"schema_id", "analysis.eye_angle_variant_schema"},
      {"schema_version", 1},
      {"default_representation", "eye_frame"},
      {"representation_order", {"eye_frame", "gaze"}},
      {"representations",
       {{"eye_frame",
         {{"display_name", "Bianco/Engert eye-frame angles"},
          {"role", "biological_presentation"},
          {"coordinate_frame", "per_eye_nasal_positive"},
          {"units", "deg"},
          {"default_plot_fields",
           {"left_eye_angle_deg_smoothed",
            "right_eye_angle_deg_smoothed",
            "vergence_eye_angle_deg_smoothed"}}}},
        {"gaze",
         {{"display_name", "Gaze direction"},
          {"role", "gaze_direction"},
          {"coordinate_frame", "fish_body_frame"},
          {"units", "deg"},
          {"default_plot_fields",
           {"left_gaze_signed_deg_smoothed",
            "right_gaze_signed_deg_smoothed"}}}}}},
      {"fields", json::object()},
  };
}

bool WriteFixture(const std::filesystem::path& root,
                  bool channel_shape_mismatch = false) {
  CHECK(WriteJson(root / "zarr.json",
                  {{"zarr_format", 3}, {"node_type", "group"}}));
  const std::string group = "analysis/eye_angle_runs";
  const std::string run = group + "/timeline_fixture";
  CHECK(WriteJson(root / group / "zarr.json",
                  {{"zarr_format", 3},
                   {"node_type", "group"},
                   {"attributes", {{"latest_complete", "timeline_fixture"}}}}));
  CHECK(WriteJson(
      root / run / "zarr.json",
      {{"zarr_format", 3},
       {"node_type", "group"},
       {"attributes",
        {{"schema_id", "analysis.eye_angle_runs"},
         {"schema_version", 5},
         {"method", "ellipse_and_centroid_eye_angles"},
         {"method_version", "eye_angle_analysis.v5"},
         {"layout", "compact_dense_v2"},
         {"eye_angle_variant_schema", VariantSchema()}}}}));

  std::vector<std::string> names = {
      "left_eye_angle_deg", "right_eye_angle_deg_smoothed",
      "vergence_eye_angle_deg", "left_gaze_signed_deg",
      "right_gaze_signed_deg"};
  if (channel_shape_mismatch) {
    names.push_back("invalid_channel");
  }
  CHECK((WriteArray<uint8_t, 2>(root, run + "/angle_channel_index/name",
                                "uint8", {static_cast<ts::Index>(names.size()),
                                          64},
                                Names(names))));
  CHECK((WriteArray<uint8_t, 1>(
      root, run + "/angle_channel_index/frame_available", "uint8",
      {static_cast<ts::Index>(names.size())},
      std::vector<uint8_t>(names.size(), 1))));

  constexpr size_t frames = 12;
  constexpr size_t columns = 5;
  std::vector<float> frame_values(frames * columns);
  for (size_t frame = 0; frame < frames; ++frame) {
    frame_values[frame * columns + 0] = 10.0f + frame;
    frame_values[frame * columns + 1] = -10.0f - frame;
    frame_values[frame * columns + 2] = 20.0f + frame * 2.0f;
    frame_values[frame * columns + 3] = 30.0f + frame * 3.0f;
    frame_values[frame * columns + 4] = -30.0f - frame * 4.0f;
  }
  CHECK((WriteArray<float, 2>(root, run + "/frame_angles", "float32",
                              {frames, columns}, frame_values)));
  CHECK((WriteArray<float, 2>(root, run + "/roi_angles", "float32",
                              {6, columns},
                              std::vector<float>(6 * columns, 0.0f))));
  std::vector<float> times(frames);
  for (size_t frame = 0; frame < frames; ++frame) {
    times[frame] = static_cast<float>(frame) * 0.0125f;
  }
  CHECK((WriteArray<float, 1>(root, run + "/support/frame_time_seconds",
                              "float32", {frames}, times)));
  return true;
}

bool TestRepository(const std::filesystem::path& root) {
  using namespace crimson::timeline;
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(root, &error);
  CHECK(archive != nullptr);
  auto repository =
      crimson::zarr::OpenEyeAngleTimelineRepository(archive, {}, &error);
  CHECK(repository != nullptr);
  const auto& descriptor = repository->descriptor();
  CHECK(descriptor.run_name == "timeline_fixture");
  CHECK(descriptor.schema_version == 5);
  CHECK(descriptor.roi_row_count == 6);
  CHECK(descriptor.frame_count == 12);
  CHECK(descriptor.default_representation == "eye_frame");
  CHECK(descriptor.representations.size() == 2);
  CHECK(descriptor.representations[0].fields.size() == 3);
  CHECK(descriptor.representations[0].fields[0].fallback);
  CHECK(descriptor.representations[0].fields[0].source_name ==
        "left_eye_angle_deg");
  CHECK(!descriptor.representations[0].fields[1].fallback);
  CHECK(descriptor.representations[0].fields[2].fallback);

  EyeAngleTimelineRequest request;
  request.representation_key = "eye_frame";
  request.first_frame = 2;
  request.last_frame = 9;
  request.anchor_frame = 5;
  request.max_points_per_trace = 5;
  request.fallback_frames_per_second = 100.0;
  const auto window = repository->resolveWindow(request);
  CHECK(window.ready());
  CHECK(window.source_row_count == 8);
  CHECK(window.traces.size() == 3);
  CHECK(window.traces[0].values.front() == 12.0);
  CHECK(std::fabs(window.traces[0].times_seconds.front() - 0.025) < 1e-6);
  for (const auto& trace : window.traces) {
    CHECK(trace.values.size() <= 5);
  }

  request.representation_key = "gaze";
  const auto gaze = repository->resolveWindow(request);
  CHECK(gaze.ready());
  CHECK(gaze.traces.size() == 2);
  CHECK(gaze.traces[0].field.fallback);
  CHECK(gaze.traces[0].field.role == EyeAngleTraceRole::Left);
  CHECK(gaze.traces[1].field.role == EyeAngleTraceRole::Right);
  return true;
}

bool TestInvalidChannelShape(const std::filesystem::path& root) {
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(root, &error);
  CHECK(archive != nullptr);
  auto repository =
      crimson::zarr::OpenEyeAngleTimelineRepository(archive, {}, &error);
  CHECK(repository == nullptr);
  CHECK(error.find("exceeds") != std::string::npos);
  return true;
}

}  // namespace

int main() {
  TemporaryDirectory valid;
  TemporaryDirectory invalid;
  if (valid.path().empty() || invalid.path().empty() ||
      !WriteFixture(valid.path()) ||
      !WriteFixture(invalid.path(), true) || !TestRepository(valid.path()) ||
      !TestInvalidChannelShape(invalid.path())) {
    return 1;
  }
  std::cout << "eye_angle_timeline_repository_tests: PASS\n";
  return 0;
}
