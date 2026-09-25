#include "eye_geometry_overlay_buffer.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace crimson::zarr;
using namespace std::chrono_literals;
#define CHECK(value) do { if (!(value)) { \
  std::cerr << "CHECK failed: " #value << " at " << __LINE__ << '\n'; \
  return 1; } } while (false)

struct SharedState {
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<std::pair<int64_t, EyeGeometryFieldMask>> reads;
  bool blocking = false;
  bool entered = false;
  bool release = false;
};

class Repository final : public EyeGeometryOverlayRepository {
 public:
  explicit Repository(std::shared_ptr<SharedState> state) : state_(std::move(state)) {
    descriptor_.source_group = "analysis/eye_angle_runs";
    descriptor_.run_name = "demand_fixture";
    descriptor_.camera_frame_count = 20;
  }
  const EyeGeometryOverlayDescriptor &descriptor() const override {
    return descriptor_;
  }
  EyeGeometryOverlayResolution resolveCameraFrame(int64_t frame, int width,
                                                  int height) const override {
    return resolveCameraFrameFields(frame, width, height, EyeGeometryFields::All);
  }
  EyeGeometryOverlayResolution resolveCameraFrameFields(
      int64_t frame, int, int, EyeGeometryFieldMask fields,
      const std::function<bool()> & = {}) const override {
    if (frame == 9) throw std::runtime_error("synthetic reader exception");
    {
      std::unique_lock<std::mutex> lock(state_->mutex);
      state_->reads.emplace_back(frame, fields);
      if (frame == 5 && state_->blocking) {
        state_->entered = true;
        state_->condition.notify_all();
        state_->condition.wait(lock, [&] { return state_->release; });
      }
    }
    EyeGeometryOverlayResolution result;
    const bool fail = frame == 7 || (frame == 8 && (fields & EyeGeometryFields::RightGeometry));
    result.status = fail ? EyeGeometryOverlayStatus::ReadFailed
                         : EyeGeometryOverlayStatus::Mapped;
    result.camera_frame = frame;
    result.loaded_fields = fail ? 0 : fields;
    if (fail) result.error = "synthetic read failure";
    return result;
  }
 private:
  std::shared_ptr<SharedState> state_;
  EyeGeometryOverlayDescriptor descriptor_;
};

bool waitCoverage(EyeGeometryOverlayBuffer &buffer, int64_t frame,
                  EyeGeometryFieldMask fields) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto result = buffer.frame(frame);
    if (result && EyeGeometryFieldsCover(result->loaded_fields, fields))
      return true;
    std::this_thread::sleep_for(2ms);
  }
  return false;
}
} // namespace

int main() {
  auto state = std::make_shared<SharedState>();
  auto scheduler = std::make_shared<crimson::data::DataAccessScheduler>(32, 1);
  EyeGeometryOverlayBuffer buffer(scheduler, "fixture_archive");
  std::string error;
  CHECK(buffer.open(std::make_unique<Repository>(state), 0, 4, &error));
  const auto left = EyeGeometryFields::LeftGeometry;
  const auto right = EyeGeometryFields::RightGeometry;
  const auto body = EyeGeometryFields::BodyFrame;
  CHECK(buffer.requestFrames({{0, left}, {0, right}, {3, body}},
                             400, 200, false, &error));
  CHECK(waitCoverage(buffer, 0, left | right));
  CHECK(waitCoverage(buffer, 3, body));
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    CHECK(state->reads.size() == 2);
    CHECK(state->reads[0].first == 0);
    CHECK(state->reads[0].second == (left | right));
  }
  CHECK(buffer.requestFrames({{0, left | right | body}, {3, body}},
                             400, 200, false, &error));
  CHECK(waitCoverage(buffer, 0, left | right | body));
  size_t reads_after_upgrade;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    reads_after_upgrade = state->reads.size();
    CHECK(reads_after_upgrade == 3);
    CHECK(state->reads.back().second == (left | right | body));
  }
  CHECK(buffer.requestFrames({{0, left}, {3, body}},
                             400, 200, false, &error));
  scheduler->waitUntilIdle();
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    CHECK(state->reads.size() == reads_after_upgrade);
  }
  CHECK(buffer.requestFrames({}, 400, 200, false, &error));
  CHECK(!buffer.frame(0));

  buffer.close();
  CHECK(buffer.open(std::make_unique<Repository>(state), 2, 4, &error));

  CHECK(buffer.requestFrames({{10, left, true}, {15, right, false}},
                             400, 200, false, &error));
  CHECK(waitCoverage(buffer, 10, left));
  CHECK(waitCoverage(buffer, 15, right));
  CHECK(waitCoverage(buffer, 11, left));
  CHECK(waitCoverage(buffer, 12, left));
  scheduler->waitUntilIdle();
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    bool fetched_inspector_future = false;
    for (const auto &read : state->reads)
      fetched_inspector_future |= read.first == 16;
    CHECK(!fetched_inspector_future);
  }
  CHECK(buffer.requestFrames({}, 400, 200, false, &error));

  buffer.close();
  CHECK(buffer.open(std::make_unique<Repository>(state), 0, 4, &error));
  CHECK(buffer.requestFrames({{1, left}}, 400, 200, false, &error));
  CHECK(waitCoverage(buffer, 1, left));
  CHECK(buffer.requestFrame(1, 400, 200, false, &error));
  CHECK(waitCoverage(buffer, 1, EyeGeometryFields::All));
  CHECK(buffer.requestFrames({{7, left}}, 400, 200, false, &error));
  CHECK(buffer.waitForFrame(7, 3s));
  CHECK(buffer.frame(7)->status == EyeGeometryOverlayStatus::ReadFailed);
  CHECK(buffer.frame(7)->loaded_fields == 0);
  size_t failure_reads;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    failure_reads = state->reads.size();
  }
  CHECK(buffer.requestFrames({{7, left}}, 400, 200, false, &error));
  scheduler->waitUntilIdle();
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    CHECK(state->reads.size() == failure_reads);
  }
  CHECK(buffer.requestFrames({{7, left | right}}, 400, 200, false, &error));
  scheduler->waitUntilIdle();
  CHECK(buffer.frame(7)->loaded_fields == 0);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    CHECK(state->reads.size() == failure_reads + 1);
  }
  CHECK(buffer.requestFrames({{8, left}}, 400, 200, false, &error));
  CHECK(waitCoverage(buffer, 8, left));
  CHECK(buffer.requestFrames({{8, left | right}}, 400, 200, false, &error));
  scheduler->waitUntilIdle();
  CHECK(buffer.frame(8)->status == EyeGeometryOverlayStatus::Mapped);
  CHECK(buffer.frame(8)->loaded_fields == left);
  size_t reads_after_failed_upgrade;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    reads_after_failed_upgrade = state->reads.size();
  }
  CHECK(buffer.requestFrames({{8, left | right}}, 400, 200, false, &error));
  scheduler->waitUntilIdle();
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    CHECK(state->reads.size() == reads_after_failed_upgrade);
  }
  CHECK(buffer.requestFrames({{9, left}}, 400, 200, false, &error));
  CHECK(buffer.waitForFrame(9, 3s));
  CHECK(buffer.frame(9)->status == EyeGeometryOverlayStatus::ReadFailed);
  CHECK(buffer.frame(9)->loaded_fields == 0);
  CHECK(buffer.frame(9)->error == "synthetic reader exception");
  CHECK(buffer.requestFrames({}, 400, 200, false, &error));

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->blocking = true;
  }
  CHECK(buffer.requestFrames({{5, left}}, 400, 200, false, &error));
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    CHECK(state->condition.wait_for(lock, 3s, [&] { return state->entered; }));
  }
  CHECK(buffer.requestFrames({{6, right}}, 400, 200, true, &error));
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->release = true;
    state->condition.notify_all();
  }
  CHECK(waitCoverage(buffer, 6, right));
  CHECK(!buffer.frame(5));
  CHECK(buffer.metrics().discarded_results > 0);
  buffer.close();
  scheduler->shutdown();

  // A same-frame field upgrade may arrive while the first selective read is
  // active. The second request must eventually publish the union, and the
  // older partial completion must never replace that richer result.
  {
    auto upgrade_state = std::make_shared<SharedState>();
    upgrade_state->blocking = true;
    auto upgrade_scheduler =
        std::make_shared<crimson::data::DataAccessScheduler>(16, 1);
    EyeGeometryOverlayBuffer upgrade_buffer(upgrade_scheduler, "upgrade_archive");
    CHECK(upgrade_buffer.open(std::make_unique<Repository>(upgrade_state),
                              0, 4, &error));
    CHECK(upgrade_buffer.requestFrames({{5, left}}, 400, 200, false, &error));
    {
      std::unique_lock<std::mutex> lock(upgrade_state->mutex);
      CHECK(upgrade_state->condition.wait_for(
          lock, 3s, [&] { return upgrade_state->entered; }));
    }
    CHECK(upgrade_buffer.requestFrames({{5, left | right}}, 400, 200,
                                       false, &error));
    {
      std::lock_guard<std::mutex> lock(upgrade_state->mutex);
      upgrade_state->release = true;
      upgrade_state->condition.notify_all();
    }
    CHECK(waitCoverage(upgrade_buffer, 5, left | right));
    upgrade_scheduler->waitUntilIdle();
    CHECK(upgrade_buffer.frame(5));
    CHECK(EyeGeometryFieldsCover(upgrade_buffer.frame(5)->loaded_fields,
                                 left | right));
    {
      std::lock_guard<std::mutex> lock(upgrade_state->mutex);
      CHECK(upgrade_state->reads.size() == 2);
      CHECK(upgrade_state->reads[0].second == left);
      CHECK(upgrade_state->reads[1].second == (left | right));
    }
    upgrade_buffer.close();
    upgrade_scheduler->shutdown();
  }

  // Disabling the only consumer while its read is active must discard that
  // completion, even though the reader itself cannot interrupt the I/O.
  {
    auto disable_state = std::make_shared<SharedState>();
    disable_state->blocking = true;
    auto disable_scheduler =
        std::make_shared<crimson::data::DataAccessScheduler>(16, 1);
    EyeGeometryOverlayBuffer disable_buffer(disable_scheduler, "disable_archive");
    CHECK(disable_buffer.open(std::make_unique<Repository>(disable_state),
                              0, 4, &error));
    CHECK(disable_buffer.requestFrames({{5, left}}, 400, 200, false, &error));
    {
      std::unique_lock<std::mutex> lock(disable_state->mutex);
      CHECK(disable_state->condition.wait_for(
          lock, 3s, [&] { return disable_state->entered; }));
    }
    CHECK(disable_buffer.requestFrames({}, 0, 0, false, &error));
    {
      std::lock_guard<std::mutex> lock(disable_state->mutex);
      disable_state->release = true;
      disable_state->condition.notify_all();
    }
    disable_scheduler->waitUntilIdle();
    CHECK(!disable_buffer.frame(5));
    CHECK(disable_buffer.metrics().discarded_results > 0);
    disable_buffer.close();
    disable_scheduler->shutdown();
  }
  std::cout << "eye_geometry_demand_buffer_tests: PASS\n";
  return 0;
}
