#include "crop_source_contract.h"
#include "zarr/archive_context.h"
#include "zarr/tensorstore_acquisition_crop_repository.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
using json = nlohmann::json;

#define CHECK(condition)                                                      \
  do {                                                                        \
    if (!(condition)) {                                                       \
      std::cerr << "CHECK failed: " #condition << " at " << __FILE__ << ':' \
                << __LINE__ << '\n';                                         \
      return false;                                                           \
    }                                                                         \
  } while (false)

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              ("crimson-acquisition-crop-" + std::to_string(seed) + "-" +
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

struct FixtureOptions {
  bool zarr_v2 = false;
  bool root_child_mismatch = false;
  bool crop_available = true;
  bool omit_video = false;
  bool traversal_video = false;
  bool imported_count_mismatch = false;
  bool keyframe_count_mismatch = false;
  bool bad_recording_id = false;
  bool bad_blank_geometry = false;
  bool foreign_absolute_paths = false;
  bool omit_status = false;
  bool missing_csv_column = false;
};

struct FixturePaths {
  std::filesystem::path recording_root;
  std::filesystem::path archive_root;
  std::filesystem::path video;
  std::filesystem::path metadata;
};

bool WriteText(const std::filesystem::path& path,
               const std::string& value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << value;
  return output.good();
}

bool WriteJson(const std::filesystem::path& path, const json& value) {
  return WriteText(path, value.dump(2) + "\n");
}

bool WriteGroupAttributes(const std::filesystem::path& group,
                          const json& attributes,
                          bool zarr_v2) {
  std::filesystem::create_directories(group);
  if (zarr_v2) {
    return WriteJson(group / ".zattrs", attributes);
  }
  return WriteJson(group / "zarr.json",
                   {{"zarr_format", 3},
                    {"node_type", "group"},
                    {"attributes", attributes}});
}

json CropContract(const std::string& video_path,
                  const std::string& metadata_path,
                  const std::string& keyframes_path,
                  const std::string& summary_path) {
  return {
      {"role", "runtime_derived_acquisition_input"},
      {"output_kind", "crop"},
      {"source", "orange_external_ipc"},
      {"camera_id", "2010093"},
      {"stream_id", "2010093_crop"},
      {"orange_declared_role", "sidecar"},
      {"video", video_path},
      {"metadata", metadata_path},
      {"keyframes", keyframes_path},
      {"summary", summary_path},
      {"frame_clock", "recording_frame_id"},
      {"video_pixel_coordinate_space", "crop_frame_pixels"},
      {"source_geometry_coordinate_space", "full_frame_pixels"},
      {"geometry_columns",
       {"crop_x", "crop_y", "crop_w", "crop_h", "detection_x",
        "detection_y", "detection_w", "detection_h"}},
      {"blank_frame_policy", "encode_black_frame_when_no_detection"},
      {"selection_policy", "largest_detection_by_confidence"},
      {"width", 256},
      {"height", 128},
      {"frame_count", 3},
      {"frame_rate", 100},
      {"codec", "hevc"},
      {"container", "mp4"},
      {"encoded_format", "nv12"},
      {"pixel_source_format", "mono8"},
  };
}

json FileDescriptor(const std::string& path) {
  return {{"path", path}, {"exists", true}, {"size_bytes", 1}};
}

std::string CsvPayload(const FixtureOptions& options) {
  const int second_crop_x = options.bad_blank_geometry ? 1 : 0;
  const int final_recording = options.bad_recording_id ? 4 : 3;
  std::string payload =
      "\"camera_frame_id\",recording_frame_id,local_frame_id,timestamp,"
      "timestamp_sys,has_detection,blank_frame,detection_confidence,crop_y,"
      "crop_x,crop_h,crop_w,detection_y,detection_x,detection_h,detection_w\n"
      "10,\"1\",100,1000,1001,1,0,0.75,200,100,256,512,264,228,64,128\n"
      "11,2,101,1010,1011,0,1,0,0," +
      std::to_string(second_crop_x) +
      ",0,0,0,0,0,0\n"
      "12," +
      std::to_string(final_recording) +
      ",102,1020,1021,1,0,0.8,300,400,128,256,332,464,32,64\n";
  if (options.missing_csv_column) {
    const size_t position = payload.find("detection_w");
    payload.replace(position, std::string("detection_w").size(),
                    "unknown_column");
  }
  return payload;
}

bool BuildFixture(const TemporaryDirectory& temporary,
                  const FixtureOptions& options,
                  FixturePaths* paths) {
  paths->recording_root = temporary.path() / "fixture_recording";
  paths->archive_root =
      paths->recording_root / "zarr/fixture_analysis.zarr";
  const std::string relative_video_path = "derived/crop/crop.mp4";
  const std::string relative_metadata_path = "derived/crop/crop_meta.csv";
  const std::string relative_keyframes_path =
      "derived/crop/crop_keyframes.json";
  const std::string relative_summary_path = "derived/crop/crop_summary.json";
  const std::string relative_status_path = "derived/crop/crop_status.json";
  const std::string foreign_prefix =
      "/groups/archive/" + paths->recording_root.filename().string() + "/";
  const std::string video_path = options.traversal_video
                                     ? "../outside.mp4"
                                 : options.foreign_absolute_paths
                                     ? foreign_prefix + relative_video_path
                                     : relative_video_path;
  const std::string metadata_path = options.foreign_absolute_paths
                                        ? foreign_prefix + relative_metadata_path
                                        : relative_metadata_path;
  const std::string keyframes_path = options.foreign_absolute_paths
                                         ? foreign_prefix + relative_keyframes_path
                                         : relative_keyframes_path;
  const std::string summary_path = options.foreign_absolute_paths
                                       ? foreign_prefix + relative_summary_path
                                       : relative_summary_path;
  const std::string status_path = options.foreign_absolute_paths
                                      ? foreign_prefix + relative_status_path
                                      : relative_status_path;
  paths->video = paths->recording_root / relative_video_path;
  paths->metadata = paths->recording_root / relative_metadata_path;

  json contract = CropContract(video_path, metadata_path, keyframes_path,
                               summary_path);
  json files = {
      {"video", FileDescriptor(video_path)},
      {"metadata",
       {{"path", metadata_path},
        {"exists", true},
        {"size_bytes", 1},
        {"data_row_count", options.imported_count_mismatch ? 4 : 3}}},
      {"keyframes", FileDescriptor(keyframes_path)},
      {"summary", FileDescriptor(summary_path)},
      {"status", FileDescriptor(status_path)},
  };
  json crop = {
      {"stream_key", "crop"},
      {"availability_status", options.crop_available ? "ok" : "missing"},
      {"required_missing", json::array()},
      {"warnings", json::array()},
      {"files", files},
      {"contract", contract},
      {"summary",
       {{"frames_received", 3}, {"frames_encoded", 3}, {"codec", "hevc"}}},
  };
  json root_crop = crop;
  if (options.root_child_mismatch) {
    root_crop["contract"]["width"] = 999;
  }
  json root = {
      {"schema_id", "palette.acquisition_video_streams.v1"},
      {"schema_version", 1},
      {"source_schema_id", "orange_runtime_video_streams_v1"},
      {"source_frame_clock", "recording_frame_id"},
      {"inventory_status", "ok"},
      {"stream_count", 2},
      {"stream_keys", {"crop", "full"}},
      {"crop_stream_available", options.crop_available},
      {"streams", {{"crop", root_crop}}},
  };

  CHECK(WriteGroupAttributes(paths->archive_root /
                                 "analysis/acquisition_video_streams",
                             root, options.zarr_v2));
  CHECK(WriteGroupAttributes(
      paths->archive_root /
          "analysis/acquisition_video_streams/streams/crop",
      crop, options.zarr_v2));
  if (!options.omit_video && !options.traversal_video) {
    CHECK(WriteText(paths->video, "video"));
  }
  CHECK(WriteText(paths->metadata, CsvPayload(options)));
  CHECK(WriteJson(paths->recording_root / relative_keyframes_path,
                  {{"codec", "hevc"},
                   {"fps", 100},
                   {"total_frames",
                    options.keyframe_count_mismatch ? 4 : 3},
                   {"keyframe_frames", {0, 1, 2}}}));
  CHECK(WriteJson(paths->recording_root / relative_summary_path,
                  {{"frames_received", 3},
                   {"frames_encoded", 3},
                   {"codec", "hevc"}}));
  if (!options.omit_status) {
    CHECK(WriteJson(paths->recording_root / relative_status_path,
                    {{"status", "completed"},
                     {"frames_received", 3},
                     {"frames_encoded", 3},
                     {"frames_dropped", 0}}));
  }
  return true;
}

bool OpensWithError(const FixtureOptions& options,
                    const std::string& expected_error) {
  TemporaryDirectory temporary;
  FixturePaths paths;
  CHECK(BuildFixture(temporary, options, &paths));
  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(paths.archive_root, &error);
  CHECK(archive != nullptr);
  CHECK(archive->rootPath().is_absolute());
  auto repository =
      crimson::zarr::OpenAcquisitionCropRepository(archive, &error);
  CHECK(repository == nullptr);
  CHECK(error.find(expected_error) != std::string::npos);
  return true;
}

bool TestSuccessfulFixture(const FixtureOptions& options) {
  TemporaryDirectory temporary;
  FixturePaths paths;
  CHECK(BuildFixture(temporary, options, &paths));

  std::string error;
  auto archive = crimson::zarr::ArchiveContext::Open(paths.archive_root, &error);
  CHECK(archive != nullptr);
  CHECK(archive->recordingRootPath() == paths.recording_root);
  CHECK(archive->resolveStoredPath("derived/crop/crop.mp4") == paths.video);
  const auto foreign = std::filesystem::path("/groups/archive") /
                       paths.recording_root.filename() /
                       "derived/crop/crop.mp4";
  CHECK(archive->resolveStoredPath(foreign) == paths.video);

  auto repository =
      crimson::zarr::OpenAcquisitionCropRepository(archive, &error);
  CHECK(repository != nullptr);
  CHECK(repository->cameraFrameCount() == 3);
  CHECK(repository->descriptor().schema_version == 1);
  CHECK(repository->descriptor().output_width == 256);
  CHECK(repository->descriptor().output_height == 128);
  CHECK(repository->descriptor().resolved_video_path == paths.video);
  CHECK(repository->descriptor().keyframe_count == 3);
  CHECK(repository->sourceCapabilities().live_geometry);
  CHECK(repository->sourceCapabilities().acquisition_video);
  CHECK(!repository->sourceCapabilities().persisted_zarr);

  const auto first = repository->resolveCameraFrame(0);
  CHECK(first.status == crimson::zarr::AcquisitionCropMappingStatus::Mapped);
  CHECK(first.video_frame == 0);
  CHECK(first.metadata_row == 0);
  CHECK(first.row.has_value());
  CHECK(first.row->recording_frame_id == 1);
  CHECK(first.row->camera_frame_id == 10);
  CHECK(first.row->full_frame_crop.width == 512.0);

  auto acquisition = repository->acquisitionFrameState(0, 0, 4512, 4512);
  CHECK(acquisition.resolved_camera_frame == 0);
  CHECK(acquisition.mapped_video_frame == 0);
  CHECK(acquisition.decoded_video_frame == 0);
  CHECK(acquisition.geometry.has_value());
  CHECK(acquisition.geometry->valid());
  const auto point = acquisition.geometry->fullFrameToCrop(
      crimson::crop::CropPoint{356.0, 328.0});
  CHECK(point.has_value());
  CHECK(point->x == 128.0);
  CHECK(point->y == 64.0);

  crimson::crop::CropFrameSourceState selection_state;
  selection_state.camera_frame = 0;
  selection_state.exact_full_frame = 0;
  selection_state.live_geometry = repository->liveGeometry(0, 4512, 4512);
  selection_state.acquisition = acquisition;
  auto selected = crimson::crop::SelectCropSource(
      repository->sourceCapabilities(), selection_state,
      crimson::crop::CropSourcePreference::PreferAcquisitionVideo,
      crimson::crop::CropFallbackPolicy::WaitForPreferred, 3);
  CHECK(selected.selected());
  CHECK(selected.source == crimson::crop::CropSourceKind::AcquisitionVideo);

  const auto blank = repository->acquisitionFrameState(1, 1, 4512, 4512);
  CHECK(blank.blank_frame);
  CHECK(blank.geometry.has_value());
  CHECK(blank.geometry->valid());
  CHECK(!blank.geometry->usableForLiveCrop());

  const auto too_small = repository->liveGeometry(2, 500, 500);
  CHECK(too_small.has_value());
  CHECK(!too_small->valid());
  CHECK(repository->resolveCameraFrame(-1).status ==
        crimson::zarr::AcquisitionCropMappingStatus::OutOfRange);
  CHECK(repository->resolveCameraFrame(3).status ==
        crimson::zarr::AcquisitionCropMappingStatus::OutOfRange);
  return true;
}

bool TestFailures() {
  {
    TemporaryDirectory temporary;
    const auto archive_path =
        temporary.path() / "empty_recording/zarr/empty.zarr";
    std::filesystem::create_directories(archive_path);
    std::string error;
    auto archive = crimson::zarr::ArchiveContext::Open(archive_path, &error);
    CHECK(archive != nullptr);
    auto repository =
        crimson::zarr::OpenAcquisitionCropRepository(archive, &error);
    CHECK(repository == nullptr);
    CHECK(error.find("no acquisition crop inventory") != std::string::npos);
  }

  FixtureOptions options;
  options.root_child_mismatch = true;
  CHECK(OpensWithError(options, "descriptors disagree"));

  options = {};
  options.crop_available = false;
  CHECK(OpensWithError(options, "not available"));

  options = {};
  options.omit_video = true;
  CHECK(OpensWithError(options, "video file does not exist"));

  options = {};
  options.traversal_video = true;
  CHECK(OpensWithError(options, "escapes recording root"));

  options = {};
  options.imported_count_mismatch = true;
  CHECK(OpensWithError(options, "row count disagrees"));

  options = {};
  options.keyframe_count_mismatch = true;
  CHECK(OpensWithError(options, "keyframe metadata disagrees"));

  options = {};
  options.bad_recording_id = true;
  CHECK(OpensWithError(options, "recording_frame_id is not row+1"));

  options = {};
  options.bad_blank_geometry = true;
  CHECK(OpensWithError(options, "Blank crop row has nonzero geometry"));

  options = {};
  options.missing_csv_column = true;
  CHECK(OpensWithError(options, "missing column 'detection_w'"));
  return true;
}

}  // namespace

int main() {
  FixtureOptions v3;
  FixtureOptions v2;
  v2.zarr_v2 = true;
  FixtureOptions foreign_without_status;
  foreign_without_status.foreign_absolute_paths = true;
  foreign_without_status.omit_status = true;
  if (!TestSuccessfulFixture(v3) || !TestSuccessfulFixture(v2) ||
      !TestSuccessfulFixture(foreign_without_status) ||
      !TestFailures()) {
    return 1;
  }
  std::cout << "acquisition_crop_repository_tests: PASS\n";
  return 0;
}
