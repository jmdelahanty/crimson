#include "platform/macos/apple_acquisition_crop_playback_session.h"
#include "zarr/acquisition_crop_repository.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

struct TestFailure {
  std::string message;
};

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      throw TestFailure{std::string("CHECK failed: ") + #condition +          \
                        " at " + __FILE__ + ":" +                             \
                        std::to_string(__LINE__)};                              \
    }                                                                          \
  } while (false)

struct RepositoryOptions {
  int output_width = 64;
  int64_t descriptor_frame_count = 12;
  int64_t row_count = 12;
  double frame_rate = 10.0;
  bool invalid_geometry = false;
  bool include_resolved_video_path = true;
};

std::unique_ptr<crimson::zarr::AcquisitionCropRepository> MakeRepository(
    const std::string& video_path,
    const RepositoryOptions& options = {}) {
  crimson::zarr::AcquisitionCropStreamDescriptor descriptor;
  descriptor.schema_id = "palette.acquisition_video_streams.v1";
  descriptor.schema_version = 1;
  descriptor.stream_key = "crop";
  descriptor.stream_id = "fixture_crop";
  descriptor.camera_id = "fixture_camera";
  descriptor.frame_clock = "recording_frame_id";
  descriptor.output_width = options.output_width;
  descriptor.output_height = 48;
  descriptor.frame_count = options.descriptor_frame_count;
  descriptor.frame_rate = options.frame_rate;
  descriptor.codec = "prores";
  descriptor.container = "mov";
  if (options.include_resolved_video_path) {
    descriptor.resolved_video_path = video_path;
  }

  std::vector<crimson::zarr::AcquisitionCropFrameRow> rows;
  for (int64_t frame = 0; frame < options.row_count; ++frame) {
    crimson::zarr::AcquisitionCropFrameRow row;
    row.recording_frame_id = frame + 1;
    row.local_frame_id = 100 + frame;
    row.camera_frame_id = 200 + frame;
    row.timestamp = frame * 100;
    row.timestamp_sys = frame * 100 + 1;
    row.blank_frame = frame == 5;
    row.has_detection = !row.blank_frame;
    row.detection_confidence = row.blank_frame ? 0.0 : 0.8;
    if (!row.blank_frame) {
      row.full_frame_crop = {
          options.invalid_geometry && frame == 3 ? 100.0 : 0.0,
          0.0, 128.0, 96.0};
      row.full_frame_detection = crimson::crop::CropRect{
          32.0, 24.0, 32.0, 24.0};
    }
    rows.push_back(std::move(row));
  }
  return crimson::zarr::MakeAcquisitionCropRepository(
      std::move(descriptor), std::move(rows));
}

AppleAlignedAcquisitionCropFrame RequestAndCheck(
    AppleAcquisitionCropPlaybackSession& session,
    int64_t camera_frame,
    bool discontinuity,
    std::string* error) {
  CHECK(session.requestCameraFrame(camera_frame, discontinuity, error));
  CHECK(session.waitForCameraFrame(camera_frame, std::chrono::seconds(5),
                                   error));
  auto aligned = session.frameForCameraFrame(camera_frame);
  CHECK(aligned.has_value());
  CHECK(aligned->resolution.video_frame == camera_frame);
  CHECK(aligned->resolution.metadata_row == camera_frame);
  CHECK(aligned->resolution.row.has_value());
  CHECK(aligned->resolution.row->recording_frame_id == camera_frame + 1);
  CHECK(aligned->decoded_frame.metadata.stream_id == "crop");
  CHECK(aligned->decoded_frame.metadata.frame_number == camera_frame);
  CHECK(aligned->decoded_frame.metadata.local_frame_number == camera_frame);
  CHECK(aligned->decoded_frame.surface != nullptr);
  CHECK(aligned->source_state.resolved_camera_frame == camera_frame);
  CHECK(aligned->source_state.mapped_video_frame == camera_frame);
  CHECK(aligned->source_state.decoded_video_frame == camera_frame);
  CHECK(aligned->selection.selected());
  CHECK(aligned->selection.source ==
        crimson::crop::CropSourceKind::AcquisitionVideo);
  CHECK(aligned->selection.source_frame_index == camera_frame);
  CHECK(aligned->selection.blank_frame == (camera_frame == 5));
  CHECK(aligned->source_state.geometry.has_value());
  CHECK(aligned->source_state.geometry->source_width == 128);
  CHECK(aligned->source_state.geometry->source_height == 96);
  CHECK(aligned->source_state.geometry->output_width == 64);
  CHECK(aligned->source_state.geometry->output_height == 48);
  return std::move(*aligned);
}

void TestOpenValidation(const std::string& video_path) {
  std::string error;
  AppleAcquisitionCropPlaybackSession session;
  CHECK(!session.open(nullptr, 128, 96, 4, &error));
  CHECK(error == "acquisition crop repository is null");
  CHECK(!session.open(MakeRepository(video_path), 0, 96, 4, &error));
  CHECK(error == "full-frame dimensions must be positive");
  CHECK(!session.open(MakeRepository(video_path), 128, 96, 1, &error));
  CHECK(error == "playback buffer capacity must be at least two");

  RepositoryOptions options;
  options.descriptor_frame_count = 11;
  CHECK(!session.open(MakeRepository(video_path, options), 128, 96, 4,
                      &error));
  CHECK(error == "acquisition crop repository descriptor is inconsistent");

  options = {};
  options.invalid_geometry = true;
  CHECK(!session.open(MakeRepository(video_path, options), 128, 96, 4,
                      &error));
  CHECK(error.find("geometry is invalid") != std::string::npos);

  options = {};
  options.include_resolved_video_path = false;
  CHECK(!session.open(MakeRepository(video_path, options), 128, 96, 4,
                      &error));
  CHECK(error ==
        "acquisition crop repository has no resolved video path");

  options = {};
  options.output_width = 65;
  CHECK(!session.open(MakeRepository(video_path, options), 128, 96, 4,
                      &error));
  CHECK(error == "acquisition crop video disagrees with repository contract");

  options = {};
  options.descriptor_frame_count = 11;
  options.row_count = 11;
  CHECK(!session.open(MakeRepository(video_path, options), 128, 96, 4,
                      &error));
  CHECK(error == "acquisition crop video disagrees with repository contract");

  options = {};
  options.frame_rate = 11.0;
  CHECK(!session.open(MakeRepository(video_path, options), 128, 96, 4,
                      &error));
  CHECK(error == "acquisition crop video disagrees with repository contract");
  CHECK(!session.isOpen());
}

void RunTest(const std::string& video_path) {
  TestOpenValidation(video_path);

  std::string error;
  AppleAcquisitionCropPlaybackSession session;
  CHECK(session.open(MakeRepository(video_path), 128, 96, 4, &error));
  CHECK(session.isOpen());
  CHECK(session.info().stream_id == "crop");
  CHECK(session.info().width == 64);
  CHECK(session.info().height == 48);
  CHECK(session.info().frame_count == 12);
  CHECK(std::fabs(session.info().nominal_frame_rate - 10.0) < 0.01);
  CHECK(session.fullFrameWidth() == 128);
  CHECK(session.fullFrameHeight() == 96);

  auto first = RequestAndCheck(session, 0, true, &error);
  const auto retained_surface = first.decoded_frame.surface;
  const uintptr_t retained_handle = retained_surface->nativeHandle();
  CHECK(retained_handle != 0);
  RequestAndCheck(session, 1, false, &error);
  RequestAndCheck(session, 2, false, &error);
  RequestAndCheck(session, 2, false, &error);

  auto blank = RequestAndCheck(session, 5, true, &error);
  CHECK(blank.resolution.row->blank_frame);
  CHECK(blank.source_state.blank_frame);
  CHECK(blank.source_state.geometry->valid());
  CHECK(!blank.source_state.geometry->usableForLiveCrop());

  RequestAndCheck(session, 1, false, &error);
  RequestAndCheck(session, 10, true, &error);
  CHECK(retained_surface->nativeHandle() == retained_handle);

  CHECK(session.requestCameraFrame(-1, true, &error));
  CHECK(session.resolveCameraFrame(-1).status ==
        crimson::zarr::AcquisitionCropMappingStatus::OutOfRange);
  CHECK(!session.waitForCameraFrame(-1, std::chrono::milliseconds(1), &error));
  CHECK(!session.frameForCameraFrame(-1));
  CHECK(session.requestCameraFrame(12, true, &error));
  CHECK(session.resolveCameraFrame(12).status ==
        crimson::zarr::AcquisitionCropMappingStatus::OutOfRange);
  CHECK(!session.frameForCameraFrame(12));

  session.suspend();
  CHECK(session.isOpen());
  CHECK(session.metrics().decoder.buffered_frames == 0);
  RequestAndCheck(session, 3, true, &error);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const auto metrics = session.metrics();
  CHECK(metrics.camera_requests == 10);
  CHECK(metrics.mapped_requests == 8);
  CHECK(metrics.out_of_range_requests == 2);
  CHECK(metrics.hold_requests > 0);
  CHECK(metrics.seek_requests > 0);
  CHECK(metrics.failed_requests == 0);
  CHECK(metrics.decoder.buffered_frames <= 4);
  CHECK(metrics.decoder.peak_buffered_frames <= 4);
  CHECK(metrics.decoder.last_error.empty());

  std::cout << "[AppleAcquisitionCropPlayback] PASS requests="
            << metrics.camera_requests << " seeks=" << metrics.seek_requests
            << " follows=" << metrics.follow_requests
            << " holds=" << metrics.hold_requests
            << " decoded=" << metrics.decoder.decoded_frames
            << " peak_buffered=" << metrics.decoder.peak_buffered_frames
            << '\n';
  session.close();
  CHECK(!session.isOpen());
}

}  // namespace

int main(int argc, char** argv) {
  @autoreleasepool {
    if (argc != 2) {
      std::cerr << "usage: apple_acquisition_crop_playback_tests VIDEO_FIXTURE\n";
      return 2;
    }
    try {
      RunTest(argv[1]);
    } catch (const TestFailure& failure) {
      std::cerr << failure.message << std::endl;
      return 1;
    } catch (const std::exception& exception) {
      std::cerr << "unexpected exception: " << exception.what() << std::endl;
      return 1;
    }
  }
  return 0;
}
