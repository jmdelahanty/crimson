#include "platform/macos/apple_analysis_repository_loader.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                    \
      return false;                                                            \
    }                                                                          \
  } while (false)

class TemporaryArchive {
public:
  TemporaryArchive() {
    const auto seed =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("crimson-analysis-loader-" + std::to_string(seed));
    std::error_code error;
    std::filesystem::create_directories(path_, error);
    if (!error) {
      std::ofstream metadata(path_ / "zarr.json");
      metadata << R"({"zarr_format":3,"node_type":"group"})" << '\n';
    }
  }

  ~TemporaryArchive() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

bool testAsynchronousLifecycle() {
  TemporaryArchive archive;
  CHECK(std::filesystem::exists(archive.path() / "zarr.json"));

  AppleAnalysisRepositoryLoadRequest request;
  request.archive_path = archive.path().string();
  request.camera_frame_count = 100;
  request.subject_masks_enabled = false;
  request.subject_shapes_enabled = false;
  request.eye_geometry_enabled = false;
  request.motion_timeline_enabled = false;
  request.swim_bout_timeline_enabled = false;
  request.eye_angle_timeline_enabled = false;
  request.tail_kinematics_timeline_enabled = false;
  request.stimulus_context_timeline_enabled = false;

  AppleAnalysisRepositoryLoader loader;
  std::string error;
  const auto started = std::chrono::steady_clock::now();
  CHECK(loader.start(std::move(request), &error));
  CHECK(error.empty());
  CHECK(std::chrono::steady_clock::now() - started <
        std::chrono::milliseconds(100));

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  std::vector<AppleAnalysisRepositoryBundle> events;
  while (loader.loading() && std::chrono::steady_clock::now() < deadline) {
    while (auto ready = loader.takeReady()) {
      events.push_back(std::move(*ready));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  while (auto ready = loader.takeReady()) {
    events.push_back(std::move(*ready));
  }
  CHECK(!loader.loading());
  const auto progress = loader.progress();
  CHECK(!progress.running);
  CHECK(!progress.cancelled);
  CHECK(progress.state == crimson::loading::LoadingState::Ready);
  CHECK(progress.phase == "Analysis products ready");
  CHECK(progress.completed_products == 6);
  CHECK(progress.total_products == 6);
  CHECK(progress.products.size() == 6);
  CHECK(progress.fraction() == 1.0);

  CHECK(events.size() == 6);
  CHECK(events.front().archive != nullptr);
  CHECK(events.front().hasResultFor("archive"));
  std::set<std::string> products;
  for (const auto &event : events) {
    CHECK(event.archive != nullptr);
    CHECK(!event.cancelled);
    CHECK(event.timings.size() == 1);
    products.insert(event.timings.front().product);
  }
  CHECK(products == std::set<std::string>(
                        {"archive", "chaser_polar", "stimulus", "keypoints",
                         "crop_geometry", "acquisition_crop"}));
  CHECK(!loader.takeReady().has_value());
  return true;
}

bool testSharedSchedulerSurvivesLoaderClose() {
  TemporaryArchive archive;
  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(16, 2, 1);
  AppleAnalysisRepositoryLoadRequest request;
  request.archive_path = archive.path().string();
  request.scheduler = scheduler;
  request.subject_masks_enabled = false;
  request.subject_shapes_enabled = false;
  request.eye_geometry_enabled = false;
  request.motion_timeline_enabled = false;
  request.swim_bout_timeline_enabled = false;
  request.eye_angle_timeline_enabled = false;
  request.tail_kinematics_timeline_enabled = false;
  request.stimulus_context_timeline_enabled = false;
  AppleAnalysisRepositoryLoader loader;
  CHECK(loader.start(std::move(request)));
  loader.cancel();
  loader.close();
  CHECK(scheduler->running());
  scheduler->shutdown();
  return true;
}

bool testExplicitDetectionRunFailsClosed() {
  TemporaryArchive archive;
  AppleAnalysisRepositoryLoadRequest request;
  request.archive_path = archive.path().string();
  request.detection_run = "required_missing_run";
  request.camera_frame_count = 100;
  request.subject_masks_enabled = false;
  request.subject_shapes_enabled = false;
  request.eye_geometry_enabled = false;
  request.motion_timeline_enabled = false;
  request.swim_bout_timeline_enabled = false;
  request.eye_angle_timeline_enabled = false;
  request.tail_kinematics_timeline_enabled = false;
  request.stimulus_context_timeline_enabled = false;

  AppleAnalysisRepositoryLoader loader;
  CHECK(loader.start(std::move(request)));
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  std::vector<AppleAnalysisRepositoryBundle> events;
  while (loader.loading() && std::chrono::steady_clock::now() < deadline) {
    while (auto ready = loader.takeReady()) {
      events.push_back(std::move(*ready));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  while (auto ready = loader.takeReady()) {
    events.push_back(std::move(*ready));
  }

  CHECK(!loader.loading());
  CHECK(loader.progress().total_products == 7);
  CHECK(events.size() == 7);
  const auto detection =
      std::find_if(events.begin(), events.end(), [](const auto &event) {
        return event.hasResultFor("canonical_detection");
      });
  CHECK(detection != events.end());
  CHECK(detection->canonical_detection == nullptr);
  CHECK(!detection->errorFor("canonical_detection").empty());
  CHECK(detection->timings.size() == 1);
  CHECK(!detection->timings.front().available);
  return true;
}

} // namespace

int main() {
  if (!testAsynchronousLifecycle() ||
      !testSharedSchedulerSurvivesLoaderClose() ||
      !testExplicitDetectionRunFailsClosed()) {
    return 1;
  }
  std::cout << "apple_analysis_repository_loader_tests: PASS\n";
  return 0;
}
