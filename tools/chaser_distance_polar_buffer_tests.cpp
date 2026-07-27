#include "chaser_distance_polar_buffer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
using crimson::polar::ChaserDistancePolarAvailability;
using crimson::polar::ChaserDistancePolarBuffer;
using crimson::polar::ChaserDistancePolarColorProvenance;
using crimson::polar::ChaserDistancePolarDescriptor;
using crimson::polar::ChaserDistancePolarFrameSample;
using crimson::polar::ChaserDistancePolarPoint;
using crimson::polar::ChaserDistancePolarRepository;
using crimson::polar::ChaserDistancePolarSelectionProvenance;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__          \
                << ": " #condition << '\n';                                   \
      return false;                                                            \
    }                                                                          \
  } while (false)

ChaserDistancePolarDescriptor readyDescriptor() {
  ChaserDistancePolarDescriptor descriptor;
  descriptor.availability = ChaserDistancePolarAvailability::Ready;
  descriptor.provenance.run_name = "run";
  descriptor.provenance.component_name = "component";
  descriptor.provenance.run_selection =
      ChaserDistancePolarSelectionProvenance::LatestComplete;
  descriptor.provenance.component_selection =
      ChaserDistancePolarSelectionProvenance::LatestCompleted;
  descriptor.row_count = 1000;
  descriptor.chaser_count = 1;
  descriptor.coordinate_frame =
      std::string(crimson::polar::kArenaRelativeCanvasPixelFrame);
  descriptor.angle_convention =
      std::string(crimson::polar::kPositiveAnatomicalLeftAngleConvention);
  descriptor.dataset_global_max_distance_mm = 100.0;
  return crimson::polar::normalizeChaserDistancePolarDescriptor(
      std::move(descriptor));
}

ChaserDistancePolarPoint usablePoint(int64_t frame) {
  ChaserDistancePolarPoint point;
  point.chaser_index = 7;
  point.distance_mm = static_cast<double>(frame + 1);
  point.bearing_degrees = 15.0;
  point.valid = true;
  point.color.rgba = {0.25, 0.5, 0.75, 1.0};
  point.color.provenance =
      ChaserDistancePolarColorProvenance::StimulusProtocol;
  return point;
}

struct FakeState {
  std::atomic<int64_t> active_frame{-1};
};

class FakeRepository final : public ChaserDistancePolarRepository {
public:
  explicit FakeRepository(std::shared_ptr<FakeState> state = {},
                          std::chrono::milliseconds delay = 0ms)
      : descriptor_(readyDescriptor()),
        state_(std::move(state)),
        delay_(delay) {}

  const ChaserDistancePolarDescriptor& descriptor() const override {
    return descriptor_;
  }

  ChaserDistancePolarFrameSample resolveCameraFrame(
      int64_t camera_frame) const override {
    if (state_) {
      state_->active_frame.store(camera_frame);
    }
    if (delay_.count() > 0) {
      std::this_thread::sleep_for(delay_);
    }
    if (camera_frame == 1) {
      return crimson::polar::makeChaserDistancePolarFrameSample(
          descriptor_, camera_frame, camera_frame, {});
    }
    if (camera_frame == 2) {
      return crimson::polar::makeChaserDistancePolarFrameSample(
          descriptor_, camera_frame, std::nullopt, {});
    }
    if (camera_frame == 3 || camera_frame == 4) {
      auto unavailable = descriptor_;
      unavailable.availability =
          camera_frame == 3
              ? ChaserDistancePolarAvailability::DatasetUnavailable
              : ChaserDistancePolarAvailability::UnsupportedMetadata;
      unavailable.error = camera_frame == 3 ? "unavailable" : "unsupported";
      return crimson::polar::makeChaserDistancePolarFrameSample(
          unavailable, camera_frame, std::nullopt, {});
    }
    if (camera_frame == 5) {
      return crimson::polar::makeChaserDistancePolarFrameSample(
          descriptor_, camera_frame, camera_frame, {}, "read failed");
    }
    return crimson::polar::makeChaserDistancePolarFrameSample(
        descriptor_, camera_frame, camera_frame, {usablePoint(camera_frame)});
  }

private:
  ChaserDistancePolarDescriptor descriptor_;
  std::shared_ptr<FakeState> state_;
  std::chrono::milliseconds delay_;
};

bool testOpenValidation() {
  ChaserDistancePolarBuffer buffer;
  std::string error;
  CHECK(!buffer.open(nullptr, 2, 3, &error));
  CHECK(!error.empty());
  CHECK(!buffer.open(std::make_unique<FakeRepository>(), 2, 0, &error));
  CHECK(!error.empty());
  CHECK(!buffer.requestFrame(0, false, &error));
  CHECK(!buffer.isOpen());
  return true;
}

bool testAvailabilityMetricsAndBounds() {
  ChaserDistancePolarBuffer buffer;
  std::string error;
  CHECK(buffer.open(std::make_unique<FakeRepository>(), 8, 3, &error));
  CHECK(buffer.isOpen());
  CHECK(buffer.descriptor().ready());

  for (int64_t frame = 0; frame <= 5; ++frame) {
    CHECK(buffer.requestFrame(frame, true, &error));
    CHECK(buffer.waitForFrame(frame, 2s));
    const auto sample = buffer.frame(frame);
    CHECK(sample != nullptr);
  }
  CHECK(buffer.requestFrame(0, true, &error));
  CHECK(buffer.waitForFrame(0, 2s));
  CHECK(buffer.requestFrame(0, false, &error));

  const auto metrics = buffer.metrics();
  CHECK(metrics.requests == 8);
  CHECK(metrics.cache_hits >= 1);
  CHECK(metrics.ready_frames >= 2);
  CHECK(metrics.empty_frames >= 1);
  CHECK(metrics.missing_frames >= 1);
  CHECK(metrics.unavailable_frames >= 1);
  CHECK(metrics.unsupported_frames >= 1);
  CHECK(metrics.failed_frames >= 1);
  CHECK(metrics.source_points >= metrics.published_points);
  CHECK(metrics.peak_cached_frames <= 3);
  CHECK(metrics.peak_pending_frames <= 3);
  buffer.close();
  CHECK(!buffer.isOpen());
  return true;
}

bool testDiscontinuityDiscardsOldGeneration() {
  auto state = std::make_shared<FakeState>();
  ChaserDistancePolarBuffer buffer;
  CHECK(buffer.open(std::make_unique<FakeRepository>(state, 60ms), 2, 4));
  CHECK(buffer.requestFrame(10));
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (state->active_frame.load() != 10 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  CHECK(state->active_frame.load() == 10);
  CHECK(buffer.requestFrame(100, true));
  CHECK(buffer.waitForFrame(100, 3s));
  CHECK(buffer.frame(10) == nullptr);
  CHECK(buffer.frame(100) != nullptr);
  CHECK(buffer.metrics().discarded_results >= 1);
  return true;
}

}  // namespace

int main() {
  if (!testOpenValidation() || !testAvailabilityMetricsAndBounds() ||
      !testDiscontinuityDiscardsOldGeneration()) {
    return 1;
  }
  std::cout << "chaser_distance_polar_buffer_tests: PASS\n";
  return 0;
}
